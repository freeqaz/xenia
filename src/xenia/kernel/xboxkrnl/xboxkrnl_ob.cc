/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <unordered_map>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t ObOpenObjectByName_entry(lpunknown_t obj_attributes_ptr,
                                        lpunknown_t object_type_ptr,
                                        dword_t unk, lpdword_t handle_ptr) {
  // r3 = ptr to info?
  //   +0 = -4
  //   +4 = name ptr
  //   +8 = 0
  // r4 = ExEventObjectType | ExSemaphoreObjectType | ExTimerObjectType
  // r5 = 0
  // r6 = out_ptr (handle?)

  if (!obj_attributes_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }

  auto obj_attributes = kernel_memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
      obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_memory(),
                                               obj_attributes->name_ptr);

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name, &handle);
  if (XSUCCEEDED(result)) {
    *handle_ptr = handle;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByName, kNone, kImplemented);

dword_result_t ObOpenObjectByPointer_entry(lpvoid_t object_ptr,
                                           lpdword_t out_handle_ptr) {
  auto object = XObject::GetNativeObject<XObject>(kernel_state(), object_ptr);
  if (!object) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Retain the handle. Will be released in NtClose.
  object->RetainHandle();
  *out_handle_ptr = object->handle();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObOpenObjectByPointer, kNone, kImplemented);

dword_result_t ObLookupThreadByThreadId_entry(dword_t thread_id,
                                              lpdword_t out_object_ptr) {
  auto thread = kernel_state()->GetThreadByID(thread_id);
  if (!thread) {
    return X_STATUS_NOT_FOUND;
  }

  // Retain the object. Will be released in ObDereferenceObject.
  thread->RetainHandle();
  *out_object_ptr = thread->guest_object();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObLookupThreadByThreadId, kNone, kImplemented);

