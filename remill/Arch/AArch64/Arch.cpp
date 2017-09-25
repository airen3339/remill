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

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include "remill/Arch/AArch64/Decode.h"
#include "remill/Arch/Arch.h"
#include "remill/Arch/Instruction.h"
#include "remill/Arch/Name.h"
#include "remill/BC/Version.h"
#include "remill/OS/OS.h"

namespace remill {
namespace {

static constexpr int kInstructionSize = 4;  // In bytes.
static constexpr int kPCWidth = 64;  // In bits.

template <uint32_t bit, typename T>
static inline T Select(T val) {
  return (val >> bit) & T(1);
}

Instruction::Category InstCategory(const aarch64::InstData &inst) {
  switch (inst.iclass) {
    case aarch64::InstName::INVALID:
      return Instruction::kCategoryInvalid;

    // TODO(pag): B.cond.
    case aarch64::InstName::B:
      if (aarch64::InstForm::B_ONLY_CONDBRANCH == inst.iform) {
        return Instruction::kCategoryConditionalBranch;
      } else {
        return Instruction::kCategoryDirectJump;
      }

    case aarch64::InstName::BR:
      return Instruction::kCategoryIndirectJump;

    case aarch64::InstName::CBZ:
    case aarch64::InstName::CBNZ:
    case aarch64::InstName::TBZ:
    case aarch64::InstName::TBNZ:
      return Instruction::kCategoryConditionalBranch;

    case aarch64::InstName::BL:
      return Instruction::kCategoryDirectFunctionCall;

    case aarch64::InstName::BLR:
      return Instruction::kCategoryIndirectFunctionCall;

    case aarch64::InstName::RET:
      return Instruction::kCategoryFunctionReturn;

    case aarch64::InstName::HLT:
      return Instruction::kCategoryError;

    case aarch64::InstName::HVC:
    case aarch64::InstName::SMC:
    case aarch64::InstName::SVC:
    case aarch64::InstName::SYS:  // Has aliases `IC`, `DC`, `AT`, and `TLBI`.
    case aarch64::InstName::SYSL:
      return Instruction::kCategoryAsyncHyperCall;

    case aarch64::InstName::HINT:
    case aarch64::InstName::NOP:
      return Instruction::kCategoryNoOp;

    // Note: These are implemented with synchronous hyper calls.
    case aarch64::InstName::BRK:
      return Instruction::kCategoryNormal;

    default:
      return Instruction::kCategoryNormal;
  }
}

class AArch64Arch : public Arch {
 public:
  AArch64Arch(OSName os_name_, ArchName arch_name_);

  virtual ~AArch64Arch(void);

  // Decode an instruction.
  bool DecodeInstruction(
      uint64_t address, const std::string &instr_bytes,
      Instruction &inst) const override;

  // Maximum number of bytes in an instruction.
  uint64_t MaxInstructionSize(void) const override;

  llvm::Triple Triple(void) const override;
  llvm::DataLayout DataLayout(void) const override;

  // Default calling convention for this architecture.
  llvm::CallingConv::ID DefaultCallingConv(void) const override;

