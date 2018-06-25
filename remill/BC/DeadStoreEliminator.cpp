/*
 * Copyright (c) 2017 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glog/logging.h>
#include <gflags/gflags.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Module.h>

#include "remill/Arch/Arch.h"
#include "remill/BC/DeadStoreEliminator.h"
#include "remill/BC/Util.h"
#include "remill/BC/Compat/Local.h"
#include "remill/OS/FileSystem.h"

DEFINE_string(dot_output_dir, "",
              "The directory in which to log DOT digraphs of the alias "
              "analysis information derived during the process of "
              "eliminating dead stores");

DEFINE_bool(disable_register_forwarding, false,
            "Whether or not register forwarding should be enabled "
            "to perform load-to-load and load-to-store forwarding "
            "to eliminate dead instructions more aggressively");

namespace remill {
namespace {

using ValueToOffset = std::unordered_map<llvm::Value *, uint64_t>;
using InstToOffset = std::unordered_map<llvm::Instruction *, uint64_t>;
using ScopeToOffset = std::unordered_map<llvm::MDNode *, uint64_t>;
using LiveSet = std::bitset<256>;
using InstToLiveSet = std::unordered_map<llvm::Instruction *, LiveSet>;

// Struct to keep track of how murderous the dead store eliminator is.
struct KillCounter {
  uint64_t dead_stores;
  uint64_t removed_insts;
  uint64_t fwd_loads;
  uint64_t fwd_stores;
  uint64_t fwd_perfect;
  uint64_t fwd_truncated;
  uint64_t fwd_casted;
  uint64_t fwd_reordered;
  uint64_t fwd_failed;
};

// Return true if the given function is a lifted function
// (and not the `__remill_basic_block`).
static bool IsLiftedFunction(llvm::Function *func,
                             const llvm::Function *bb_func) {
  return !(func == bb_func ||
           func->isDeclaration() ||
           func->getFunctionType() != bb_func->getFunctionType());
}

// Recursive visitor of the `State` structure that assigns slots of ranges of
// bytes.
class StateVisitor {
 public:
  StateVisitor(llvm::DataLayout *dl_, uint64_t num_bytes);
  virtual ~StateVisitor(void) = default;

  // Visit a type and record it (and any children) in the slots vector
  virtual void Visit(llvm::Type *ty);

  std::vector<StateSlot> offset_to_slot;

  // The current index in the state structure.
  uint64_t index;

  // The current offset in the state structure.
  uint64_t offset;

 private:
  // Used for calculating type allocation size.
  llvm::DataLayout *dl;
};

StateVisitor::StateVisitor(llvm::DataLayout *dl_, uint64_t num_bytes)
    : offset_to_slot(),
      index(0),
      offset(0),
      dl(dl_) {
  offset_to_slot.reserve(num_bytes);
}

// Update the `StateVisitor`s slots field to hold a StateSlot for every byte
// offset into the state. The `StateSlot` element is the same across each byte
// offset that is within the element's begin offset and end offset.
void StateVisitor::Visit(llvm::Type *ty) {
  if (!ty) {  // TODO(tim): Is this even possible?
    LOG(FATAL)
        << "NULL type in `State` structure.";

  // Structure, class, or union.
  } else if (auto struct_ty = llvm::dyn_cast<llvm::StructType>(ty)) {
    for (auto elem_ty : struct_ty->elements()) {
      Visit(elem_ty);
    }

  // Array or vector.
  } else if (auto seq_ty = llvm::dyn_cast<llvm::SequentialType>(ty)) {
    auto first_ty = seq_ty->getElementType();

    // Special case: sequences of primitive types (or vectors thereof) are
    // treated as one slot.
    if (first_ty->isIntegerTy() || first_ty->isFloatingPointTy()) {
      uint64_t len = dl->getTypeAllocSize(seq_ty);
      for (uint64_t i = 0; i < len; i++) {
        offset_to_slot.emplace_back(index, offset, len);
      }
      index++;
      offset += len;

    // This is an array of non-primitive types.
    } else {
      for (unsigned int i = 0; i < seq_ty->getNumElements(); i++) {
        // NOTE(tim): Recalculates every time, rather than memoizing.
        Visit(first_ty);
      }
    }

  // Primitive type.
  } else if (ty->isIntegerTy() || ty->isFloatingPointTy()) {
    uint64_t len = dl->getTypeAllocSize(ty);
    for (uint64_t i = 0; i < len; i++) {
      offset_to_slot.emplace_back(index, offset, len);
    }
    index++;
    offset += len;

  } else {
    LOG(FATAL)
        << "Unexpected type `" << LLVMThingToString(ty)
        << "` in state structure";
  }
}

// Try to get the offset associated with some value.
static bool TryGetOffset(llvm::Value *val,
                         const ValueToOffset &state_offset,
                         uint64_t *offset_out) {
  auto ptr = state_offset.find(val);
  if (ptr != state_offset.end()) {
    *offset_out = ptr->second;
    return true;

  } else {
    return false;
  }
}

// Try to get the offset associated with some value, or if the value is
// a constant integer, get that instead.
static bool TryGetOffsetOrConst(
    llvm::Value *val, const ValueToOffset &state_offset,
    uint64_t *offset_out) {
  if (auto const_val = llvm::dyn_cast<llvm::ConstantInt>(val)) {
    *offset_out = static_cast<uint64_t>(const_val->getSExtValue());
    return true;
  } else {
    return TryGetOffset(val, state_offset, offset_out);
  }
}

enum class VisitResult : int {
  Progress,
  NoProgress,
  Incomplete,
  Ignored,
  Error
};

enum class OpType : int {
  Plus,
  Minus,
};

// Add or subtract `lhs_offset` and `rhs_offset`, and do bounds checking.
static bool TryCombineOffsets(uint64_t lhs_offset, OpType op_type,
                              uint64_t rhs_offset, uint64_t max_offset,
                              uint64_t *out_offset) {
  int64_t signed_result = 0;
  switch (op_type) {
    case OpType::Plus:
      signed_result = static_cast<int64_t>(lhs_offset) +
                      static_cast<int64_t>(rhs_offset);
      break;
    case OpType::Minus:
      signed_result = static_cast<int64_t>(lhs_offset) -
                      static_cast<int64_t>(rhs_offset);
      break;
  }

  *out_offset = static_cast<uint64_t>(signed_result);
  return (*out_offset) < max_offset;
}

// Return the scope of the given instruction.
static llvm::MDNode *GetScopeFromInst(llvm::Instruction &inst) {
  return inst.getMetadata(llvm::LLVMContext::MD_alias_scope);
}

static LiveSet GetLiveSetFromArgs(llvm::iterator_range<llvm::Use *> args,
                                  const ValueToOffset &val_to_offset,
                                  const std::vector<StateSlot> &state_slots) {
  LiveSet live;
  for (auto &arg_it : args) {
    auto arg = arg_it->stripPointerCasts();
    const auto offset_it = val_to_offset.find(arg);
    if (offset_it != val_to_offset.end()) {
      const auto offset = offset_it->second;

      // If we access a single non-zero offset, mark just that offset.
      if (offset != 0) {
        live.set(state_slots[offset].index);

      // If we access offset `0`, then maybe we're actually passing
      // a state pointer, in which anything can be changed, so we want
      // to treat everything as live, OR maybe we're passing a pointer
      // to the first thing in the `State` structure, which would be
      // rare and unusual.
      } else {
        live.set();
      }
    }
  }
  return live;
}

// Visits instructions and propagates information about where in the
// `State` structure a given instruction might reference.
struct ForwardAliasVisitor
    : public llvm::InstVisitor<ForwardAliasVisitor, VisitResult> {
 public:
  virtual ~ForwardAliasVisitor(void) = default;

  ForwardAliasVisitor(const llvm::DataLayout &dl_,
                      const std::vector<StateSlot> &offset_to_slot_,
                      InstToLiveSet &live_args_);

  bool Analyze(llvm::Function *func);

 protected:
  friend class llvm::InstVisitor<ForwardAliasVisitor, VisitResult>;

  virtual VisitResult visitInstruction(llvm::Instruction &I);
  virtual VisitResult visitAllocaInst(llvm::AllocaInst &I);
  virtual VisitResult visitLoadInst(llvm::LoadInst &inst);
  virtual VisitResult visitStoreInst(llvm::StoreInst &inst);
  virtual VisitResult visitGetElementPtrInst(llvm::GetElementPtrInst &inst);
  virtual VisitResult visitCastInst(llvm::CastInst &inst);
  virtual VisitResult visitAdd(llvm::BinaryOperator &inst);
  virtual VisitResult visitSub(llvm::BinaryOperator &I);
  virtual VisitResult visitSelect(llvm::SelectInst &inst);
  virtual VisitResult visitPHINode(llvm::PHINode &I);
  virtual VisitResult visitCallInst(llvm::CallInst &inst);
  virtual VisitResult visitInvokeInst(llvm::InvokeInst &inst);

 private:
  void AddInstruction(llvm::Instruction *inst);
  virtual VisitResult visitBinaryOp_(llvm::BinaryOperator &inst, OpType op);

 public:
  const llvm::DataLayout dl;
  const std::vector<StateSlot> &offset_to_slots;
  ValueToOffset state_offset;
  InstToOffset state_access_offset;
  InstToLiveSet &live_args;
  std::unordered_set<llvm::Value *> exclude;
  std::vector<llvm::Instruction *> curr_wl;
  std::vector<llvm::Instruction *> calls;
  llvm::Value *state_ptr;
};

ForwardAliasVisitor::ForwardAliasVisitor(
    const llvm::DataLayout &dl_,
    const std::vector<StateSlot> &offset_to_slot_,
    InstToLiveSet &live_args_)
    : dl(dl_),
      offset_to_slots(offset_to_slot_),
      state_offset(),
      state_access_offset(),
      live_args(live_args_),
      exclude(),
      curr_wl(),
      state_ptr(nullptr) {}

void ForwardAliasVisitor::AddInstruction(llvm::Instruction *inst) {
  inst->setName("");

  if (auto store_inst = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    llvm::AAMDNodes aamd;
    store_inst->setAAMetadata(aamd);
    curr_wl.push_back(inst);

  } else if (auto load_inst = llvm::dyn_cast<llvm::LoadInst>(inst)) {
    llvm::AAMDNodes aamd;
    load_inst->setAAMetadata(aamd);
    curr_wl.push_back(inst);

  } else if (llvm::isa<llvm::CallInst>(inst) ||
             llvm::isa<llvm::InvokeInst>(inst)) {

    calls.push_back(inst);
  } else {
    curr_wl.push_back(inst);
  }
}

// Iterate through the current worklist, updating the `state_offset` and
// `state_access_offset` according to the instructions in the list. Any
// instruction that is not currently interpretable (some of its pointers
// are not yet in `state_offset`) is withheld to the next analysis round
// in the next worklist. Analysis repeats until the current worklist is
// empty or until an error condition is hit.
bool ForwardAliasVisitor::Analyze(llvm::Function *func) {
  curr_wl.clear();
  exclude.clear();
  calls.clear();
  state_access_offset.clear();
  state_offset.clear();

  state_ptr = LoadStatePointer(func);
  if (!state_ptr) {
    return false;
  }

  state_offset.emplace(state_ptr, 0);

  for (auto &block : *func) {
    for (auto &inst : block) {
      AddInstruction(&inst);
    }
  }

  std::vector<llvm::Instruction *> next_wl;
  std::vector<llvm::Instruction *> pending_wl;
  next_wl.reserve(curr_wl.size());

  bool progress = true;
  bool bump = false;

  while (!curr_wl.empty() && (progress || bump)) {
    curr_wl.insert(curr_wl.end(), pending_wl.begin(), pending_wl.end());
    pending_wl.clear();

    progress = false;

    for (auto inst : curr_wl) {
      switch (visit(inst)) {
        case VisitResult::Progress:
          progress = true;
          break;
        case VisitResult::Incomplete:
          pending_wl.push_back(inst);
          break;
        case VisitResult::NoProgress:
          next_wl.push_back(inst);
          break;
        case VisitResult::Ignored:
          break;
        case VisitResult::Error:
          return false;
      }
    }

    curr_wl.swap(next_wl);
    next_wl.clear();

    if (progress || bump) {
      bump = false;
    } else if (!pending_wl.empty()) {
      bump = true;
    }
  }

  for (auto inst : calls) {
    visit(inst);
  }

  // TODO(tim): This condition is triggered a lot.
  if (!pending_wl.empty()) {
    DLOG(ERROR)
        << "Alias analysis failed to complete on function `"
        << func->getName().str() << "` with " << curr_wl.size()
        << " instructions in the worklist and " << pending_wl.size()
        << " incomplete but no progress made in the last"
        << " iteration";
  }

  return true;
}

VisitResult ForwardAliasVisitor::visitInstruction(llvm::Instruction &I) {
  return VisitResult::Ignored;
}

VisitResult ForwardAliasVisitor::visitAllocaInst(llvm::AllocaInst &I) {
  exclude.insert(&I);
  return VisitResult::Progress;
}

// Visit a load instruction and update the alias map.
VisitResult ForwardAliasVisitor::visitLoadInst(llvm::LoadInst &inst) {
  auto val = inst.getPointerOperand();

  // Special case: loaded value is itself a `State` pointer. Not sure if
  // this ever comes up, but if it does then we want to treat all `State`
  // structures as aliasing.
  if (inst.getType() == state_ptr->getType()) {
    state_offset.emplace(&inst, 0);
    return VisitResult::Progress;

  } else if (exclude.count(val)) {
    exclude.insert(&inst);
    return VisitResult::Progress;

  } else {
    auto ptr = state_offset.find(val);
    if (ptr == state_offset.end()) {
      return VisitResult::NoProgress;

    // The `State` structure doesn't contain pointers, so loaded values
    // should not be used to index elsewhere into `State`. Technically,
    // this could happen where an index into a vector register is stored
    // in another register. We don't handle that yet.
    } else {
      exclude.insert(&inst);
      state_access_offset.emplace(&inst, ptr->second);
      return VisitResult::Progress;
    }
  }
}

// Visit a `store` instruction and update the alias map.
VisitResult ForwardAliasVisitor::visitStoreInst(llvm::StoreInst &inst) {

  // If we're storing a pointer into the `State` structure into the `State`
  // structure then just bail out because that shouldn't even be possible
  // and is not allowed by the Remill design.
  if (state_offset.count(inst.getOperand(0))) {
    return VisitResult::Error;
  }

  auto addr = inst.getPointerOperand();
  if (exclude.count(addr)) {
    exclude.insert(&inst);
    return VisitResult::Progress;
  }

  auto ptr = state_offset.find(addr);
  if (ptr == state_offset.end()) {
    return VisitResult::NoProgress;
  }

  // loads mean we now have an alias to the pointer
  state_access_offset.emplace(&inst, ptr->second);
  return VisitResult::Progress;
}

// Visit a `getelementptr` (GEP) instruction and update the offset map.
VisitResult ForwardAliasVisitor::visitGetElementPtrInst(
    llvm::GetElementPtrInst &inst) {

  auto val = inst.getPointerOperand();

  if (exclude.count(val)) {
    exclude.insert(&inst);
    return VisitResult::Progress;
  }

  auto ptr = state_offset.find(val);
  if (ptr == state_offset.end()) {
    return VisitResult::NoProgress;
  }

  // Try to get the offset as a single constant. If we can't then
  llvm::APInt const_offset(64, 0);
  if (!inst.accumulateConstantOffset(dl, const_offset)) {
    return VisitResult::Error;
  }

  // the final offset (adding the ptr->second value to the const_offset)
  uint64_t offset = 0;
  if (!TryCombineOffsets(ptr->second, OpType::Plus,
                         static_cast<uint64_t>(const_offset.getSExtValue()),
                         offset_to_slots.size(), &offset)) {
    LOG(WARNING)
        << "Out of bounds GEP operation: " << LLVMThingToString(&inst)
        << " with inferred offset " << static_cast<int64_t>(offset)
        << " and max allowed offset of " << offset_to_slots.size();
    return VisitResult::Error;
  }

  state_offset.emplace(&inst, offset);
  return VisitResult::Progress;
}

// Visit a cast instruction and update the offset map. This could be
// a `bitcast`, `inttoptr`, `ptrtoint`, etc.
VisitResult ForwardAliasVisitor::visitCastInst(llvm::CastInst &inst) {
  auto addr = inst.getOperand(0);
  if (exclude.count(addr)) {
    exclude.insert(&inst);
    return VisitResult::Progress;
  }

  auto ptr = state_offset.find(addr);
  if (ptr == state_offset.end()) {
    return VisitResult::NoProgress;

  } else {
    state_offset.emplace(&inst, ptr->second);
    return VisitResult::Progress;
  }
}

// Visit an `add` instruction and update the offset map.
VisitResult ForwardAliasVisitor::visitAdd(llvm::BinaryOperator &inst) {
  return ForwardAliasVisitor::visitBinaryOp_(inst, OpType::Plus);
}

// Visit a `sub` instruction and update the offset map.
VisitResult ForwardAliasVisitor::visitSub(llvm::BinaryOperator &inst) {
  return ForwardAliasVisitor::visitBinaryOp_(inst, OpType::Minus);
}

// Visit an `add` or `sub` instruction.
VisitResult ForwardAliasVisitor::visitBinaryOp_(
    llvm::BinaryOperator &inst, OpType op) {

  auto lhs_val = inst.getOperand(0);
  auto rhs_val = inst.getOperand(1);
  if (exclude.count(lhs_val) || exclude.count(rhs_val)) {
    exclude.insert(&inst);
    return VisitResult::Progress;
  }

  uint64_t lhs_offset = 0;
  uint64_t rhs_offset = 0;
  auto num_offsets = 0;
  auto ret = VisitResult::NoProgress;

  if (TryGetOffsetOrConst(lhs_val, state_offset, &lhs_offset)) {
    ret = VisitResult::Incomplete;
    num_offsets += 1;
  }

  if (TryGetOffsetOrConst(rhs_val, state_offset, &rhs_offset)) {
    ret = VisitResult::Incomplete;
    num_offsets += 1;
  }

  if (2 == num_offsets) {
    uint64_t offset = 0;
    if (!TryCombineOffsets(lhs_offset, op, rhs_offset, offset_to_slots.size(),
                           &offset)) {
      LOG(WARNING)
          << "Out of bounds operation `"
          << LLVMThingToString(&inst) << "` with LHS offset "
          << static_cast<int64_t>(lhs_offset)
          << ", RHS offset " << static_cast<int64_t>(rhs_offset)
          << ", combined offset " << static_cast<int64_t>(offset)
          << ", and max allowed offset of " << offset_to_slots.size();
      return VisitResult::Error;
    }

    state_offset.emplace(&inst, offset);
    return VisitResult::Progress;

  } else {
    return ret;
  }
}

// Visit a `select` instruction and update the offset map.
VisitResult ForwardAliasVisitor::visitSelect(llvm::SelectInst &inst) {
  auto true_val = inst.getTrueValue();
  auto false_val = inst.getFalseValue();
  auto true_ptr = state_offset.find(true_val);
  auto false_ptr = state_offset.find(false_val);
  auto in_exclude_set = exclude.count(true_val) ||
                        exclude.count(false_val);
  auto in_state_offset = true_ptr != state_offset.end() ||
                         false_ptr != state_offset.end();

  // Fail if the two values are inconsistent.
  if (in_state_offset && in_exclude_set) {
    return VisitResult::Error;

  // At least one of the selected values points into `State`.
  } else if (in_state_offset) {
    if (true_ptr == state_offset.end()) {
      state_offset.emplace(&inst, false_ptr->second);
      return VisitResult::Incomplete;  // Wait for the other to be found.

    } else if (false_ptr == state_offset.end()) {
      state_offset.emplace(&inst, true_ptr->second);
      return VisitResult::Incomplete;  // Wait for the other to be found.

    // Both point into `State`.
    } else {
      if (true_ptr->second == false_ptr->second) {
        state_offset.emplace(&inst, true_ptr->second);
        return VisitResult::Progress;

      } else {
        return VisitResult::Error;
      }
    }

  // At least one of the values being selected definitely does not point
  // into the `State` structure.
  } else if (in_exclude_set) {
    if (exclude.count(true_val) != exclude.count(false_val)) {
      exclude.insert(&inst);
      return VisitResult::Incomplete;  // Wait for the other to be found.

    } else {
      exclude.insert(&inst);
      return VisitResult::Progress;
    }

  // The status of the values being selected are as-of-yet unknown.
  } else {
    return VisitResult::NoProgress;
  }
}

// Visit a PHI node and update the offset map. We unconditionally visit
// all incoming values in PHI nodes, and repeatedly do so until every
// such value is resolved, so that we can make sure that there are no
// inconsistencies.
VisitResult ForwardAliasVisitor::visitPHINode(llvm::PHINode &I) {
  auto complete = true;
  auto in_state_offset = false;
  auto in_exclude_set = false;
  uint64_t offset = 0;

  for (auto &operand : I.operands()) {
    if (exclude.count(operand)) {
      in_exclude_set = true;
      continue;
    }

    auto ptr = state_offset.find(operand);

    // The status of the incoming value is unknown, so we can't yet mark
    // handling this PHI as being complete.
    if (ptr == state_offset.end()) {
      complete = false;
      continue;
    }

    // This is the first incoming value that points into `State`.
    if (!in_state_offset) {
      offset = ptr->second;
      in_state_offset = true;

    // This is the Nth incoming value that points into `State`, let's
    // make sure that it aggrees with the others.
    } else if (ptr->second != offset) {
      return VisitResult::Error;
    }
  }

  // Fail if some operands are excluded and others are state offsets.
  if (in_state_offset && in_exclude_set) {
    return VisitResult::Error;

  // At least one incoming value is a `State` offset, so opportunistically
  // assume that all will match. This lets us have the algorithm progress
  // in the presence of loops.
  } else if (in_state_offset) {
    state_offset.emplace(&I, offset);
    return (complete ? VisitResult::Progress : VisitResult::Incomplete);

  // Similar case to above, but at least one thing is in the exclude set.
  } else if (in_exclude_set) {
    exclude.insert(&I);
    return (complete ? VisitResult::Progress : VisitResult::Incomplete);

  } else {
    return VisitResult::NoProgress;
  }
}

VisitResult ForwardAliasVisitor::visitCallInst(llvm::CallInst &inst) {

  // If we have not seen this instruction before, add it.
  auto args = inst.arg_operands();
  auto live = GetLiveSetFromArgs(args, state_offset, offset_to_slots);
  live_args.emplace(&inst, std::move(live));
  return VisitResult::Ignored;
}

VisitResult ForwardAliasVisitor::visitInvokeInst(llvm::InvokeInst &inst) {

  // If we have not seen this instruction before, add it.
  auto args = inst.arg_operands();
  auto live = GetLiveSetFromArgs(args, state_offset, offset_to_slots);
  live_args.emplace(&inst, std::move(live));
  return VisitResult::Ignored;
}

// Back-and-forth mapping between LLVM meta-data node that we create per slot,
// and `StateSlot`s.
class AAMDInfo {
 public:
  // Return a map of `llvm::MDNode` scopes and a vector of AAMDNodes based on
  // the given vector of `StateSlot`s, where each byte offset (i.e. index) in
  // the slots vector is mapped to a corresponding `llvm::AAMDNodes` struct.
  AAMDInfo(const std::vector<StateSlot> &offset_to_slot,
           llvm::LLVMContext &context);

  // Maps `llvm::MDNode`s to byte offset into the `State` structure.
  ScopeToOffset slot_scopes;

  // Maps byte offsets in the `State` structure to `llvm::AAMDNodes`
  std::vector<llvm::AAMDNodes> slot_aamds;
};

// Return a map of `llvm::MDNode` scopes and a vector of AAMDNodes based on
// the given vector of `StateSlot`s, where each byte offset (i.e. index) in
// the slots vector is mapped to a corresponding `llvm::AAMDNodes` struct.
AAMDInfo::AAMDInfo(const std::vector<StateSlot> &offset_to_slot,
                   llvm::LLVMContext &context) {
  auto arch = GetTargetArch();

  // Create a vector of pairs of scopes to slot offsets. This will be made
  // into a map at the end of the function. We need it as a vector for now
  // so that it is ordered when creating the `noalias` sets.
  std::vector<std::pair<llvm::MDNode *, uint64_t>> scope_offsets;
  scope_offsets.reserve(offset_to_slot.size());
  for (const auto &slot : offset_to_slot) {
    llvm::MDString *mdstr = nullptr;
    if (auto reg = arch->RegisterAtStateOffset(slot.offset)) {
      mdstr = llvm::MDString::get(context, reg->EnclosingRegister()->name);
    } else {
      mdstr = llvm::MDString::get(
          context, "slot_" + std::to_string(slot.index));
    }
    scope_offsets.emplace_back(
        llvm::MDNode::get(context, mdstr), slot.offset);
  }

  // One `llvm::AAMDNodes` struct for each byte offset so that we can easily
  // connect them.
  slot_aamds.reserve(offset_to_slot.size());
  for (uint64_t i = 0; i < offset_to_slot.size(); i++) {

    // This byte belongs to the same slot as the previous byte, so duplicate
    // the previous info.
    if (!slot_aamds.empty() && i &&
        offset_to_slot[i].index == offset_to_slot[i - 1].index) {
      slot_aamds.push_back(slot_aamds.back());
      continue;
    }

    // The `noalias` set is all `llvm::MDNode`s that aren't associated with
    // the slot.
    std::vector<llvm::Metadata *> noalias_vec;
    noalias_vec.reserve(offset_to_slot.back().index);

    for (uint64_t j = 0; j < offset_to_slot.size(); j++) {

      // The `j`th byte offset belong to the same slot as the `i`th byte
      // offset.
      if (offset_to_slot[i].index == offset_to_slot[j].index) {
        continue;

      // Duplicate of previous one.
      } else if (j && offset_to_slot[j].index == offset_to_slot[j - 1].index) {
        continue;

      } else {
        noalias_vec.push_back(scope_offsets[j].first);
      }
    }

    auto noalias = llvm::MDNode::get(
        context, llvm::MDTuple::get(context, noalias_vec));
    slot_aamds.emplace_back(nullptr, scope_offsets[i].first, noalias);
  }
  slot_scopes.insert(scope_offsets.begin(), scope_offsets.end());
}


class LiveSetBlockVisitor {
 public:
  llvm::Module &module;
  InstToLiveSet debug_live_args_at_call;
  const InstToLiveSet &live_args;
  const ScopeToOffset &scope_to_offset;
  const std::vector<StateSlot> &offset_to_slot;
  std::vector<llvm::BasicBlock *> curr_wl;
  std::unordered_map<llvm::BasicBlock *, LiveSet> block_map;
  std::vector<llvm::Instruction *> to_remove;
  const llvm::Function *bb_func;

  LiveSetBlockVisitor(llvm::Module &module_,
                      const InstToLiveSet &live_args_,
                      const ScopeToOffset &scope_to_offset_,
                      const std::vector<StateSlot> &state_slots_,
                      const llvm::Function *bb_func_,
                      const llvm::DataLayout *dl_);
  void FindLiveInsts(void);

  void CollectDeadInsts(void);
  bool VisitBlock(llvm::BasicBlock *block);
  bool DeleteDeadInsts(KillCounter &stats);
  void CreateDOTDigraph(llvm::Function *func, const char *extensions);

 private:
  bool on_remove_pass;
  const llvm::DataLayout *dl;
};

LiveSetBlockVisitor::LiveSetBlockVisitor(
    llvm::Module &module_,
    const InstToLiveSet &live_args_,
    const ScopeToOffset &scope_to_offset_,
    const std::vector<StateSlot> &state_slots_,
    const llvm::Function *bb_func_,
    const llvm::DataLayout *dl_)
    : module(module_),
      live_args(live_args_),
      scope_to_offset(scope_to_offset_),
      offset_to_slot(state_slots_),
      curr_wl(),
      block_map(),
      to_remove(),
      bb_func(bb_func_),
      on_remove_pass(false),
      dl(dl_) {
  for (auto &func : module) {
    for (auto &block : func) {
      auto succ_block_it = successors(&block);
      if (succ_block_it.begin() == succ_block_it.end()) {
        curr_wl.push_back(&block);
      }
    }
  }
}

// Visit the basic blocks in the worklist and update the block_map.
void LiveSetBlockVisitor::FindLiveInsts(void) {
  std::vector<llvm::BasicBlock *> next_wl;
  while (!curr_wl.empty()) {
    for (auto block : curr_wl) {

      // If we change the live slots state of the block, then add the
      // block's predecessors to the next work list.
      if (VisitBlock(block)) {
        int num_preds = 0;
        for (llvm::BasicBlock *pred : predecessors(block)) {
          next_wl.push_back(pred);
          num_preds++;
        }

        // If we've visited an entry block, add its callers to the
        // next work list.
        if (!num_preds) {
          auto func = block->getParent();
          for (auto user : func->users()) {
            if (auto inst = llvm::dyn_cast<llvm::Instruction>(user)) {
              if (llvm::isa<llvm::CallInst>(inst) ||
                  llvm::isa<llvm::InvokeInst>(inst)) {
                next_wl.push_back(inst->getParent());
              }
            }
          }
        }
      }
    }

    curr_wl.swap(next_wl);
    next_wl.clear();
  }
}

bool LiveSetBlockVisitor::VisitBlock(llvm::BasicBlock *block) {
  LiveSet live;

  for (auto inst_it = block->rbegin(); inst_it != block->rend(); ++inst_it) {
    auto inst = &*inst_it;

    // Code that we return to or branch to could read out registers
    // so mark as all live.
    if (llvm::isa<llvm::ReturnInst>(inst) ||
        llvm::isa<llvm::UnreachableInst>(inst) ||
        llvm::isa<llvm::IndirectBrInst>(inst) ||
        llvm::isa<llvm::ResumeInst>(inst) ||
        llvm::isa<llvm::CatchSwitchInst>(inst) ||
        llvm::isa<llvm::CatchReturnInst>(inst) ||
        llvm::isa<llvm::CleanupReturnInst>(inst)) {

      live.set();

    // Update the live set from the successors. If a successors has not
    // been visited yet then we will inherit an empty live set. This is
    // fine because our algorithm converges towards bits being set.
    } else if (llvm::isa<llvm::BranchInst>(inst) ||
               llvm::isa<llvm::SwitchInst>(inst)) {
      for (llvm::BasicBlock *succ : successors(block)) {
        live |= block_map[succ];
      }

    // This could be a call to another lifted function or control-flow
    // intrinsic, or to something that won't access the state like a simple
    // memory intrinsic or LLVM intrinsic (e.g. bswap).
    } else if (llvm::isa<llvm::CallInst>(inst) ||
               llvm::isa<llvm::InvokeInst>(inst)) {

      auto args = inst->operands();
      auto func = inst->getFunction();
      if (llvm::isa<llvm::CallInst>(inst)) {
        auto call_inst = llvm::dyn_cast<llvm::CallInst>(inst);
        args = call_inst->arg_operands();
        func = call_inst->getCalledFunction();
      } else {
        auto invoke_inst = llvm::dyn_cast<llvm::InvokeInst>(inst);
        args = invoke_inst->arg_operands();
        func = invoke_inst->getCalledFunction();
      }

      // We're calling another lifted function; add a trigger relation between
      // this block and the called function's entry block.
      if (func && IsLiftedFunction(func, bb_func)) {
        auto entry_block = &*func->begin();
        live = block_map[entry_block];

        if (!FLAGS_dot_output_dir.empty()) {
          debug_live_args_at_call[inst] = live;
        }

      // We're calling something for which we lack the code, so just use prior
      // information about the arguments.
      } else {
        auto arg_live_it = live_args.find(inst);

        // Likely due to a more general failure to analyze this particular
        // function.
        if (arg_live_it == live_args.end()) {
          live.set();
        } else {
          live |= arg_live_it->second;
        }
      }

    } else if (auto store_inst = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      auto scope = GetScopeFromInst(*inst);
      if (!scope) {
        continue;
      }

      auto val = store_inst->getOperand(0);
      auto val_size = dl->getTypeAllocSize(val->getType());
      const auto &state_slot = offset_to_slot[scope_to_offset.at(scope)];
      auto slot_num = state_slot.index;

      if (!live.test(slot_num)) {
        if (on_remove_pass) {
          to_remove.push_back(inst);
        }

      // We're storing to all the bytes, so kill it. Ignore partial stores
      // (that would revive it) because it's already marked as live.
      } else if (val_size == state_slot.size) {
        live.reset(slot_num);
      }

    // Loads from slots revive the slots.
    } else if (llvm::isa<llvm::LoadInst>(inst)) {
      if (auto scope = GetScopeFromInst(*inst)) {
        auto slot_num = offset_to_slot[scope_to_offset.at(scope)].index;
        live.set(slot_num);
      }
    }
  }

  auto &old_live_on_entry = block_map[block];
  if (old_live_on_entry != live) {
    old_live_on_entry = live;
    return true;
  } else {
    return false;
  }
}

void LiveSetBlockVisitor::CollectDeadInsts(void) {
  on_remove_pass = true;
  for (auto &func : module) {
    for (auto &block : func) {
      VisitBlock(&block);
    }
  }
  on_remove_pass = false;
}

// Remove all dead stores.
bool LiveSetBlockVisitor::DeleteDeadInsts(KillCounter &stats) {
  stats.dead_stores += to_remove.size();
  bool changed = false;
  while (!to_remove.empty()) {
    stats.removed_insts++;
    auto inst = to_remove.back();
    to_remove.pop_back();

    if (!inst->getType()->isVoidTy()) {
      inst->replaceAllUsesWith(llvm::UndefValue::get(inst->getType()));
    }

    for (auto &operand : inst->operands()) {
      if (auto op_inst = llvm::dyn_cast<llvm::Instruction>(operand)) {
        operand = nullptr;
        if (llvm::isInstructionTriviallyDead(op_inst)) {
          to_remove.push_back(op_inst);
        }
      }
    }
    inst->eraseFromParent();
    changed = true;
  }
  return changed;
}

static void StreamSlot(std::ostream &dot, const StateSlot &slot) {
  auto arch = GetTargetArch();
  // slot #
  if (auto reg = arch->RegisterAtStateOffset(slot.offset)) {
    auto enc_reg = reg->EnclosingRegisterOfSize(slot.size);
    if (!enc_reg) {
      enc_reg = reg->EnclosingRegister();
    }
    dot << enc_reg->name;
  } else {
    dot << slot.index;
  }
}

// Generate a DOT digraph file representing the dataflow of the LSBV.
void LiveSetBlockVisitor::CreateDOTDigraph(llvm::Function *func,
                                           const char *extension) {
  std::stringstream fname;
  fname << FLAGS_dot_output_dir << PathSeparator();
  if (!func->hasName()) {
    fname << "func_" << std::hex << reinterpret_cast<uintptr_t>(&func);
  } else {
    fname << func->getName().str();
  }
  fname << extension;

  std::ofstream dot(fname.str());
  dot << "digraph {" << std::endl
      << "node [shape=none margin=0 nojustify=false labeljust=l]"
      << std::endl;

  // Make a vector so that we can go from slot index to slot.
  std::vector<const StateSlot *> slots;
  slots.resize(offset_to_slot.back().index + 1);
  for (auto &slot : offset_to_slot) {
    slots[slot.index] = &slot;
  }

  // Stream node information for each block.
  for (auto &block_ref : *func) {
    auto block_live_ptr = block_map.find(&block_ref);
    if (block_live_ptr == block_map.end()) {
      continue;
    }

    auto block = &block_ref;
    const auto &blive = block_live_ptr->second;

    // Figure out the live set on exit from the block.
    LiveSet exit_live;
    int num_succs = 0;
    for (auto succ_block_it : successors(block)) {
      auto succ = &*succ_block_it;
      exit_live |= block_map[succ];
      num_succs++;
    }

    if (!num_succs) {
      exit_live.set();
    }

    dot << "b" << reinterpret_cast<uintptr_t>(block)
        << " [label=<<table cellspacing=\"0\">" << std::endl;

    // First row, print out the DEAD slots on entry.
    dot << "<tr><td align=\"left\" colspan=\"3\">";
    auto sep = "dead: ";
    for (uint64_t i = 0; i < slots.size(); i++) {
      if (!blive.test(i)) {
        dot << sep;
        StreamSlot(dot, *(slots[i]));
        sep = ", ";
      }
    }
    dot << "</td></tr>" << std::endl;

    // Then print out one row per instruction.
    for (auto &inst : *block) {
      // First row, print out the DEAD slots on entry.
      if (debug_live_args_at_call.count(&inst)) {
        const auto &clive = debug_live_args_at_call[&inst];
        dot << "<tr><td align=\"left\" colspan=\"3\">";
        sep = "dead: ";
        for (uint64_t i = 0; i < slots.size(); i++) {
          if (!clive.test(i)) {
            dot << sep;
            StreamSlot(dot, *(slots[i]));
            sep = ", ";
          }
        }
        dot << "</td></tr>" << std::endl;
      }

      dot << "<tr><td align=\"left\">";

      if (auto scope = GetScopeFromInst(inst)) {
        const auto &slot = offset_to_slot[scope_to_offset.at(scope)];
        auto inst_size = 0;
        if (llvm::isa<llvm::LoadInst>(&inst)) {
          inst_size = dl->getTypeAllocSize(inst.getType());
        } else if (llvm::isa<llvm::StoreInst>(&inst)) {
          inst_size = dl->getTypeAllocSize(inst.getOperand(0)->getType());
        } else {
          LOG(FATAL)
              << "Instruction " << LLVMThingToString(&inst)
              << " has scope meta-data";
        }

        StreamSlot(dot, slot);

        // slot size minus load/store size
        dot << "</td><td align=\"left\">" << (slot.size - inst_size)
            << "</td>";
      } else {
        dot << "</td><td></td>";
      }

      // Calls can be quite wide, so we don't present the whole instruction.
      if (llvm::isa<llvm::CallInst>(&inst) ||
          llvm::isa<llvm::InvokeInst>(&inst)) {
        dot << "<td align=\"left\">  ";

        if (!inst.getType()->isVoidTy()) {
          dot << "%" << inst.getValueID() << " = ";
        }

        llvm::Value *called_val = nullptr;
        if (auto call_inst = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          dot << "call ";
          called_val = call_inst->getCalledValue();
        } else {
          dot << "invoke ";
          auto invoke_inst = llvm::dyn_cast<llvm::InvokeInst>(&inst);
          called_val = invoke_inst->getCalledValue();
        }


        if (called_val->getName().empty()) {
          dot << called_val->getValueID();
        } else {
          dot << called_val->getName().str();
        }

      // PHI nodes can also be quite wide (with the incoming block names)
      // so we compress those as well.
      } else if (auto phi_node = llvm::dyn_cast<llvm::PHINode>(&inst)) {
        dot << "<td align=\"left\">"
            << "  %" << phi_node->getValueID();

        sep = " = phi ";
        for (auto i = 0U; i < phi_node->getNumIncomingValues(); ++i) {
          auto val = phi_node->getIncomingValue(i);
          if (auto inst_val = llvm::dyn_cast<llvm::Instruction>(val)) {
            dot << sep << "%" << inst_val->getValueID();
          } else {
            dot << sep << "...";
          }
          sep = ", ";
        }

      } else {
        llvm::AAMDNodes blank;
        llvm::AAMDNodes original;
        inst.getAAMetadata(original);
        inst.setAAMetadata(blank);

        // Highlight nodes in red that will be removed.
        if (std::count(to_remove.begin(), to_remove.end(), &inst)) {
          dot << "<td align=\"left\" bgcolor=\"red\">"
              << LLVMThingToString(&inst);
        } else {
          dot << "<td align=\"left\">" << LLVMThingToString(&inst);
        }
        inst.setAAMetadata(original);
      }
      dot << "</td></tr>" << std::endl;
    }

    // Last row, print out the DEAD slots incoming from successors.
    dot << "<tr><td align=\"left\" colspan=\"3\">";
    sep = "dead: ";
    for (uint64_t i = 0; i < slots.size(); i++) {
      if (!exit_live.test(i)) {
        dot << sep;
        StreamSlot(dot, *(slots[i]));
        sep = ", ";
      }
    }
    dot << "</td></tr>" << std::endl;

    dot << "</table>>];" << std::endl;

    // Arrows to successor blocks.
    for (auto succ_block_it : successors(block)) {
      auto succ = &*succ_block_it;
      dot << "b" << reinterpret_cast<uintptr_t>(block) << " -> b"
          << reinterpret_cast<uintptr_t>(succ) << std::endl;
    }
  }
  dot << "}" << std::endl;
}

// For each instruction in the alias map, add an `llvm::AAMDNodes` struct
// which specifies the aliasing stores and loads to the instruction's
// byte offset.
static void AddAAMDNodes(
    const InstToOffset &inst_to_offset,
    const std::vector<llvm::AAMDNodes> &offset_to_aamd) {
  for (const auto &map_pair : inst_to_offset) {
    auto inst = map_pair.first;
    auto offset = map_pair.second;
    if (auto load_inst = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      auto aamd = offset_to_aamd[offset];
      load_inst->setAAMetadata(aamd);
    } else if (auto store_inst = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      auto aamd = offset_to_aamd[offset];
      store_inst->setAAMetadata(aamd);
    }
  }
}

class ForwardingBlockVisitor {
  public:
    llvm::Function &func;
    const InstToOffset &inst_to_offset;
    const ScopeToOffset &scope_to_offset;
    const std::vector<StateSlot> &state_slots;
    const llvm::FunctionType *lifted_func_ty;

    ForwardingBlockVisitor(
      llvm::Function &func_,
      const InstToOffset &inst_to_offset_,
      const ScopeToOffset &scope_to_offset_,
      const std::vector<StateSlot> &state_slots_,
      const llvm::DataLayout *dl_);

    void Visit(const ValueToOffset &val_to_offset, KillCounter &stats);
    void VisitBlock(llvm::BasicBlock *block, const ValueToOffset &val_to_offset,
                    KillCounter &stats);

  private:
    const llvm::DataLayout *dl;
};

ForwardingBlockVisitor::ForwardingBlockVisitor(
    llvm::Function &func_,
    const InstToOffset &inst_to_offset_,
    const ScopeToOffset &scope_to_offset_,
    const std::vector<StateSlot> &state_slots_,
    const llvm::DataLayout *dl_)
    : func(func_),
      inst_to_offset(inst_to_offset_),
      scope_to_offset(scope_to_offset_),
      state_slots(state_slots_),
      lifted_func_ty(func.getFunctionType()),
      dl(dl_) {}

void ForwardingBlockVisitor::Visit(const ValueToOffset &val_to_offset,
                                   KillCounter &stats) {
  // If any visit makes progress, continue the loop.
  for (auto &block : func) {
    VisitBlock(&block, val_to_offset, stats);
  }
}

void ForwardingBlockVisitor::VisitBlock(llvm::BasicBlock *block,
                                        const ValueToOffset &val_to_offset,
                                        KillCounter &stats) {
  std::unordered_map<uint64_t, llvm::LoadInst *> slot_to_load;

  // Collect the instructions into a vector. We're going to be shuffling them
  // around and deleting some, so we don't want to invalidate any iterators.
  std::vector<llvm::Instruction *> insts;
  for (auto inst_it = block->rbegin(); inst_it != block->rend(); ++inst_it) {
    insts.push_back(&*inst_it);
  }

  for (auto &inst : insts) {
    if (llvm::isa<llvm::CallInst>(inst) ||
        llvm::isa<llvm::InvokeInst>(inst)) {

      auto args = inst->operands();
      if (llvm::isa<llvm::CallInst>(inst)) {
        args = llvm::dyn_cast<llvm::CallInst>(inst)->arg_operands();
      } else {
        args = llvm::dyn_cast<llvm::InvokeInst>(inst)->arg_operands();
      }

      for (auto &arg_it : args) {
        auto arg = arg_it->stripPointerCasts();
        const auto offset_it = val_to_offset.find(arg);
        if (offset_it != val_to_offset.end()) {
          const auto offset = offset_it->second;

          // If we access a single non-zero offset, mark just that slot.
          if (offset != 0) {
            const auto &slot = state_slots[offset];
            slot_to_load.erase(slot.index);

          // If we access offset `0`, then maybe we're actually passing
          // a state pointer, in which anything can be changed.
          } else {
            slot_to_load.clear();
          }
        }
      }
    // Try to do store-to-load forwarding.
    } else if (auto store_inst = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      const auto scope = GetScopeFromInst(*inst);
      if (!scope) {
        continue;
      }

      const auto val = store_inst->getOperand(0);
      const auto val_type = val->getType();
      const auto val_size = dl->getTypeAllocSize(val_type);
      const auto &state_slot = state_slots[scope_to_offset.at(scope)];
      if (!slot_to_load.count(state_slot.index)) {
        continue;
      }

      const auto next_load = slot_to_load[state_slot.index];
      const auto next_type = next_load->getType();

      // We're visiting a store so erase the entry because we don't want to
      // accidentally forward around a store.
      slot_to_load.erase(state_slot.index);

      if (inst_to_offset.at(store_inst) != inst_to_offset.at(next_load)) {
        stats.fwd_failed++;
        continue;
      }

      auto next_size = dl->getTypeAllocSize(next_type);

      // Perfect forwarding.
      if (val_type == next_type) {
        next_load->replaceAllUsesWith(val);
        next_load->eraseFromParent();
        stats.fwd_perfect++;
        stats.fwd_stores++;

      // Forwarding, but changing the type.
      } else if (next_size == val_size) {
        auto cast = new llvm::BitCastInst(val, next_type, "", next_load);
        next_load->replaceAllUsesWith(cast);
        next_load->eraseFromParent();
        stats.fwd_casted++;
        stats.fwd_stores++;

      // Forwarding, but changing the size.
      } else if (next_size < val_size) {
        if (val_type->isIntegerTy() && next_type->isIntegerTy()) {
          auto trunc = new llvm::TruncInst(val, next_type, "", next_load);
          next_load->replaceAllUsesWith(trunc);
          next_load->eraseFromParent();

        } else if (val_type->isFloatingPointTy() &&
                   next_type->isFloatingPointTy()) {
          auto trunc = new llvm::FPTruncInst(val, next_type, "", next_load);
          next_load->replaceAllUsesWith(trunc);
          next_load->eraseFromParent();

        } else {
          stats.fwd_failed++;
          continue;
        }

        stats.fwd_truncated++;
        stats.fwd_stores++;

      // This is like a store to `AX` followed by a load of `EAX`.
      } else {
        stats.fwd_failed++;
        continue;
      }

    // Try to do load-to-load forwarding.
    } else if (auto load_inst = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      auto scope = GetScopeFromInst(*load_inst);
      if (!scope) {
        continue;
      }

      auto &state_slot = state_slots[scope_to_offset.at(scope)];
      auto &load_ref = slot_to_load[state_slot.index];

      // Get the next load, and update the slot with the current load.
      auto next_load = load_ref;
      load_ref = load_inst;

      // There was no next load, but instead the map default-initialized to
      // `nullptr`, so move on with this load as a candidate for being the
      // target of forwarding.
      if (!next_load) {
        continue;
      }

      // E.g. One load of `AH`, one load of `AL`.
      if (inst_to_offset.at(load_inst) != inst_to_offset.at(next_load)) {
        stats.fwd_failed++;
        continue;
      }

      auto val_type = load_inst->getType();
      auto val_size = dl->getTypeAllocSize(val_type);
      auto next_type = next_load->getType();
      auto next_size = dl->getTypeAllocSize(next_type);

      // Perfecting forwarding.
      if (val_type == next_type) {
        next_load->replaceAllUsesWith(load_inst);
        next_load->eraseFromParent();
        stats.fwd_perfect++;
        stats.fwd_loads++;

      // Forwarding, but changing the type.
      } else if (val_size == next_size) {
        auto cast = new llvm::BitCastInst(load_inst, next_type, "", next_load);
        next_load->replaceAllUsesWith(cast);
        next_load->eraseFromParent();
        stats.fwd_casted++;
        stats.fwd_loads++;

      // Forwarding, but changing the size.
      } else if (next_size < val_size) {
      try_truncate:
        if (val_type->isIntegerTy() && next_type->isIntegerTy()) {
          auto trunc = new llvm::TruncInst(load_inst, next_type, "", next_load);
          next_load->replaceAllUsesWith(trunc);
          next_load->eraseFromParent();

        } else if (val_type->isFloatingPointTy() &&
                   next_type->isFloatingPointTy()) {
          auto trunc = new llvm::FPTruncInst(
              load_inst, next_type, "", next_load);
          next_load->replaceAllUsesWith(trunc);
          next_load->eraseFromParent();
        } else {
          stats.fwd_failed++;
          slot_to_load.erase(state_slot.index);
          continue;
        }

        stats.fwd_truncated++;
        stats.fwd_loads++;

      // Try to re-order the loads.
      } else {
        next_load->removeFromParent();
        next_load->insertBefore(load_inst);
        load_ref = next_load;
        std::swap(next_load, load_inst);
        std::swap(next_size, val_size);
        std::swap(val_type, next_type);
        stats.fwd_reordered++;
        goto try_truncate;
      }
    }
  }
}

}  // namespace

// Returns a covering vector of `StateSlots` for the module's `State` type.
// This vector contains one entry per byte of the `State` type.
std::vector<StateSlot> StateSlots(llvm::Module *module) {

  if (!FLAGS_dot_output_dir.empty()) {
    if (!TryCreateDirectory(FLAGS_dot_output_dir)) {
      FLAGS_dot_output_dir.clear();
      LOG(ERROR)
          << "Invalid path specified to `--dot_output_dir`.";
    } else {
      FLAGS_dot_output_dir = CanonicalPath(FLAGS_dot_output_dir);
    }
  }

  auto state_ptr_type = StatePointerType(module);
  auto type = state_ptr_type->getElementType();
  CHECK(type->isStructTy());

  llvm::DataLayout dl = module->getDataLayout();
  const auto num_bytes = dl.getTypeAllocSize(type);
  StateVisitor vis(&dl, num_bytes);
  vis.Visit(type);
  CHECK(vis.offset_to_slot.size() == num_bytes);
  return vis.offset_to_slot;
}

// Analyze a module, discover aliasing loads and stores, and remove dead
// stores into the `State` structure.
void RemoveDeadStores(llvm::Module *module,
                      llvm::Function *bb_func,
                      const std::vector<StateSlot> &slots) {

  KillCounter stats = {};
  const AAMDInfo aamd_info(slots, module->getContext());
  const llvm::DataLayout dl = module->getDataLayout();

  InstToLiveSet live_args;

  for (auto &func : *module) {
    if (!IsLiftedFunction(&func, bb_func)) {
      continue;
    }

    ForwardAliasVisitor fav(dl, slots, live_args);

    // If the analysis succeeds for this function, add the AAMDNodes.
    if (fav.Analyze(&func)) {
      AddAAMDNodes(fav.state_access_offset, aamd_info.slot_aamds);
    }

    // Perform load and store forwarding.
    if (!FLAGS_disable_register_forwarding) {
      ForwardingBlockVisitor fbv(func, fav.state_access_offset,
                                 aamd_info.slot_scopes, slots, &dl);
      fbv.Visit(fav.state_offset, stats);
    }
  }

  // Perform live set analysis
  LiveSetBlockVisitor visitor(*module, live_args, aamd_info.slot_scopes,
                              slots, bb_func, &dl);

  visitor.FindLiveInsts();
  visitor.CollectDeadInsts();

  if (!FLAGS_dot_output_dir.empty()) {
    for (auto &func : *module) {
      if (IsLiftedFunction(&func, bb_func)) {
        visitor.CreateDOTDigraph(&func, ".dot");
      }
    }
  }

  visitor.DeleteDeadInsts(stats);

  if (!FLAGS_dot_output_dir.empty()) {
    for (auto &func : *module) {
      if (IsLiftedFunction(&func, bb_func)) {
        visitor.CreateDOTDigraph(&func, ".post.dot");
      }
    }
  }

  LOG(INFO)
      << "Dead stores: " << stats.dead_stores << "; "
      << "Instructions removed from DSE: " << stats.removed_insts << "; "
      << "Forwarded loads: " << stats.fwd_loads << "; "
      << "Forwarded stores: " << stats.fwd_stores << "; "
      << "Perfectly forwarded: " << stats.fwd_perfect << "; "
      << "Forwarded by truncation: " << stats.fwd_truncated << "; "
      << "Forwarded by casting: " << stats.fwd_casted << "; "
      << "Forwarded by reordering: " << stats.fwd_reordered << "; "
      << "Could not forward: " << stats.fwd_failed;
}

}  // namespace remill
