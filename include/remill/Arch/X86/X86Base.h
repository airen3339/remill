
#pragma once
#include <remill/Arch/Arch.h>
#include <remill/Arch/ArchBase.h>
// clang-format off
#define HAS_FEATURE_AVX 1
#define HAS_FEATURE_AVX512 1
#define ADDRESS_SIZE_BITS 64
#define INCLUDED_FROM_REMILL
#include <remill/Arch/X86/Runtime/State.h>
#include <sleigh/libsleigh.hh>
// clang-format on

#include <string>
namespace remill {
/// Class to derive from to handle x86 addregs
class X86ArchBase : ArchBase {

  std::string_view StackPointerRegisterName(void) const final;

  std::string_view ProgramCounterRegisterName(void) const final;
  uint64_t MinInstructionAlign(void) const final;


  uint64_t MinInstructionSize(void) const final;

  uint64_t MaxInstructionSize(bool) const final;
  llvm::CallingConv::ID DefaultCallingConv(void) const final;

  llvm::DataLayout DataLayout(void) const final;

  llvm::Triple Triple(void) const;


  void PopulateRegisterTable(void) const final;
  // Populate a just-initialized lifted function function with architecture-
  // specific variables.
  void FinishLiftedFunctionInitialization(llvm::Module *module,
                                          llvm::Function *bb_func) const final;
};
}  // namespace remill