 private:
  AArch64Arch(void) = delete;
};

AArch64Arch::AArch64Arch(OSName os_name_, ArchName arch_name_)
    : Arch(os_name_, arch_name_) {}

AArch64Arch::~AArch64Arch(void) {}

// Default calling convention for this architecture.
llvm::CallingConv::ID AArch64Arch::DefaultCallingConv(void) const {
  return llvm::CallingConv::C;
}

// Maximum number of bytes in an instruction for this particular architecture.
uint64_t AArch64Arch::MaxInstructionSize(void) const {
  return 4;
}

llvm::Triple AArch64Arch::Triple(void) const {
  auto triple = BasicTriple();
  switch (arch_name) {
    case kArchAArch64LittleEndian:
      triple.setArch(llvm::Triple::aarch64);
      break;

    default:
      LOG(FATAL)
          << "Cannot get triple for non-AArch64 architecture "
          << GetArchName(arch_name);
      break;
  }
  return triple;
}

llvm::DataLayout AArch64Arch::DataLayout(void) const {
  std::string dl;
  switch (arch_name) {
    case kArchAArch64LittleEndian:
      dl = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";
      break;

    default:
      LOG(FATAL)
          << "Cannot get data layout for non-AArch64 architecture "
          << GetArchName(arch_name);
      break;
  }
  return llvm::DataLayout(dl);
}

enum RegClass {
  kRegX,  // 64-bit int.
  kRegW,  // Word, 32-bit int.
  kRegB,  // Byte.
  kRegH,  // Half-word, 16-bit float.
  kRegS,  // Single-precision float.
  kRegD,  // Doubleword, Double precision float.
  kRegQ,  // Quadword.
};

using RegNum = uint8_t;

enum RegUsage {
  kUseAsAddress,  // Interpret X31 == SP and W32 == WSP.
  kUseAsValue  // Interpret X31 == XZR and W31 == WZR.
};

enum Action {
  kActionRead,
  kActionWrite,
  kActionReadWrite
};

// Immediate integer type.
enum ImmType {
  kUnsigned,
  kSigned
};

// Note: Order is significant; extracted bits may be casted to this type.
enum Extend : uint8_t {
  kExtendUXTB,  // 0b000
  kExtendUXTH,  // 0b001
  kExtendUXTW,  // 0b010
  kExtendUXTX,  // 0b011
  kExtendSXTB,  // 0b100
  kExtendSXTH,  // 0b101
  kExtendSXTW,  // 0b110
  kExtendSXTX  // 0b111
};

static uint64_t ExtractSizeInBits(Extend extend) {
  switch (extend) {
    case kExtendUXTB: return 8;
    case kExtendUXTH: return 16;
    case kExtendUXTW: return 32;
    case kExtendUXTX: return 64;
    case kExtendSXTB: return 8;
    case kExtendSXTH: return 16;
    case kExtendSXTW: return 32;
    case kExtendSXTX: return 64;
  }
  return 0;
}

static RegClass ExtendTypeToRegClass(Extend extend) {
  switch (extend) {
    case kExtendUXTB: return kRegW;
    case kExtendUXTH: return kRegW;
    case kExtendUXTW: return kRegW;
    case kExtendUXTX: return kRegX;
    case kExtendSXTB: return kRegW;
    case kExtendSXTH: return kRegW;
    case kExtendSXTW: return kRegW;
    case kExtendSXTX: return kRegX;
  }
}

static Operand::ShiftRegister::Extend ShiftRegExtendType(Extend extend) {
  switch (extend) {
    case kExtendUXTB:
    case kExtendUXTH:
    case kExtendUXTW:
    case kExtendUXTX:
      return Operand::ShiftRegister::kExtendUnsigned;
    case kExtendSXTB:
    case kExtendSXTH:
    case kExtendSXTW:
    case kExtendSXTX:
      return Operand::ShiftRegister::kExtendSigned;
  }
  return Operand::ShiftRegister::kExtendInvalid;
}

// Note: Order is significant; extracted bits may be casted to this type.
enum Shift : uint8_t {
  kShiftLSL,
  kShiftLSR,
  kShiftASR,
  kShiftROR
};

// Translate a shift encoding into an operand shift type used by the shift
// register class.
static Operand::ShiftRegister::Shift GetOperandShift(Shift s) {
  switch (s) {
    case kShiftLSL:
      return Operand::ShiftRegister::kShiftLeftWithZeroes;
    case kShiftLSR:
      return Operand::ShiftRegister::kShiftUnsignedRight;
    case kShiftASR:
      return Operand::ShiftRegister::kShiftSignedRight;
    case kShiftROR:
      return Operand::ShiftRegister::kShiftRightAround;
  }
  return Operand::ShiftRegister::kShiftInvalid;
}

// Get the name of an integer register.
static std::string RegNameXW(Action action, RegClass rclass, RegUsage rtype,
                           RegNum number) {
  CHECK_LE(number, 31U);

  std::stringstream ss;
  CHECK(kActionReadWrite != action);

  if (31 == number) {
    if (rtype == kUseAsValue) {
      if (action == kActionWrite) {
        ss << "IGNORE_WRITE_TO_XZR";
      } else {
        ss << (rclass == kRegX ? "XZR" : "WZR");
      }
    } else {
      if (action == kActionWrite) {
        ss << "SP";
      } else {
        ss << (rclass == kRegX ? "SP" : "WSP");
      }
    }
  } else {
    if (action == kActionWrite) {
      ss << "X";
    } else {
      ss << (rclass == kRegX ? "X" : "W");
    }
    ss << static_cast<unsigned>(number);
  }
  return ss.str();
}

// Get the name of a floating point register.
static std::string RegNameFP(Action action, RegClass rclass, RegUsage rtype,
                             RegNum number) {
  CHECK_LE(number, 31U);

  std::stringstream ss;
  CHECK(kActionReadWrite != action);

  if (kActionRead == action) {
    if (kRegB == rclass) {
      ss << "B";
    } else if (kRegH == rclass) {
      ss << "H";
    } else if (kRegS == rclass) {
      ss << "S";
    } else if (kRegD == rclass) {
      ss << "D";
    } else {
      CHECK(kRegQ == rclass);
      ss << "Q";
    }
  } else {
    ss << "V";
  }

  ss << static_cast<unsigned>(number);

  return ss.str();
}

static std::string RegName(Action action, RegClass rclass, RegUsage rtype,
                           RegNum number) {
  switch (rclass) {
    case kRegX:
    case kRegW:
      return RegNameXW(action, rclass, rtype, number);
    case kRegB:
    case kRegH:
    case kRegS:
    case kRegD:
    case kRegQ:
      return RegNameFP(action, rclass, rtype, number);
  }
}

static uint64_t ReadRegSize(RegClass rclass) {
  switch (rclass) {
    case kRegX:
      return 64;
    case kRegW:
      return 32;
    case kRegB:
      return 8;
    case kRegH:
      return 16;
    case kRegS:
      return 32;
    case kRegD:
      return 64;
    case kRegQ:
      return 128;
  }
  return 0;
}

static uint64_t WriteRegSize(RegClass rclass) {
  switch (rclass) {
    case kRegX:
    case kRegW:
      return 64;
    case kRegB:
    case kRegH:
    case kRegS:
    case kRegD:
    case kRegQ:
      return 128;
  }
  return 0;
}

// This gives us a register operand. If we have an operand like `<Xn|SP>`,
// then the usage is `kTypeUsage`, otherwise (i.e. `<Xn>`), the usage is
// a `kTypeValue`.
static Operand::Register Reg(Action action, RegClass rclass, RegUsage rtype,
                             RegNum reg_num) {
  Operand::Register reg;
  if (kActionWrite == action) {
    reg.name = RegName(action, rclass, rtype, reg_num);
    reg.size = WriteRegSize(rclass);
  } else if (kActionRead == action) {
    reg.name = RegName(action, rclass, rtype, reg_num);
    reg.size = ReadRegSize(rclass);
  } else {
    LOG(FATAL)
        << "Reg function only takes a simple read or write action.";
  }
  return reg;
}

static void AddRegOperand(Instruction &inst, Action action,
                          RegClass rclass, RegUsage rtype,
                          RegNum reg_num) {
  Operand op;
  op.type = Operand::kTypeRegister;

  if (kActionWrite == action || kActionReadWrite == action) {
    op.reg = Reg(kActionWrite, rclass, rtype, reg_num);
    op.size = op.reg.size;
    op.action = Operand::kActionWrite;
    inst.operands.push_back(op);
  }

  if (kActionRead == action || kActionReadWrite == action) {
    op.reg = Reg(kActionRead, rclass, rtype, reg_num);
    op.size = op.reg.size;
    op.action = Operand::kActionRead;
    inst.operands.push_back(op);
  }
}

static void AddShiftRegOperand(Instruction &inst, RegClass rclass,
                               RegUsage rtype, RegNum reg_num,
                               Shift shift_type,
                               uint64_t shift_size) {
  if (!shift_size) {
    AddRegOperand(inst, kActionRead, rclass, rtype, reg_num);
  } else {
    Operand op;
    op.shift_reg.reg = Reg(kActionRead, rclass, rtype, reg_num);
    op.shift_reg.shift_op = GetOperandShift(shift_type);
    op.shift_reg.shift_size = shift_size;

    op.type = Operand::kTypeShiftRegister;
    op.size = op.shift_reg.reg.size;
    op.action = Operand::kActionRead;
    inst.operands.push_back(op);
  }
}

static void AddExtendRegOperand(Instruction &inst, RegClass rclass,
                                RegUsage rtype, RegNum reg_num,
                                Extend extend_type, uint64_t output_size,
                                uint64_t shift_size=0) {
  Operand op;
  op.shift_reg.reg = Reg(kActionRead, rclass, rtype, reg_num);
  op.shift_reg.extend_op = ShiftRegExtendType(extend_type);
  op.shift_reg.extract_size = ExtractSizeInBits(extend_type);

  // No extraction needs to be done, and zero extension already happens.
  if (Operand::ShiftRegister::kExtendUnsigned == op.shift_reg.extend_op &&
      op.shift_reg.extract_size == op.shift_reg.reg.size) {
    op.shift_reg.extend_op = Operand::ShiftRegister::kExtendInvalid;
    op.shift_reg.extract_size = 0;

  // Extracting a value that is wider than the register.
  } else if (op.shift_reg.extract_size > op.shift_reg.reg.size) {
    op.shift_reg.extend_op = Operand::ShiftRegister::kExtendInvalid;
    op.shift_reg.extract_size = 0;
  }

  if (shift_size) {
    op.shift_reg.shift_op = Operand::ShiftRegister::kShiftLeftWithZeroes;
    op.shift_reg.shift_size = shift_size;
  }

  op.type = Operand::kTypeShiftRegister;
  op.size = output_size;
  op.action = Operand::kActionRead;
  inst.operands.push_back(op);
}

static void AddImmOperand(Instruction &inst, uint64_t val,
                          ImmType signedness=kUnsigned,
                          unsigned size=64) {
  Operand op;
  op.type = Operand::kTypeImmediate;
  op.action = Operand::kActionRead;
  op.size = size;
  op.imm.is_signed = signedness == kUnsigned ? false : true;
  op.imm.val = val;
  inst.operands.push_back(op);
}

static void AddPCRegOp(Instruction &inst, Operand::Action action, int64_t disp,
                       Operand::Address::Kind op_kind) {
  Operand op;
  op.type = Operand::kTypeAddress;
  op.size = 64;
  op.addr.address_size = 64;
  op.addr.base_reg.name = "PC";
  op.addr.base_reg.size = 64;
  op.addr.displacement = disp;
  op.addr.kind = op_kind;
  op.action = action;
  inst.operands.push_back(op);
}

// Emit a memory read or write operand of the form `[PC + disp]`.
static void AddPCRegMemOp(Instruction &inst, Action action, int64_t disp) {
  if (kActionRead == action) {
    AddPCRegOp(inst, Operand::kActionRead, disp, Operand::Address::kMemoryRead);
  } else if (kActionWrite == action) {
    AddPCRegOp(inst, Operand::kActionWrite, disp,
               Operand::Address::kMemoryWrite);
  } else {
    LOG(FATAL)<< __FUNCTION__ << " only accepts simple operand actions.";
  }
}

// Emit an address operand that computes `PC + disp`.
static void AddPCDisp(Instruction &inst, int64_t disp) {
  AddPCRegOp(inst, Operand::kActionRead, disp,
             Operand::Address::kAddressCalculation);
}

static void AddNextPC(Instruction &inst) {
  // add +4 as the PC displacement
  // emit an address computation operand
  AddPCDisp(inst, kInstructionSize);
}

// Base+offset memory operands are equivalent to indexing into an array.
//
// We have something like this:
//    [<Xn|SP>, #<imm>]
//
// Which gets is:
//    addr = Xn + imm
//    ... deref addr and do stuff ...
static void AddBasePlusOffsetMemOp(Instruction &inst, Action action,
                                   uint64_t access_size,
                                   RegNum base_reg, uint64_t disp) {
  Operand op;
  op.type = Operand::kTypeAddress;
  op.size = access_size;
  op.addr.address_size = 64;
  op.addr.base_reg = Reg(kActionRead, kRegX, kUseAsAddress, base_reg);
  op.addr.displacement = disp;

  if (kActionWrite == action || kActionReadWrite == action) {
    op.action = Operand::kActionWrite;
    op.addr.kind = Operand::Address::kMemoryWrite;
    inst.operands.push_back(op);
  }

  if (kActionRead == action || kActionReadWrite == action) {
    op.action = Operand::kActionRead;
    op.addr.kind = Operand::Address::kMemoryRead;
    inst.operands.push_back(op);
  }
}

// Pre-index memory operands write back the result of the displaced address
// to the base register.
//
// We have something like this:
//    [<Xn|SP>, #<imm>]!
//
// Which gets us:
//    addr = Xn + imm
//    ... deref addr and do stuff ...
//    Xn = addr + imm
//
// So we add in two operands: one that is a register write operand for Xn,
// the other that is the value of (Xn + imm + imm).
static void AddPreIndexMemOp(Instruction &inst, Action action,
                             uint64_t access_size,
                             RegNum base_reg, uint64_t disp) {
  AddBasePlusOffsetMemOp(inst, action, access_size, base_reg, disp);
  auto addr_op = inst.operands[inst.operands.size() - 1];

  Operand reg_op;
  reg_op.type = Operand::kTypeRegister;
  reg_op.action = Operand::kActionWrite;
  reg_op.reg = Reg(kActionWrite, kRegX, kUseAsAddress, base_reg);
  reg_op.size = reg_op.reg.size;
  inst.operands.push_back(reg_op);

  addr_op.addr.kind = Operand::Address::kAddressCalculation;
  addr_op.addr.address_size = 64;
  addr_op.addr.base_reg = Reg(kActionRead, kRegX, kUseAsAddress, base_reg);
  addr_op.addr.displacement *= 2;
  inst.operands.push_back(addr_op);
}

// Post-index memory operands write back the result of the displaced address
// to the base register.
//
// We have something like this:
//    [<Xn|SP>], #<imm>
//
// Which gets us:
//    addr = Xn
//    ... deref addr and do stuff ...
//    Xn = addr + imm
//
// So we add in two operands: one that is a register write operand for Xn,
// the other that is the value of (Xn + imm).
static void AddPostIndexMemOp(Instruction &inst, Action action,
                              uint64_t access_size,
                              RegNum base_reg, uint64_t disp) {
  AddBasePlusOffsetMemOp(inst, action, access_size, base_reg, 0);
  auto addr_op = inst.operands[inst.operands.size() - 1];

  Operand reg_op;
  reg_op.type = Operand::kTypeRegister;
  reg_op.action = Operand::kActionWrite;
  reg_op.reg = Reg(kActionWrite, kRegX, kUseAsAddress, base_reg);
  reg_op.size = reg_op.reg.size;
  inst.operands.push_back(reg_op);

  addr_op.addr.kind = Operand::Address::kAddressCalculation;
  addr_op.addr.address_size = 64;
  addr_op.addr.base_reg = Reg(kActionRead, kRegX, kUseAsAddress, base_reg);
  addr_op.addr.displacement = disp;
  inst.operands.push_back(addr_op);
}

static bool MostSignificantSetBit(uint64_t val, uint64_t *highest_out) {
  auto found = false;
  for (uint64_t i = 0; i < 64; ++i) {
    if ((val >> i) & 1) {
      *highest_out = i;
      found = true;
    }
  }
  return found;
}

static constexpr uint64_t kOne = static_cast<uint64_t>(1);

inline static uint64_t Ones(uint64_t val) {
  uint64_t out = 0;
  for (; val != 0; --val) {
    out <<= kOne;
    out |= kOne;
  }
  return out;
}

static uint64_t ROR(uint64_t val, uint64_t val_size, uint64_t rotate_amount) {
  for (uint64_t i = 0; i < rotate_amount; ++i) {
    val = ((val & kOne) << (val_size - kOne)) | (val >> kOne);
  }
  return val;
}

// Take a bit string `val` of length `val_size` bits, and concatenate it to
// itself until it occupies at least `goal_size` bits.
static uint64_t Replicate(uint64_t val, uint64_t val_size, uint64_t goal_size) {
  uint64_t replicated_val = 0;
  for (uint64_t i = 0; i < goal_size; i += val_size) {
    replicated_val = (replicated_val << val_size) | val;
  }
  return replicated_val;
}

// Decode bitfield and logical immediate masks. There is a nice piece of code
// here for producing all valid (64-bit) inputs:
//
//      https://stackoverflow.com/a/33265035/247591
//
// The gist of the format is that you hav
static bool DecodeBitMasks(uint64_t N /* one bit */,
                           uint64_t imms /* six bits */,
                           uint64_t immr /* six bits */,
                           bool is_immediate,
                           uint64_t data_size,
                           uint64_t *wmask_out=nullptr,
                           uint64_t *tmask_out=nullptr) {
  uint64_t len = 0;
  if (!MostSignificantSetBit((N << 6ULL) | (~imms & 0x3fULL), &len)) {
    return false;
  }
  if (len < 1) {
    return false;
  }

  const uint64_t esize = kOne << len;
  if (esize > data_size) {
    return false;  // `len == 0` is a `ReservedValue()`.
  }

  const uint64_t levels = Ones(len);  // ZeroExtend(Ones(len), 6).
  const uint64_t R = immr & levels;
  const uint64_t S = imms & levels;

  if (is_immediate && S == levels) {
    return false;  // ReservedValue.
  }

  const uint64_t diff = (S - R) & static_cast<uint64_t>(0x3F);  // 6-bit sbb.
  const uint64_t d = diff & levels;  // `diff<len-1:0>`.
  const uint64_t welem = Ones(S + kOne);
  const uint64_t telem = Ones(d + kOne);
  const uint64_t wmask = Replicate(
      ROR(welem, esize, R), esize, data_size);
  const uint64_t tmask = Replicate(telem, esize, data_size);

  if (wmask_out) {
    *wmask_out = wmask;
  }

  if (tmask_out) {
    *tmask_out = tmask;
  }
  return true;
}

bool AArch64Arch::DecodeInstruction(
    uint64_t address, const std::string &inst_bytes,
    Instruction &inst) const {

  aarch64::InstData dinst = {};
  auto bytes = reinterpret_cast<const uint8_t *>(inst_bytes.data());

  inst.arch_name = arch_name;
  inst.pc = address;
  inst.next_pc = address + kInstructionSize;
  inst.category = Instruction::kCategoryInvalid;

  if (kInstructionSize != inst_bytes.size()) {
    inst.category = Instruction::kCategoryError;
    return false;

  } else if (0 != (address % kInstructionSize)) {
    inst.category = Instruction::kCategoryError;
    return false;

  } else if (!aarch64::TryExtract(bytes, dinst)) {
    inst.category = Instruction::kCategoryInvalid;
    return false;
  }

  inst.bytes = inst_bytes.substr(0, kInstructionSize);
  inst.category = InstCategory(dinst);
  inst.function = aarch64::InstFormToString(dinst.iform);

  if (!aarch64::TryDecode(dinst, inst)) {
    inst.category = Instruction::kCategoryError;
    return false;
  }

  return true;
}

}  // namespace

namespace aarch64 {

// RET  {<Xn>}
bool TryDecodeRET_64R_BRANCH_REG(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  return true;
}

// BLR  <Xn>
bool TryDecodeBLR_64_BRANCH_REG(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddNextPC(inst);
  return true;
}

// STP  <Wt1>, <Wt2>, [<Xn|SP>, #<imm>]!
bool TryDecodeSTP_32_LDSTPAIR_PRE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt2);
  uint64_t offset = static_cast<uint64_t>(data.imm7.simm7);
  AddPreIndexMemOp(inst, kActionWrite, 64, data.Rn, offset << 2);
  return true;
}

