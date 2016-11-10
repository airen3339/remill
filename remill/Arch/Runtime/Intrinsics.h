/* Copyright 2015 Peter Goodman (peter@trailofbits.com), all rights reserved. */

#ifndef REMILL_ARCH_RUNTIME_INTRINSICS_H_
#define REMILL_ARCH_RUNTIME_INTRINSICS_H_

#include "remill/Arch/Runtime/Types.h"

struct IndirectBlock final {
  const uint64_t lifted_address;
  void (* const lifted_func)(State &, Memory *, addr_t);
};

// TODO(pag): Add a `lifted_address` field in here for extra cross-checking?
struct NamedBlock final {
  const char * const name;
  void (* const lifted_func)(State &, Memory *, addr_t);
  void (* const native_func)(void);
};

enum class SyncHyperCall : uint32_t {
  kInvalid,
  kX86CPUID,
  kX86ReadTSC,
  kX86ReadTSCP
};

enum class AsynchHyperCall : uint32_t {
  kInvalid,

  // Interrupts calls.
  kX86Int1,
  kX86Int3,
  kX86IntO,
  kX86IntN,
  kX86Bound,

  // Interrupt returns.
  kX86IRet,

  // System calls.
  kX86SysCall,
  kX86SysRet,

  kX86SysEnter,
  kX86SysExit,
};

extern "C" {

extern const IndirectBlock __remill_indirect_blocks[];
extern const NamedBlock __remill_exported_blocks[];
extern const NamedBlock __remill_imported_blocks[];

// The basic block "template".
[[gnu::used]]
void __remill_basic_block(State &state, Memory &memory, addr_t);

// Address computation intrinsic. This is only used for non-zero
// `address_space`d memory accesses.
[[gnu::used, gnu::const]]
extern addr_t __remill_compute_address(addr_t address, addr_t segment);

// Memory read intrinsics.
[[gnu::used, gnu::const]]
extern uint8_t __remill_read_memory_8(Memory *, addr_t);

[[gnu::used, gnu::const]]
extern uint16_t __remill_read_memory_16(Memory *, addr_t);

[[gnu::used, gnu::const]]
extern uint32_t __remill_read_memory_32(Memory *, addr_t);

[[gnu::used, gnu::const]]
extern uint64_t __remill_read_memory_64(Memory *, addr_t);

// Memory write intrinsics.
[[gnu::used, gnu::const]]
extern Memory *__remill_write_memory_8(Memory *, addr_t, uint8_t);

[[gnu::used, gnu::const]]
extern Memory *__remill_write_memory_16(Memory *, addr_t, uint16_t);

[[gnu::used, gnu::const]]
extern Memory *__remill_write_memory_32(Memory *, addr_t, uint32_t);

[[gnu::used, gnu::const]]
extern Memory *__remill_write_memory_64(Memory *, addr_t, uint64_t);

[[gnu::used, gnu::const]]
extern float32_t __remill_read_memory_f32(Memory *, addr_t);

[[gnu::used, gnu::const]]
extern float64_t __remill_read_memory_f64(Memory *, addr_t);

[[gnu::used]]
extern float64_t __remill_read_memory_f80(Memory *, addr_t);

[[gnu::used, gnu::const]]
extern Memory *__remill_write_memory_f32(Memory *, addr_t, float32_t);

[[gnu::used, gnu::const]]
extern Memory *__remill_write_memory_f64(Memory *, addr_t, float64_t);

[[gnu::used]]
extern Memory *__remill_write_memory_f80(Memory *, addr_t, float64_t);

[[gnu::used, gnu::const]]
extern bool __remill_undefined_bool(void);

[[gnu::used, gnu::const]]
extern uint8_t __remill_undefined_8(void);

[[gnu::used, gnu::const]]
extern uint16_t __remill_undefined_16(void);

[[gnu::used, gnu::const]]
extern uint32_t __remill_undefined_32(void);

[[gnu::used, gnu::const]]
extern uint64_t __remill_undefined_64(void);

[[gnu::used, gnu::const]]
extern float32_t __remill_undefined_f32(void);

[[gnu::used, gnu::const]]
extern float64_t __remill_undefined_f64(void);

// Inlining control. The idea here is that sometimes we want to defer inlining
// until a later time, and we need to communicate what should eventually be
// inlined, even if it's not currently inlined.
[[gnu::used]]
extern void __remill_defer_inlining(void);

// Generic error.
[[gnu::used]]
extern void __remill_error(State &, Memory *, addr_t addr);

// Control-flow intrinsics.
[[gnu::used]]
extern void __remill_function_call(State &, Memory *, addr_t addr);

[[gnu::used]]
extern void __remill_function_return(State &, Memory *, addr_t addr);

[[gnu::used]]
extern void __remill_jump(State &, Memory *, addr_t addr);

[[gnu::used]]
extern void __remill_async_hyper_call(State &, Memory *, addr_t ret_addr);

[[gnu::used, gnu::const]]
extern Memory *__remill_sync_hyper_call(State &, Memory *, SyncHyperCall);

// Transition to "native", unmodelled code from Remill-lifted code.
[[gnu::used]]
extern void __remill_detach(State &, Memory *, addr_t);

// Transition from native code into Remill-lifted code.
//
// Note:  It is possible to transition between two independent Remill-lifted
//        modules via a `__remill_detach` and `__remill_attach`.
[[gnu::used]]
extern void __remill_attach(void);

//[[gnu::used]]
//extern bool __remill_conditional_branch(
//    bool condition, addr_t if_true, addr_t if_false);

// Memory barriers types, see: http://g.oswego.edu/dl/jmm/cookbook.html
[[gnu::used, gnu::const]]
extern Memory *__remill_barrier_load_load(Memory *);

[[gnu::used, gnu::const]]
extern Memory *__remill_barrier_load_store(Memory *);

[[gnu::used, gnu::const]]
extern Memory *__remill_barrier_store_load(Memory *);

[[gnu::used, gnu::const]]
extern Memory *__remill_barrier_store_store(Memory *);

// Atomic operations. The address/size are hints, but the granularity of the
// access can be bigger. These have implicit StoreLoad semantics.
[[gnu::used, gnu::const]]
extern Memory *__remill_atomic_begin(Memory *);

[[gnu::used, gnu::const]]
extern Memory *__remill_atomic_end(Memory *);

[[gnu::used]]
extern void __remill_intrinsics(void);

}  // extern C

#endif  // REMILL_ARCH_RUNTIME_INTRINSICS_H_
