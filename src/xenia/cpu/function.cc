/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/function.h"

#include "xenia/base/logging.h"
#include "xenia/cpu/milo_trace.h"
#include "xenia/cpu/symbol.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

Function::Function(Module* module, uint32_t address)
    : Symbol(Symbol::Type::kFunction, module, address) {}

Function::~Function() = default;

BuiltinFunction::BuiltinFunction(Module* module, uint32_t address)
    : Function(module, address) {
  behavior_ = Behavior::kBuiltin;
}

BuiltinFunction::~BuiltinFunction() = default;

void BuiltinFunction::SetupBuiltin(Handler handler, void* arg0, void* arg1) {
  behavior_ = Behavior::kBuiltin;
  handler_ = handler;
  arg0_ = arg0;
  arg1_ = arg1;
}

bool BuiltinFunction::Call(ThreadState* thread_state, uint32_t return_address) {
  // SCOPE_profile_cpu_f("cpu");

  ThreadState* original_thread_state = ThreadState::Get();
  if (original_thread_state != thread_state) {
    ThreadState::Bind(thread_state);
  }

  assert_not_null(handler_);
  handler_(thread_state->context(), arg0_, arg1_);

  if (original_thread_state != thread_state) {
    ThreadState::Bind(original_thread_state);
  }

  return true;
}

GuestFunction::GuestFunction(Module* module, uint32_t address)
    : Function(module, address) {
  behavior_ = Behavior::kDefault;
}

GuestFunction::~GuestFunction() = default;

void GuestFunction::SetupExtern(ExternHandler handler, Export* export_data) {
  behavior_ = Behavior::kExtern;
  extern_handler_ = handler;
  export_data_ = export_data;
}

const SourceMapEntry* GuestFunction::LookupGuestAddress(
    uint32_t guest_address) const {
  // TODO(benvanik): binary search? We know the list is sorted by code order.
  for (size_t i = 0; i < source_map_.size(); ++i) {
    const auto& entry = source_map_[i];
    if (entry.guest_address == guest_address) {
      return &entry;
    }
  }
  return nullptr;
}

const SourceMapEntry* GuestFunction::LookupHIROffset(uint32_t offset) const {
  // TODO(benvanik): binary search? We know the list is sorted by code order.
  for (size_t i = 0; i < source_map_.size(); ++i) {
    const auto& entry = source_map_[i];
    if (entry.hir_offset >= offset) {
      return &entry;
    }
  }
  return nullptr;
}

const SourceMapEntry* GuestFunction::LookupMachineCodeOffset(
    uint32_t offset) const {
  // TODO(benvanik): binary search? We know the list is sorted by code order.
  for (int64_t i = source_map_.size() - 1; i >= 0; --i) {
    const auto& entry = source_map_[i];
    if (entry.code_offset <= offset) {
      return &entry;
    }
  }
  return source_map_.empty() ? nullptr : &source_map_[0];
}

uint32_t GuestFunction::MapGuestAddressToMachineCodeOffset(
    uint32_t guest_address) const {
  auto entry = LookupGuestAddress(guest_address);
  return entry ? entry->code_offset : 0;
}

uintptr_t GuestFunction::MapGuestAddressToMachineCode(
    uint32_t guest_address) const {
  auto entry = LookupGuestAddress(guest_address);
  return reinterpret_cast<uintptr_t>(machine_code()) +
         (entry ? entry->code_offset : 0);
}

uint32_t GuestFunction::MapMachineCodeToGuestAddress(
    uintptr_t host_address) const {
  auto entry = LookupMachineCodeOffset(static_cast<uint32_t>(
      host_address - reinterpret_cast<uintptr_t>(machine_code())));
  return entry ? entry->guest_address : address();
}

bool GuestFunction::Call(ThreadState* thread_state, uint32_t return_address) {
  // SCOPE_profile_cpu_f("cpu");

  ThreadState* original_thread_state = ThreadState::Get();
  if (original_thread_state != thread_state) {
    ThreadState::Bind(thread_state);
  }

  bool result = false;
  if (behavior_ == Behavior::kExtern && extern_handler_) {
    // kExtern override semantics are unchanged: the handler REPLACES the body.
    // milo-trace deliberately does NOT wrap kExtern calls — there is no real
    // guest body to capture entry/exit around, and wrapping would double-count
    // the DC3 override stubs that share this path.
    extern_handler_(thread_state->context(),
                    thread_state->context()->kernel_state);
    result = true;
  } else {
    // milo-trace X2 override-wrap PROTOTYPE hook: when a capture session is
    // active, snapshot entry state (full reg file + Tier-A stack window +
    // Tier-B pointer chase) before the real body runs and exit state (return
    // regs + memory write delta) after it. MiloTraceOnEntryCtx internally gates
    // on the traced-address set (MiloTraceShouldTrace), so a non-traced address
    // pushes nothing and MiloTraceOnExitCtx then pops nothing — the entry/exit
    // pair is self-balancing per (thread, func_va). MiloTraceIsActive() is a
    // single relaxed atomic load, so the common (no-capture) path is ~free and
    // emits no extra code beyond the branch. CallImpl may re-enter Call() for
    // nested guest calls; the per-thread shadow stack in milo_trace.cc handles
    // arbitrary nesting (depth_hint = pending.size()).
    const bool trace_wrap = MiloTraceIsActive();
    const uint32_t trace_addr = address_;
    if (trace_wrap) {
      MiloTraceOnEntryCtx(thread_state->context(), trace_addr);
    }
    result = CallImpl(thread_state, return_address);
    if (trace_wrap) {
      MiloTraceOnExitCtx(thread_state->context(), trace_addr);
    }
  }

  if (original_thread_state != thread_state) {
    ThreadState::Bind(original_thread_state);
  }

  return result;
}

}  // namespace cpu
}  // namespace xe