// STP  <Xt1>, <Xt2>, [<Xn|SP>, #<imm>]!
bool TryDecodeSTP_64_LDSTPAIR_PRE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt2);
  uint64_t offset = static_cast<uint64_t>(data.imm7.simm7);
  AddPreIndexMemOp(inst, kActionWrite, 128, data.Rn, offset << 3);
  return true;
}

// STP  <Wt1>, <Wt2>, [<Xn|SP>], #<imm>
bool TryDecodeSTP_32_LDSTPAIR_POST(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt2);
  uint64_t offset = static_cast<uint64_t>(data.imm7.simm7);
  AddPostIndexMemOp(inst, kActionWrite, 64, data.Rn, offset << 2);
  return true;
}

// STP  <Xt1>, <Xt2>, [<Xn|SP>], #<imm>
bool TryDecodeSTP_64_LDSTPAIR_POST(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt2);
  uint64_t offset = static_cast<uint64_t>(data.imm7.simm7);
  AddPostIndexMemOp(inst, kActionWrite, 128, data.Rn, offset << 3);
  return true;
}

// STP  <Wt1>, <Wt2>, [<Xn|SP>{, #<imm>}]
bool TryDecodeSTP_32_LDSTPAIR_OFF(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt2);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 64, data.Rn,
                         static_cast<uint64_t>(data.imm7.simm7) << 2);
  return true;
}

// STP  <Xt1>, <Xt2>, [<Xn|SP>{, #<imm>}]
bool TryDecodeSTP_64_LDSTPAIR_OFF(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt2);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 128, data.Rn,
                         static_cast<uint64_t>(data.imm7.simm7) << 3);
  return true;
}

// LDP  <Wt1>, <Wt2>, [<Xn|SP>], #<imm>
bool TryDecodeLDP_32_LDSTPAIR_POST(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt2);
  AddPostIndexMemOp(inst, kActionRead, 64, data.Rn,
                    static_cast<uint64_t>(data.imm7.simm7) << 2);
  return true;
}

// LDP  <Xt1>, <Xt2>, [<Xn|SP>], #<imm>
bool TryDecodeLDP_64_LDSTPAIR_POST(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt2);
  AddPostIndexMemOp(inst, kActionRead, 128, data.Rn,
                    static_cast<uint64_t>(data.imm7.simm7) << 3);
  return true;
}

// LDP  <Wt1>, <Wt2>, [<Xn|SP>, #<imm>]!
bool TryDecodeLDP_32_LDSTPAIR_PRE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt2);
  AddPreIndexMemOp(inst, kActionRead, 64, data.Rn,
                   static_cast<uint64_t>(data.imm7.simm7) << 2);
  return true;
}

// LDP  <Xt1>, <Xt2>, [<Xn|SP>, #<imm>]!
bool TryDecodeLDP_64_LDSTPAIR_PRE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt2);
  AddPreIndexMemOp(inst, kActionRead, 128, data.Rn,
                   static_cast<uint64_t>(data.imm7.simm7) << 3);
  return true;
}