dword_result_t ObReferenceObjectByHandle_entry(dword_t handle,
                                               dword_t object_type_ptr,
                                               lpdword_t out_object_ptr) {
  // A title identifies the expected object type in one of two ways, and we
  // accept BOTH (fork-cleanup C14):
  //
  //  (a) the legacy Xenia sentinel D###BEEF, where ### is the ordinal of the
  //      Ex*ObjectType data export. That is what upstream compared against and
  //      what a title sees if it dereferences the (previously unmapped) export
  //      variable and gets Xenia's uninitialized-data-export placeholder.
  //  (b) the guest ADDRESS of the Ex*ObjectType export variable itself, which
  //      is what a title that passes `&ExEventObjectType` actually supplies,
  //      and what this fork switched to exclusively.
  //
  // Accepting only (b) silently broke every title that had been matching on
  // (a). Log (once per type per form) which one the title used so the legacy
  // path can eventually be retired with evidence.
  auto* resolver = kernel_state()->processor()->export_resolver();
  struct ObjectTypeInfo {
    const char* module;
    uint16_t ordinal;
    uint32_t legacy_sentinel;
  };
  const static std::unordered_map<XObject::Type, ObjectTypeInfo>
      object_type_ordinals = {
          // ExEventObjectType
          {XObject::Type::Event, {"xboxkrnl.exe", 0x0E, 0xD00EBEEF}},
          // ExSemaphoreObjectType
          {XObject::Type::Semaphore, {"xboxkrnl.exe", 0x17, 0xD017BEEF}},
          // ExThreadObjectType
          {XObject::Type::Thread, {"xboxkrnl.exe", 0x1B, 0xD01BBEEF}},
      };
  auto object = kernel_state()->object_table()->LookupObject<XObject>(handle);
  if (!object) {
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t native_ptr = object->guest_object();
  if (object_type_ptr) {
    // Check type: look up the expected variable address for this object type.
    auto ordinal_it = object_type_ordinals.find(object->type());
    if (ordinal_it != object_type_ordinals.end()) {
      auto export_entry = resolver->GetExportByOrdinal(
          ordinal_it->second.module, ordinal_it->second.ordinal);
      uint32_t expected_var_addr =
          export_entry ? export_entry->variable_ptr : 0;
      uint32_t legacy_sentinel = ordinal_it->second.legacy_sentinel;
      bool matched_var_addr =
          expected_var_addr && object_type_ptr == expected_var_addr;
      bool matched_sentinel = object_type_ptr == legacy_sentinel;
      if (!matched_var_addr && !matched_sentinel && expected_var_addr) {
        return X_STATUS_OBJECT_TYPE_MISMATCH;
      }
      // One line per (type, form) pair, not per call.
      static std::atomic<uint32_t> s_form_logged{0};
      uint32_t form_bit =
          1u << ((static_cast<uint32_t>(object->type()) & 0xF) * 2 +
                 (matched_sentinel ? 1u : 0u));
      if ((matched_var_addr || matched_sentinel) &&
          !(s_form_logged.fetch_or(form_bit, std::memory_order_relaxed) &
            form_bit)) {
        XELOGD(
            "ObReferenceObjectByHandle: object_type_ptr=0x{:08X} matched the "
            "{} form for type {} (var_addr=0x{:08X} sentinel=0x{:08X})",
            (uint32_t)object_type_ptr,
            matched_sentinel ? "legacy D###BEEF sentinel"
                             : "Ex*ObjectType variable address",
            static_cast<uint32_t>(object->type()), expected_var_addr,
            legacy_sentinel);
      }
    } else {
      // Unknown object type — don't fail, just warn
      assert_unhandled_case(object->type());
      native_ptr = 0xDEADF00D;
    }
  } else if (object_type_ordinals.find(object->type()) ==
             object_type_ordinals.end()) {
    assert_unhandled_case(object->type());
    native_ptr = 0xDEADF00D;
  }
  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  object->RetainHandle();
  if (out_object_ptr.guest_address()) {
    *out_object_ptr = native_ptr;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByHandle, kNone, kImplemented);

dword_result_t ObReferenceObjectByName_entry(lpstring_t name,
                                             dword_t attributes,
                                             dword_t object_type_ptr,
                                             lpvoid_t parse_context,
                                             lpdword_t out_object_ptr) {
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->GetObjectByName(name.value(), &handle);
  if (XSUCCEEDED(result)) {
    return ObReferenceObjectByHandle_entry(handle, object_type_ptr,
                                           out_object_ptr);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObjectByName, kNone, kImplemented);

dword_result_t ObDereferenceObject_entry(dword_t native_ptr) {
  // Check if a dummy value from ObReferenceObjectByHandle.
  if (native_ptr == 0xDEADF00D) {
    return 0;
  }

  auto object = XObject::GetNativeObject<XObject>(
      kernel_state(), kernel_memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->ReleaseHandle();
  }

  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(ObDereferenceObject, kNone, kImplemented);

dword_result_t ObCreateSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr,
                                          pointer_t<X_ANSI_STRING> target_ptr) {
  auto path = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiString(kernel_memory(), path_ptr));
  auto target = xe::utf8::canonicalize_guest_path(
      util::TranslateAnsiString(kernel_memory(), target_ptr));

  if (xe::utf8::starts_with(path, u8"\\??\\")) {
    path = path.substr(4);  // Strip the full qualifier
  }

  if (!kernel_state()->file_system()->RegisterSymbolicLink(path, target)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateSymbolicLink, kNone, kImplemented);

dword_result_t ObDeleteSymbolicLink_entry(pointer_t<X_ANSI_STRING> path_ptr) {
  auto path = util::TranslateAnsiString(kernel_memory(), path_ptr);
  if (!kernel_state()->file_system()->UnregisterSymbolicLink(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ObDeleteSymbolicLink, kNone, kImplemented);

dword_result_t NtDuplicateObject_entry(dword_t handle, lpdword_t new_handle_ptr,
                                       dword_t options) {
  // NOTE: new_handle_ptr can be zero to just close a handle.
  // NOTE: this function seems to be used to get the current thread handle
  //       (passed handle=-2).
  // This function actually just creates a new handle to the same object.
  // Most games use it to get real handles to the current thread or whatever.

  X_HANDLE new_handle = X_INVALID_HANDLE_VALUE;
  X_STATUS result =
      kernel_state()->object_table()->DuplicateHandle(handle, &new_handle);

  if (new_handle_ptr) {
    *new_handle_ptr = new_handle;
  }

  if (options == 1 /* DUPLICATE_CLOSE_SOURCE */) {
    // Always close the source object.
    kernel_state()->object_table()->RemoveHandle(handle);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtDuplicateObject, kNone, kImplemented);

dword_result_t NtClose_entry(dword_t handle) {
  return kernel_state()->object_table()->ReleaseHandle(handle);
}
DECLARE_XBOXKRNL_EXPORT1(NtClose, kNone, kImplemented);

// Object Manager stubs
dword_result_t ObCreateObject_entry(lpvoid_t object_type, dword_t attributes,
                                     dword_t object_size,
                                     lpdword_t object_ptr) {
  // TODO: real object creation
  XELOGW("ObCreateObject stub - returning error");
  return X_STATUS_UNSUCCESSFUL;
}
DECLARE_XBOXKRNL_EXPORT1(ObCreateObject, kNone, kStub);

void ObReferenceObject_entry(lpvoid_t object_ptr) {
  // TODO: real reference counting
}
DECLARE_XBOXKRNL_EXPORT1(ObReferenceObject, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ob);