// LDP  <Wt1>, <Wt2>, [<Xn|SP>{, #<imm>}]
bool TryDecodeLDP_32_LDSTPAIR_OFF(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt2);
  AddBasePlusOffsetMemOp(inst, kActionRead, 64, data.Rn,
                         static_cast<uint64_t>(data.imm7.simm7) << 2);
  return true;
}

// LDP  <Xt1>, <Xt2>, [<Xn|SP>{, #<imm>}]
bool TryDecodeLDP_64_LDSTPAIR_OFF(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt);
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt2);
  AddBasePlusOffsetMemOp(inst, kActionRead, 128, data.Rn,
                         static_cast<uint64_t>(data.imm7.simm7) << 3);
  return true;
}

// LDR  <Wt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_32_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 32, data.Rn,
                         data.imm12.uimm << 2);
  return true;
}

// LDR  <Xt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_64_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 64, data.Rn,
                         data.imm12.uimm << 3);
  return true;
}

// LDR  <Wt>, <label>
bool TryDecodeLDR_32_LOADLIT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt);
  AddPCRegMemOp(inst, kActionRead,
                static_cast<uint64_t>(data.imm19.simm19) << 2ULL);
  return true;
}

// LDR  <Xt>, <label>
bool TryDecodeLDR_64_LOADLIT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt);
  AddPCRegMemOp(inst, kActionRead,
                static_cast<uint64_t>(data.imm19.simm19) << 2ULL);
  return true;
}

static bool TryDecodeLDR_n_LDST_REGOFF(
    const InstData &data, Instruction &inst, RegClass val_class) {
  if (!(data.option & 2)) {  // Sub word indexing.
    return false;  // `if option<1> == '0' then UnallocatedEncoding();`.
  }
  unsigned scale = data.size;
  auto shift = (data.S == 1) ? scale : 0U;
  auto extend_type = static_cast<Extend>(data.option);
  AddRegOperand(inst, kActionWrite, val_class, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 8U << scale, data.Rn, 0);
  AddExtendRegOperand(inst, val_class, kUseAsValue, data.Rm,
                      extend_type, 64, shift);
  return true;
}

// LDR  <Wt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
bool TryDecodeLDR_32_LDST_REGOFF(const InstData &data, Instruction &inst) {
  return TryDecodeLDR_n_LDST_REGOFF(data, inst, kRegW);
}

// LDR  <Xt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
bool TryDecodeLDR_64_LDST_REGOFF(const InstData &data, Instruction &inst) {
  return TryDecodeLDR_n_LDST_REGOFF(data, inst, kRegX);
}

// STR  <Wt>, [<Xn|SP>], #<simm>
bool TryDecodeSTR_32_LDST_IMMPOST(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  uint64_t offset = static_cast<uint64_t>(data.imm9.simm9);
  AddPostIndexMemOp(inst, kActionWrite, 32, data.Rn, offset << 2);
  return true;
}

// STR  <Xt>, [<Xn|SP>], #<simm>
bool TryDecodeSTR_64_LDST_IMMPOST(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt);
  uint64_t offset = static_cast<uint64_t>(data.imm9.simm9);
  AddPostIndexMemOp(inst, kActionWrite, 64, data.Rn, offset << 2);
  return true;
}

// STR  <Wt>, [<Xn|SP>, #<simm>]!
bool TryDecodeSTR_32_LDST_IMMPRE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  uint64_t offset = static_cast<uint64_t>(data.imm9.simm9);
  AddPreIndexMemOp(inst, kActionWrite, 32, data.Rn, offset << 2);
  return true;
}

// STR  <Xt>, [<Xn|SP>, #<simm>]!
bool TryDecodeSTR_64_LDST_IMMPRE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt);
  uint64_t offset = static_cast<uint64_t>(data.imm9.simm9);
  AddPreIndexMemOp(inst, kActionWrite, 64, data.Rn, offset << 2);
  return true;
}

// STR  <Wt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_32_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 32, data.Rn,
                         data.imm12.uimm << 2 /* size = 2 */);
  return true;
}

// STR  <Xt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_64_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 64, data.Rn,
                         data.imm12.uimm << 3 /* size = 3 */);
  return true;
}

static bool TryDecodeSTR_n_LDST_REGOFF(
    const InstData &data, Instruction &inst, RegClass val_class) {
  if (!(data.option & 2)) {  // Sub word indexing.
    return false;  // `if option<1> == '0' then UnallocatedEncoding();`.
  }
  unsigned scale = data.size;
  auto extend_type = static_cast<Extend>(data.option);
  auto shift = data.S ? scale : 0U;
  AddRegOperand(inst, kActionRead, val_class, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 8U << data.size, data.Rn, 0);
  AddExtendRegOperand(inst, val_class, kUseAsValue, data.Rm,
                      extend_type, 64, shift);
  return true;
}

// STR  <Wt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
bool TryDecodeSTR_32_LDST_REGOFF(const InstData &data, Instruction &inst) {
  return TryDecodeSTR_n_LDST_REGOFF(data, inst, kRegW);
}

// STR  <Xt>, [<Xn|SP>, (<Wm>|<Xm>){, <extend> {<amount>}}]
bool TryDecodeSTR_64_LDST_REGOFF(const InstData &data, Instruction &inst) {
  return TryDecodeSTR_n_LDST_REGOFF(data, inst, kRegX);
}

// MOVZ  <Wd>, #<imm>{, LSL #<shift>}
bool TryDecodeMOVZ_32_MOVEWIDE(const InstData &data, Instruction &inst) {
  if (data.hw & 2) {  // Also if `sf` is zero (specifies 32-bit operands).
    return false;
  }
  auto shift = static_cast<uint64_t>(data.hw) << 4U;
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddImmOperand(inst, static_cast<uint32_t>(data.imm16.uimm << shift),
                kUnsigned, 32);
  return true;
}

// MOVZ  <Xd>, #<imm>{, LSL #<shift>}
bool TryDecodeMOVZ_64_MOVEWIDE(const InstData &data, Instruction &inst) {

  auto shift = static_cast<uint64_t>(data.hw) << 4U;
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddImmOperand(inst, (data.imm16.uimm << shift));
  return true;
}

// MOVK  <Wd>, #<imm>{, LSL #<shift>}
bool TryDecodeMOVK_32_MOVEWIDE(const InstData &data, Instruction &inst) {
  if ((data.hw >> 1) & 1) {
    return false;  // if sf == '0' && hw<1> == '1' then UnallocatedEncoding();
  }
  AddRegOperand(inst, kActionReadWrite, kRegW, kUseAsValue, data.Rd);
  AddImmOperand(inst, data.imm16.uimm);
  AddImmOperand(inst, data.hw << 4, kUnsigned, 8);  // pos = UInt(hw:'0000');
  return true;
}

// MOVK  <Xd>, #<imm>{, LSL #<shift>}
bool TryDecodeMOVK_64_MOVEWIDE(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionReadWrite, kRegX, kUseAsValue, data.Rd);
  AddImmOperand(inst, data.imm16.uimm);
  AddImmOperand(inst, data.hw << 4, kUnsigned, 8);  // pos = UInt(hw:'0000');
  return true;
}

// MOVN  <Wd>, #<imm>{, LSL #<shift>}
bool TryDecodeMOVN_32_MOVEWIDE(const InstData &data, Instruction &inst) {
  if ((data.hw >> 1) & 1) {
    return false;  // if sf == '0' && hw<1> == '1' then UnallocatedEncoding();
  }
  auto shift = static_cast<uint64_t>(data.hw << 4);
  auto imm = data.imm16.uimm << shift;
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddImmOperand(inst, static_cast<uint64_t>(static_cast<uint32_t>(~imm)));
  return true;
}

// MOVN  <Xd>, #<imm>{, LSL #<shift>}
bool TryDecodeMOVN_64_MOVEWIDE(const InstData &data, Instruction &inst) {
  auto shift = static_cast<uint64_t>(data.hw << 4);
  auto imm = data.imm16.uimm << shift;
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddImmOperand(inst, ~imm);
  return true;
}

// ADR  <Xd>, <label>
bool TryDecodeADR_ONLY_PCRELADDR(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddPCDisp(inst, static_cast<int64_t>(data.immhi_immlo.simm21));
  return false;
}

// ADRP  <Xd>, <label>
bool TryDecodeADRP_ONLY_PCRELADDR(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddPCDisp(inst, static_cast<int64_t>(data.immhi_immlo.simm21) << 12ULL);
  return true;
}

// B  <label>
bool TryDecodeB_ONLY_BRANCH_IMM(const InstData &data, Instruction &inst) {
  AddPCDisp(inst, data.imm26.simm26 << 2LL);
  return true;
}

static void DecodeFallThroughPC(Instruction &inst) {
  Operand not_taken_op = {};
  not_taken_op.action = Operand::kActionRead;
  not_taken_op.type = Operand::kTypeAddress;
  not_taken_op.size = kPCWidth;
  not_taken_op.addr.address_size = kPCWidth;
  not_taken_op.addr.base_reg.name = "PC";
  not_taken_op.addr.base_reg.size = kPCWidth;
  not_taken_op.addr.displacement = kInstructionSize;
  not_taken_op.addr.kind = Operand::Address::kControlFlowTarget;
  inst.operands.push_back(not_taken_op);

  inst.branch_not_taken_pc = inst.next_pc;
}

// Decode a relative branch target.
static void DecodeConditionalBranch(Instruction &inst, int64_t disp) {

  // Condition variable.
  Operand cond_op = {};
  cond_op.action = Operand::kActionWrite;
  cond_op.type = Operand::kTypeRegister;
  cond_op.reg.name = "BRANCH_TAKEN";
  cond_op.reg.size = 8;
  cond_op.size = 8;
  inst.operands.push_back(cond_op);

  // Taken branch.
  Operand taken_op = {};
  taken_op.action = Operand::kActionRead;
  taken_op.type = Operand::kTypeAddress;
  taken_op.size = kPCWidth;
  taken_op.addr.address_size = kPCWidth;
  taken_op.addr.base_reg.name = "PC";
  taken_op.addr.base_reg.size = kPCWidth;
  taken_op.addr.displacement = disp;
  taken_op.addr.kind = Operand::Address::kControlFlowTarget;
  inst.operands.push_back(taken_op);

  inst.branch_taken_pc = static_cast<uint64_t>(
      static_cast<int64_t>(inst.pc) + disp);

  DecodeFallThroughPC(inst);
}

static bool DecodeBranchRegLabel(const InstData &data, Instruction &inst,
                                 RegClass reg_class) {
  DecodeConditionalBranch(inst, data.imm19.simm19 << 2);
  AddRegOperand(inst, kActionRead, reg_class, kUseAsValue, data.Rt);
  return true;
}

// CBZ  <Wt>, <label>
bool TryDecodeCBZ_32_COMPBRANCH(const InstData &data, Instruction &inst) {
  return DecodeBranchRegLabel(data, inst, kRegW);
}

// CBZ  <Xt>, <label>
bool TryDecodeCBZ_64_COMPBRANCH(const InstData &data, Instruction &inst) {
  return DecodeBranchRegLabel(data, inst, kRegX);
}

// CBNZ  <Wt>, <label>
bool TryDecodeCBNZ_32_COMPBRANCH(const InstData &data, Instruction &inst) {
  return DecodeBranchRegLabel(data, inst, kRegW);
}

// CBNZ  <Xt>, <label>
bool TryDecodeCBNZ_64_COMPBRANCH(const InstData &data, Instruction &inst) {
  return DecodeBranchRegLabel(data, inst, kRegX);
}

bool DecodeTestBitBranch(const InstData &data, Instruction &inst) {
  uint8_t bit_pos = uint8_t (data.b5 << 5U) | data.b40;
  AddImmOperand(inst, bit_pos);
  DecodeConditionalBranch(inst, data.imm14.simm14 << 2);
  RegClass reg_class;
  if(data.b5 == 1) {
    reg_class = kRegX;
    inst.function += "_64";
  } else {
    reg_class = kRegW;
    inst.function += "_32";
  }
  AddRegOperand(inst, kActionRead, reg_class, kUseAsValue, data.Rt);
  return true;
}
// TBZ  <R><t>, #<imm>, <label>
bool TryDecodeTBZ_ONLY_TESTBRANCH(const InstData &data, Instruction &inst) {
  return DecodeTestBitBranch(data, inst);
}
// TBNZ  <R><t>, #<imm>, <label>
bool TryDecodeTBNZ_ONLY_TESTBRANCH(const InstData &data, Instruction &inst) {
  return DecodeTestBitBranch(data, inst);
}

// BL  <label>
bool TryDecodeBL_ONLY_BRANCH_IMM(const InstData &data, Instruction &inst) {
  AddPCDisp(inst, data.imm26.simm26 << 2LL);
  AddNextPC(inst);  // Decodes the return address.
  return true;
}

// BR  <Xn>
bool TryDecodeBR_64_BRANCH_REG(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegX, kUseAsAddress, data.Rn);
  return true;
}


static bool ShiftImmediate(uint64_t &value, uint8_t shift) {
  switch (shift) {
    case 0:  // Shift 0 to left.
      break;
    case 1:  // Shift left 12 bits.
      value = value << 12;
      break;
    default:
      LOG(ERROR)
          << "Decoding reserved bit for shift value.";
      return false;
  }
  return true;
}

// ADD  <Wd|WSP>, <Wn|WSP>, #<imm>{, <shift>}
bool TryDecodeADD_32_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  auto imm = data.imm12.uimm;
  if (!ShiftImmediate(imm, data.shift)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsAddress, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsAddress, data.Rn);
  AddImmOperand(inst, imm);
  return true;
}

// ADD  <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
bool TryDecodeADD_64_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  auto imm = data.imm12.uimm;
  if (!ShiftImmediate(imm, data.shift)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsAddress, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsAddress, data.Rn);
  AddImmOperand(inst, imm);
  return true;
}

// ADD  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeADD_32_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  if (1 & (data.imm6.uimm >> 5)) {
    return false;  // `if sf == '0' && imm6<5> == '1' then ReservedValue();`.
  }
  auto shift_type = static_cast<Shift>(data.shift);
  if (shift_type == kShiftROR) {
    return false;  // Shift type '11' is a reserved value.
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddShiftRegOperand(inst, kRegW, kUseAsValue, data.Rm,
                     shift_type, data.imm6.uimm);
  return true;
}

// ADD  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeADD_64_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  auto shift_type = static_cast<Shift>(data.shift);
  if (shift_type == kShiftROR) {
    return false;  // Shift type '11' is a reserved value.
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddShiftRegOperand(inst, kRegX, kUseAsValue, data.Rm,
                     shift_type, data.imm6.uimm);
  return true;
}

// ADD  <Wd|WSP>, <Wn|WSP>, <Wm>{, <extend> {#<amount>}}
bool TryDecodeADD_32_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  auto extend_type = static_cast<Extend>(data.option);
  auto shift = data.imm3.uimm;
  if (shift > 4) {
    return false;  // `if shift > 4 then ReservedValue();`.
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsAddress, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsAddress, data.Rn);
  AddExtendRegOperand(inst, kRegW, kUseAsValue,
                      data.Rm, extend_type, 32, shift);
  return true;
}

// ADD  <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
bool TryDecodeADD_64_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  auto extend_type = static_cast<Extend>(data.option);
  auto shift = data.imm3.uimm;
  if (shift > 4) {
    return false;  // `if shift > 4 then ReservedValue();`.
  }
  auto reg_class = ExtendTypeToRegClass(extend_type);
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsAddress, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsAddress, data.Rn);
  AddExtendRegOperand(inst, reg_class, kUseAsValue,
                      data.Rm, extend_type, 64, shift);
  return true;
}

// SUB  <Wd|WSP>, <Wn|WSP>, #<imm>{, <shift>}
bool TryDecodeSUB_32_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeADD_32_ADDSUB_IMM(data, inst);
}

// SUB  <Xd|SP>, <Xn|SP>, #<imm>{, <shift>}
bool TryDecodeSUB_64_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeADD_64_ADDSUB_IMM(data, inst);
}

// SUB  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeSUB_32_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeADD_32_ADDSUB_SHIFT(data, inst);
}

// SUB  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeSUB_64_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeADD_64_ADDSUB_SHIFT(data, inst);
}

// SUB  <Wd|WSP>, <Wn|WSP>, <Wm>{, <extend> {#<amount>}}
bool TryDecodeSUB_32_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  return TryDecodeADD_32_ADDSUB_EXT(data, inst);
}

// SUB  <Xd|SP>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
bool TryDecodeSUB_64_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  return TryDecodeADD_64_ADDSUB_EXT(data, inst);
}

// SUBS  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeSUBS_32_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  auto shift_type = static_cast<Shift>(data.shift);
  if (shift_type == kShiftROR) {
    return false;  // Shift type '11' is a reserved value.
  } else if ((data.imm6.uimm >> 5) & 1) {
    return false;  // `if sf == '0' && imm6<5> == '1' then ReservedValue();`.
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddShiftRegOperand(inst, kRegW, kUseAsValue, data.Rm, shift_type,
                     data.imm6.uimm);
  return true;
}

// SUBS  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeSUBS_64_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  auto shift_type = static_cast<Shift>(data.shift);
  if (shift_type == kShiftROR) {
    return false;  // Shift type '11' is a reserved value.
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddShiftRegOperand(inst, kRegX, kUseAsValue, data.Rm, shift_type,
                     data.imm6.uimm);
  return true;
}

// SUBS  <Wd>, <Wn|WSP>, #<imm>{, <shift>}
bool TryDecodeSUBS_32S_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  auto imm = data.imm12.uimm;
  if (!ShiftImmediate(imm, data.shift)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsAddress, data.Rn);
  AddImmOperand(inst, imm);
  return true;
}

// SUBS  <Xd>, <Xn|SP>, #<imm>{, <shift>}
bool TryDecodeSUBS_64S_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  auto imm = data.imm12.uimm;
  if (!ShiftImmediate(imm, data.shift)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsAddress, data.Rn);
  AddImmOperand(inst, imm);
  return true;
}

// SUBS  <Wd>, <Wn|WSP>, <Wm>{, <extend> {#<amount>}}
bool TryDecodeSUBS_32S_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  auto extend_type = static_cast<Extend>(data.option);
  auto shift = data.imm3.uimm;
  if (shift > 4) {
    return false;  // `if shift > 4 then ReservedValue();`.
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsAddress, data.Rn);
  AddExtendRegOperand(inst, kRegW, kUseAsValue,
                      data.Rm, extend_type, 32, shift);
  return true;
}

// SUBS  <Xd>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
bool TryDecodeSUBS_64S_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  auto extend_type = static_cast<Extend>(data.option);
  auto shift = data.imm3.uimm;
  if (shift > 4) {
    return false;  // `if shift > 4 then ReservedValue();`.
  }
  auto reg_class = ExtendTypeToRegClass(extend_type);
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsAddress, data.Rn);
  AddExtendRegOperand(inst, reg_class, kUseAsValue,
                      data.Rm, extend_type, 64, shift);
  return true;
}

// ADDS  <Wd>, <Wn|WSP>, #<imm>{, <shift>}
bool TryDecodeADDS_32S_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeSUBS_32S_ADDSUB_IMM(data, inst);
}

// ADDS  <Xd>, <Xn|SP>, #<imm>{, <shift>}
bool TryDecodeADDS_64S_ADDSUB_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeSUBS_64S_ADDSUB_IMM(data, inst);
}

// ADDS  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeADDS_32_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeSUBS_32_ADDSUB_SHIFT(data, inst);
}

// ADDS  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeADDS_64_ADDSUB_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeSUBS_64_ADDSUB_SHIFT(data, inst);
}

// ADDS  <Wd>, <Wn|WSP>, <Wm>{, <extend> {#<amount>}}
bool TryDecodeADDS_32S_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  return TryDecodeSUBS_32S_ADDSUB_EXT(data, inst);
}

// ADDS  <Xd>, <Xn|SP>, <R><m>{, <extend> {#<amount>}}
bool TryDecodeADDS_64S_ADDSUB_EXT(const InstData &data, Instruction &inst) {
  return TryDecodeSUBS_64S_ADDSUB_EXT(data, inst);
}

static const char *kCondName[] = {
    "EQ", "CS", "MI", "VS", "HI", "GE", "GT", "AL"
};

static const char *kNegCondName[] = {
    "NE", "CC", "PL", "VC", "LS", "LT", "LE", "AL"
};

static const char *CondName(uint8_t cond) {
  if (cond & 1) {
    return kNegCondName[(cond >> 1) & 0x7];
  } else {
    return kCondName[(cond >> 1) & 0x7];
  }
}

void SetConditionalFunctionName(uint8_t cond, Instruction &inst) {
  std::stringstream ss;
  ss << inst.function << "_" << CondName(cond);
  inst.function = ss.str();
}

// B.<cond>  <label>
bool TryDecodeB_ONLY_CONDBRANCH(const InstData &data, Instruction &inst) {

  // Add in the condition to the isel name.
  SetConditionalFunctionName(data.cond, inst);
  DecodeConditionalBranch(inst, data.imm19.simm19 << 2);
  return true;
}

// STRB  <Wt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTRB_32_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 8, data.Rn,
                         data.imm12.uimm);
  return true;
}

// LDRB  <Wt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDRB_32_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 8, data.Rn,
                         data.imm12.uimm);
  return true;
}

// STRH  <Wt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTRH_32_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 16, data.Rn,
                         data.imm12.uimm << 1);
  return true;
}

// ORN  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeORN_32_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_32_LOG_SHIFT(data, inst);
}

// ORN  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeORN_64_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_64_LOG_SHIFT(data, inst);
}

// EOR  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeEOR_32_LOG_SHIFT(const InstData &data, Instruction &inst) {
  if (1 & (data.imm6.uimm >> 5)) {
    return false;  // `if sf == '0' && imm6<5> == '1' then ReservedValue();`.
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddShiftRegOperand(inst, kRegW, kUseAsValue, data.Rm,
                     static_cast<Shift>(data.shift), data.imm6.uimm);
  return true;
}

// EOR  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeEOR_64_LOG_SHIFT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddShiftRegOperand(inst, kRegX, kUseAsValue, data.Rm,
                     static_cast<Shift>(data.shift), data.imm6.uimm);
  return true;
}

// EOR  <Wd|WSP>, <Wn>, #<imm>
bool TryDecodeEOR_32_LOG_IMM(const InstData &data, Instruction &inst) {
  uint64_t wmask = 0;
  if (data.N) {
    return false;  // `if sf == '0' && N != '0' then ReservedValue();`.
  }
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      true, 32, &wmask)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsAddress, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddImmOperand(inst, wmask, kUnsigned, 32);
  return true;
}

// EOR  <Xd|SP>, <Xn>, #<imm>
bool TryDecodeEOR_64_LOG_IMM(const InstData &data, Instruction &inst) {
  uint64_t wmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      true, 64, &wmask)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsAddress, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddImmOperand(inst, wmask, kUnsigned, 64);
  return true;
}

// AND  <Wd|WSP>, <Wn>, #<imm>
bool TryDecodeAND_32_LOG_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_32_LOG_IMM(data, inst);
}

// AND  <Xd|SP>, <Xn>, #<imm>
bool TryDecodeAND_64_LOG_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_64_LOG_IMM(data, inst);
}

// AND  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeAND_32_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_32_LOG_SHIFT(data, inst);
}

// AND  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeAND_64_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_64_LOG_SHIFT(data, inst);
}

// ORR  <Wd|WSP>, <Wn>, #<imm>
bool TryDecodeORR_32_LOG_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_32_LOG_IMM(data, inst);
}

// ORR  <Xd|SP>, <Xn>, #<imm>
bool TryDecodeORR_64_LOG_IMM(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_64_LOG_IMM(data, inst);
}

// ORR  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeORR_32_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_32_LOG_SHIFT(data, inst);
}

// ORR  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeORR_64_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_64_LOG_SHIFT(data, inst);
}

// BIC  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeBIC_32_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_32_LOG_SHIFT(data, inst);
}

// BIC  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeBIC_64_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeEOR_64_LOG_SHIFT(data, inst);
}

// LDUR  <Wt>, [<Xn|SP>{, #<simm>}]
bool TryDecodeLDUR_32_LDST_UNSCALED(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 32, data.Rn,
                         static_cast<uint64_t>(data.imm9.simm9));
  return true;
}

// LDUR  <Xt>, [<Xn|SP>{, #<simm>}]
bool TryDecodeLDUR_64_LDST_UNSCALED(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 64, data.Rn,
                         static_cast<uint64_t>(data.imm9.simm9));
  return true;
}

// HINT  #<imm>
bool TryDecodeHINT_1(const InstData &, Instruction &) {
  return true;  // NOP.
}

// HINT  #<imm>
bool TryDecodeHINT_2(const InstData &, Instruction &) {
  return true;  // NOP.
}

// HINT  #<imm>
bool TryDecodeHINT_3(const InstData &, Instruction &) {
  return true;  // NOP.
}

// UMADDL  <Xd>, <Wn>, <Wm>, <Xa>
bool TryDecodeUMADDL_64WA_DP_3SRC(const InstData &data,
                                  Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rm);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Ra);
  return true;
}

// UMULH  <Xd>, <Xn>, <Xm>
bool TryDecodeUMULH_64_DP_3SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rm);
  return true;
}

// SMADDL  <Xd>, <Wn>, <Wm>, <Xa>
bool TryDecodeSMADDL_64WA_DP_3SRC(const InstData &data, Instruction &inst) {
  return TryDecodeUMADDL_64WA_DP_3SRC(data, inst);
}

// SMULH  <Xd>, <Xn>, <Xm>
bool TryDecodeSMULH_64_DP_3SRC(const InstData &data, Instruction &inst) {
  return TryDecodeUMULH_64_DP_3SRC(data, inst);
}

// UDIV  <Wd>, <Wn>, <Wm>
bool TryDecodeUDIV_32_DP_2SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rm);
  return true;
}

// UDIV  <Xd>, <Xn>, <Xm>
bool TryDecodeUDIV_64_DP_2SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rm);
  return true;
}

// UBFM  <Wd>, <Wn>, #<immr>, #<imms>
bool TryDecodeUBFM_32M_BITFIELD(const InstData &data, Instruction &inst) {

  // if sf == '0' && (N != '0' || immr<5> != '0' || imms<5> != '0')
  //    then ReservedValue();
  if (data.N || (data.immr.uimm & 0x20) || (data.imms.uimm & 0x20)) {
    return false;
  }

  uint64_t wmask = 0;
  uint64_t tmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      false, 32, &wmask, &tmask)) {
    return false;
  }

  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddShiftRegOperand(inst, kRegW, kUseAsValue, data.Rn,
                     kShiftROR, data.immr.uimm);
  AddImmOperand(inst, wmask & tmask, kUnsigned, 32);
  return true;
}

// UBFM  <Xd>, <Xn>, #<immr>, #<imms>
bool TryDecodeUBFM_64M_BITFIELD(const InstData &data, Instruction &inst) {
  if (!data.N) {
    return false;  // `if sf == '1' && N != '1' then ReservedValue();`.
  }

  uint64_t wmask = 0;
  uint64_t tmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      false, 64, &wmask, &tmask)) {
    return false;
  }

  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddShiftRegOperand(inst, kRegX, kUseAsValue, data.Rn,
                     kShiftROR, data.immr.uimm);
  AddImmOperand(inst, wmask & tmask, kUnsigned, 64);
  return true;
}

// SBFM  <Wd>, <Wn>, #<immr>, #<imms>
bool TryDecodeSBFM_32M_BITFIELD(const InstData &data, Instruction &inst) {
  // if sf == '0' && (N != '0' || immr<5> != '0' || imms<5> != '0')
  //    then ReservedValue();
  if (data.N || (data.immr.uimm & 0x20) || (data.imms.uimm & 0x20)) {
    return false;
  }
  uint64_t wmask = 0;
  uint64_t tmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      false, 32, &wmask, &tmask)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddImmOperand(inst, data.immr.uimm, kUnsigned, 32);
  AddImmOperand(inst, data.imms.uimm, kUnsigned, 32);
  AddImmOperand(inst, wmask, kUnsigned, 32);
  AddImmOperand(inst, tmask, kUnsigned, 32);
  return true;
}

// SBFM  <Xd>, <Xn>, #<immr>, #<imms>
bool TryDecodeSBFM_64M_BITFIELD(const InstData &data, Instruction &inst) {
  if (!data.N) {
    return false;  // `if sf == '1' && N != '1' then ReservedValue();`.
  }
  uint64_t wmask = 0;
  uint64_t tmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      false, 64, &wmask, &tmask)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddImmOperand(inst, data.immr.uimm, kUnsigned, 64);
  AddImmOperand(inst, data.imms.uimm, kUnsigned, 64);
  AddImmOperand(inst, wmask, kUnsigned, 64);
  AddImmOperand(inst, tmask, kUnsigned, 64);
  return true;
}

// BFM  <Wd>, <Wn>, #<immr>, #<imms>
bool TryDecodeBFM_32M_BITFIELD(const InstData &data, Instruction &inst) {
  // if sf == '0' && (N != '0' || immr<5> != '0' || imms<5> != '0')
  //    then ReservedValue();
  if (data.N || (data.immr.uimm & 0x20) || (data.imms.uimm & 0x20)) {
    return false;
  }
  uint64_t wmask = 0;
  uint64_t tmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      false, 32, &wmask, &tmask)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddImmOperand(inst, data.immr.uimm, kUnsigned, 32);
  AddImmOperand(inst, wmask, kUnsigned, 32);
  AddImmOperand(inst, tmask, kUnsigned, 32);
  return true;
}

// BFM  <Xd>, <Xn>, #<immr>, #<imms>
bool TryDecodeBFM_64M_BITFIELD(const InstData &data, Instruction &inst) {
  if (!data.N) {
    return false;  // `if sf == '1' && N != '1' then ReservedValue();`.
  }
  uint64_t wmask = 0;
  uint64_t tmask = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      false, 64, &wmask, &tmask)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddImmOperand(inst, data.immr.uimm, kUnsigned, 64);
  AddImmOperand(inst, wmask, kUnsigned, 64);
  AddImmOperand(inst, tmask, kUnsigned, 64);
  return true;
}

// ANDS  <Wd>, <Wn>, #<imm>
bool TryDecodeANDS_32S_LOG_IMM(const InstData &data, Instruction &inst) {
  if (data.N) {
    return false;  // `if sf == '0' && N != '0' then ReservedValue();`.
  }
  uint64_t imm = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      true, 32, &imm)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddImmOperand(inst, imm, kUnsigned, 32);
  return true;
}

// ANDS  <Xd>, <Xn>, #<imm>
bool TryDecodeANDS_64S_LOG_IMM(const InstData &data, Instruction &inst) {
  uint64_t imm = 0;
  if (!DecodeBitMasks(data.N, data.imms.uimm, data.immr.uimm,
                      true, 64, &imm)) {
    return false;
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddImmOperand(inst, imm, kUnsigned, 64);
  return true;
}

// ANDS  <Wd>, <Wn>, <Wm>{, <shift> #<amount>}
bool TryDecodeANDS_32_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeAND_32_LOG_SHIFT(data, inst);
}

// ANDS  <Xd>, <Xn>, <Xm>{, <shift> #<amount>}
bool TryDecodeANDS_64_LOG_SHIFT(const InstData &data, Instruction &inst) {
  return TryDecodeAND_64_LOG_SHIFT(data, inst);
}

// MADD  <Wd>, <Wn>, <Wm>, <Wa>
bool TryDecodeMADD_32A_DP_3SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rm);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Ra);
  return true;
}

// MADD  <Xd>, <Xn>, <Xm>, <Xa>
bool TryDecodeMADD_64A_DP_3SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rm);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Ra);
  return true;
}

// EXTR  <Wd>, <Wn>, <Wm>, #<lsb>
bool TryDecodeEXTR_32_EXTRACT(const InstData &data, Instruction &inst) {
  if (data.N != data.sf) {
    return false;  // `if N != sf then UnallocatedEncoding();`
  }
  if (data.imms.uimm & 0x20) {
    return false;  // `if sf == '0' && imms<5> == '1' then ReservedValue();`
  }
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rm);
  AddImmOperand(inst, data.imms.uimm, kUnsigned, 32);
  return true;
}

// EXTR  <Xd>, <Xn>, <Xm>, #<lsb>
bool TryDecodeEXTR_64_EXTRACT(const InstData &data, Instruction &inst) {
  if (data.N != data.sf) {
    return false;  // `if N != sf then UnallocatedEncoding();`
  }
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rm);
  AddImmOperand(inst, data.imms.uimm, kUnsigned, 64);
  return true;
}

// LSLV  <Wd>, <Wn>, <Wm>
bool TryDecodeLSLV_32_DP_2SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rm);
  return true;
}

// LSLV  <Xd>, <Xn>, <Xm>
bool TryDecodeLSLV_64_DP_2SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rm);
  return true;
}

// LSRV  <Wd>, <Wn>, <Wm>
bool TryDecodeLSRV_32_DP_2SRC(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_32_DP_2SRC(data, inst);
}

// LSRV  <Xd>, <Xn>, <Xm>
bool TryDecodeLSRV_64_DP_2SRC(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_64_DP_2SRC(data, inst);
}

// ASRV  <Wd>, <Wn>, <Wm>
bool TryDecodeASRV_32_DP_2SRC(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_32_DP_2SRC(data, inst);
}

// ASRV  <Xd>, <Xn>, <Xm>
bool TryDecodeASRV_64_DP_2SRC(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_64_DP_2SRC(data, inst);
}

// RORV  <Wd>, <Wn>, <Wm>
bool TryDecodeRORV_32_DP_2SRC(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_32_DP_2SRC(data, inst);
}

// RORV  <Xd>, <Xn>, <Xm>
bool TryDecodeRORV_64_DP_2SRC(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_64_DP_2SRC(data, inst);
}

// SBC  <Wd>, <Wn>, <Wm>
bool TryDecodeSBC_32_ADDSUB_CARRY(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_32_DP_2SRC(data, inst);
}

// SBC  <Xd>, <Xn>, <Xm>
bool TryDecodeSBC_64_ADDSUB_CARRY(const InstData &data, Instruction &inst) {
  return TryDecodeLSLV_64_DP_2SRC(data, inst);
}

// SBCS  <Wd>, <Wn>, <Wm>
bool TryDecodeSBCS_32_ADDSUB_CARRY(const InstData &data, Instruction &inst) {
  return TryDecodeSBC_32_ADDSUB_CARRY(data, inst);
}

// SBCS  <Xd>, <Xn>, <Xm>
bool TryDecodeSBCS_64_ADDSUB_CARRY(const InstData &data, Instruction &inst) {
  return TryDecodeSBC_64_ADDSUB_CARRY(data, inst);
}

// UCVTF  <Hd>, <Wn>
bool TryDecodeUCVTF_H32_FLOAT2INT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegH, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  return true;
}

// UCVTF  <Sd>, <Wn>
bool TryDecodeUCVTF_S32_FLOAT2INT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegS, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  return true;
}

// UCVTF  <Dd>, <Wn>
bool TryDecodeUCVTF_D32_FLOAT2INT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegD, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  return true;
}

// UCVTF  <Hd>, <Xn>
bool TryDecodeUCVTF_H64_FLOAT2INT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegH, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  return true;
}

// UCVTF  <Sd>, <Xn>
bool TryDecodeUCVTF_S64_FLOAT2INT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegS, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  return true;
}

// UCVTF  <Dd>, <Xn>
bool TryDecodeUCVTF_D64_FLOAT2INT(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegD, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  return true;
}

// SVC  #<imm>
bool TryDecodeSVC_EX_EXCEPTION(const InstData &data, Instruction &inst) {
  AddImmOperand(inst, data.imm16.uimm, kUnsigned, 32);
  return true;
}

// BRK  #<imm>
bool TryDecodeBRK_EX_EXCEPTION(const InstData &data, Instruction &inst) {
  AddImmOperand(inst, data.imm16.uimm, kUnsigned, 32);
  return true;
}

// MRS  <Xt>, (<systemreg>|S<op0>_<op1>_<Cn>_<Cm>_<op2>)
bool TryDecodeMRS_RS_SYSTEM(const InstData &, Instruction &) {
  return false;
}

// STR  <Bt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_B_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegB, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 8, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 0);
  return true;
}

// STR  <Ht>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_H_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegH, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 16, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 1);
  return true;
}

// STR  <St>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_S_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegS, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 32, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 2);
  return true;
}

// STR  <Dt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_D_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegD, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 64, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 3);
  return true;
}

// STR  <Qt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeSTR_Q_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionRead, kRegQ, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionWrite, 128, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 4);
  return true;
}

// LDR  <Bt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_B_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegB, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 8, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 0);
  return true;
}

// LDR  <Ht>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_H_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegH, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 16, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 1);
  return true;
}

// LDR  <St>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_S_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegS, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 32, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 2);
  return true;
}

// LDR  <Dt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_D_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegD, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 64, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 3);
  return true;
}

// LDR  <Qt>, [<Xn|SP>{, #<pimm>}]
bool TryDecodeLDR_Q_LDST_POS(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegQ, kUseAsValue, data.Rt);
  AddBasePlusOffsetMemOp(inst, kActionRead, 128, data.Rn,
                         static_cast<uint64_t>(data.imm12.uimm) << 4);
  return true;
}


// CLZ  <Wd>, <Wn>
bool TryDecodeCLZ_32_DP_1SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegW, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegW, kUseAsValue, data.Rn);
  return true;
}

// CLZ  <Xd>, <Xn>
bool TryDecodeCLZ_64_DP_1SRC(const InstData &data, Instruction &inst) {
  AddRegOperand(inst, kActionWrite, kRegX, kUseAsValue, data.Rd);
  AddRegOperand(inst, kActionRead, kRegX, kUseAsValue, data.Rn);
  return true;
}


bool DecodeConditionalRegSelect(
  const InstData &data, 
  Instruction &inst, 
  RegClass r_class, 
  int8_t n_regs, // signed because otherwise we underflow
  bool invert_cond = false
) {

  if (!(1 <= n_regs && n_regs <= 3)) {
    LOG(ERROR) << "Number of registers in conditional select must be 1 <= x <= 3";
    return false;
  }

  AddRegOperand(inst, kActionWrite, r_class, kUseAsValue, data.Rd);
  if (--n_regs > 0) { 
    AddRegOperand(inst, kActionRead, r_class, kUseAsValue, data.Rn); 
  }
  if (--n_regs > 0) { 
    AddRegOperand(inst, kActionRead, r_class, kUseAsValue, data.Rm); 
  }

  uint8_t cond;
  if (invert_cond) { 
    cond = data.cond ^ 1;
  } else {
    cond = data.cond;
  }

  // Condition will be part of the isel, not an operand
  SetConditionalFunctionName(cond, inst);
  return true;
}

// CSEL  <Wd>, <Wn>, <Wm>, <cond>
bool TryDecodeCSEL_32_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegW, 3);
}

// CSEL  <Xd>, <Xn>, <Xm>, <cond>
bool TryDecodeCSEL_64_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegX, 3);
}

// CSINC  <Wd>, <Wn>, <Wm>, <cond>
bool TryDecodeCSINC_32_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegW, 3);
}

// CSINC  <Xd>, <Xn>, <Xm>, <cond>
bool TryDecodeCSINC_64_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegX, 3);
}

//////////////////////// DEPRICATED ALIAS
// CINC  <Wd>, <Wn>, <cond>
bool TryDecodeCINC_CSINC_32_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}

// CINC  <Xd>, <Xn>, <cond>
bool TryDecodeCINC_CSINC_64_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}

// CSET  <Wd>, <cond>
bool TryDecodeCSET_CSINC_32_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}

// CSET  <Xd>, <cond>
bool TryDecodeCSET_CSINC_64_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}
//////////////////////////////////////////

// CSINV  <Wd>, <Wn>, <Wm>, <cond>
bool TryDecodeCSINV_32_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegW, 3);
}

// CSINV  <Xd>, <Xn>, <Xm>, <cond>
bool TryDecodeCSINV_64_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegX, 3);
}

///////////////// DEPRICATED ALIAS
// CINV  <Wd>, <Wn>, <cond>
bool TryDecodeCINV_CSINV_32_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}

// CINV  <Xd>, <Xn>, <cond>
bool TryDecodeCINV_CSINV_64_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}

// CSETM  <Wd>, <cond>
bool TryDecodeCSETM_CSINV_32_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}

// CSETM  <Xd>, <cond>
bool TryDecodeCSETM_CSINV_64_CONDSEL(const InstData &data, Instruction &inst) {
  return false;
}
///////////////////////////////////

// CSNEG  <Wd>, <Wn>, <Wm>, <cond>
bool TryDecodeCSNEG_32_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegW, 3);
}

// CSNEG  <Xd>, <Xn>, <Xm>, <cond>
bool TryDecodeCSNEG_64_CONDSEL(const InstData &data, Instruction &inst) {
  return DecodeConditionalRegSelect(data, inst, kRegX, 3);
}

// CCMP  <Wn>, #<imm>, #<nzcv>, <cond>
bool TryDecodeCCMP_32_CONDCMP_IMM(const InstData &data, Instruction &inst) {
  DecodeConditionalRegSelect(data, inst, kRegW, 1);
  AddImmOperand(inst, data.imm5.uimm);
  AddImmOperand(inst, data.nzcv);
  return true;
}

// CCMP CCMP_64_condcmp_imm:
//   0 x nzcv     0
//   1 x nzcv     1
//   2 x nzcv     2
//   3 x nzcv     3
//   4 0 o3       0
//   5 x Rn       0
//   6 x Rn       1
//   7 x Rn       2
//   8 x Rn       3
//   9 x Rn       4
//  10 0 o2       0
//  11 1
//  12 x cond     0
//  13 x cond     1
//  14 x cond     2
//  15 x cond     3
//  16 x imm5     0
//  17 x imm5     1
//  18 x imm5     2
//  19 x imm5     3
//  20 x imm5     4
//  21 0
//  22 1
//  23 0
//  24 0
//  25 1
//  26 0
//  27 1
//  28 1
//  29 1 S        0
//  30 1 op       0
//  31 1 sf       0
// CCMP  <Xn>, #<imm>, #<nzcv>, <cond>
bool TryDecodeCCMP_64_CONDCMP_IMM(const InstData &, Instruction &) {
  return false;
}


}  // namespace aarch64

// TODO(pag): We pretend that these are singletons, but they aren't really!
const Arch *Arch::GetAArch64(
    OSName os_name_, ArchName arch_name_) {
//  aarch64::InstData data;
//  for (uint64_t i = 0; i < 0xFFFFFFFFULL; ++i) {
//    uint32_t bits = static_cast<uint32_t>(i);
//    if (aarch64::TryExtractBFM_32M_BITFIELD(data, bits)) {
//      if (data.iform != aarch64::InstForm::BFM_32M_BITFIELD) {
//        continue;
//      }
//      if (data.Rd == 3 && data.Rn == 0) {
//        remill::Instruction inst;
//        if (!aarch64::TryDecode(data, inst)) {
//          continue;
//        }
//        LOG(ERROR)
//            << "MAKE_BFM_TEST(" << aarch64::InstFormToString(data.iform)
//            << ", " << aarch64::InstNameToString(data.iclass) << "_w3_w0_"
//            << std::hex << data.immr.uimm << "_" << data.imms.uimm << ", 0x"
//            << std::hex << std::setw(2) << std::setfill('0')
//            << ((bits >> 0) & 0xFF) << ", 0x" << ((bits >> 8) & 0xFF)
//            << ", 0x" << ((bits >> 16) & 0xFF) << ", 0x"
//            << ((bits >> 24) & 0xFF) << ")";
//      }
//    }
//  }
  return new AArch64Arch(os_name_, arch_name_);
}

}  // namespace remill
