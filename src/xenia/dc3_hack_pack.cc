#include "xenia/dc3_hack_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <set>
#include <string>

#include "config.h"

#if defined(__linux__)
#include <cerrno>
#include <sys/mman.h>
#endif

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"

DECLARE_int32(dc3_crt_bisect_max);
DECLARE_string(dc3_crt_skip_indices);
DECLARE_bool(dc3_crt_skip_nui);
DECLARE_bool(dc3_debug_read_cache_stream_step_override);
DECLARE_bool(dc3_debug_memmgr_assert_nop_bypass);
DECLARE_string(dc3_debug_findarray_override_mode);
DECLARE_bool(fake_kinect_data);

namespace xe {

namespace kernel {
namespace xboxkrnl {
void _vsnprintf_entry(cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state);
void _vsnwprintf_entry(cpu::ppc::PPCContext* ppc_context,
                       kernel::KernelState* kernel_state);
}  // namespace xboxkrnl
}  // namespace kernel

namespace {

constexpr uint32_t kPpcLiR3_0 = 0x38600000;
constexpr uint32_t kPpcBlr = 0x4E800020;
static uint32_t g_dc3_errno_guest_ptr = 0;
constexpr uint32_t kDc3RcsReadCacheStreamAddr = 0x83116664;
constexpr uint32_t kDc3RcsBufStreamReadImplAddr = 0x82BC3AC8;
constexpr uint32_t kDc3RcsBufStreamSeekImplAddr = 0x82BC3BC8;
constexpr uint32_t kDc3OutputLAddr = 0x8361CBE0;
constexpr uint32_t kDc3WOutputLAddr = 0x83622778;
constexpr uint32_t kDc3IoStrgFlag = 0x0040;
constexpr uint32_t kDc3TempNarrowBufSize = 4096;
constexpr uint32_t kDc3TempWideBufChars = 2048;
static uint32_t g_dc3_output_l_scratch_narrow = 0;
static uint32_t g_dc3_output_l_scratch_wide = 0;
static uint32_t g_dc3_safe_null_datanode = 0;
static uint32_t g_dc3_safe_lvalue_datanode = 0;
static uint32_t g_dc3_safe_empty_array_datanode = 0;
static uint32_t g_dc3_safe_empty_dataarray = 0;

std::string Dc3FmtBytePreview(const uint8_t* data, size_t len) {
  if (!data || !len) return "";
  std::ostringstream os;
  os << std::hex;
  for (size_t i = 0; i < len; ++i) {
    if (i) os << ' ';
    char tmp[4];
    std::snprintf(tmp, sizeof(tmp), "%02X", data[i]);
    os << tmp;
  }
  return os.str();
}

void Dc3RcsBufStreamReadImplProbe(cpu::ppc::PPCContext* ppc_context,
                                  kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) return;
  auto* memory = kernel_state->memory();
  if (!memory) return;

  uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
  uint32_t dst_ptr = static_cast<uint32_t>(ppc_context->r[4]);
  int32_t req_bytes = static_cast<int32_t>(ppc_context->r[5]);
  uint32_t lr = static_cast<uint32_t>(ppc_context->lr);

  auto* bs = memory->TranslateVirtual<uint8_t*>(this_ptr);
  if (!bs) {
    static uint32_t bad_count = 0;
    if (bad_count++ < 16) {
      XELOGW("DC3:RCS ReadImpl translate fail this={:08X} dst={:08X} bytes={} "
             "LR={:08X}",
             this_ptr, dst_ptr, req_bytes, lr);
    }
    return;
  }

  uint32_t src_buf_ptr = xe::load_and_swap<uint32_t>(bs + 0x10);
  bool fail = bs[0x14] != 0;
  int32_t tell_before = xe::load_and_swap<int32_t>(bs + 0x18);
  int32_t size = xe::load_and_swap<int32_t>(bs + 0x1C);
  uint32_t checksum_ptr = xe::load_and_swap<uint32_t>(bs + 0x20);
  int32_t bytes_checksummed = xe::load_and_swap<int32_t>(bs + 0x24);

  int32_t bytes = req_bytes;
  bool truncated = false;
  if (tell_before + bytes > size) {
    fail = true;
    truncated = true;
    bytes = size - tell_before;
  }
  if (bytes < 0) {
    bytes = 0;
    fail = true;
    truncated = true;
  }

  auto* src = (src_buf_ptr && src_buf_ptr < 0xF0000000)
                  ? memory->TranslateVirtual<uint8_t*>(src_buf_ptr + tell_before)
                  : nullptr;
  auto* dst = (dst_ptr && dst_ptr < 0xF0000000)
                  ? memory->TranslateVirtual<uint8_t*>(dst_ptr)
                  : nullptr;

  uint8_t preview[16] = {};
  size_t preview_len =
      static_cast<size_t>(bytes > 0 ? std::min<int32_t>(bytes, 16) : 0);
  if (src && preview_len) {
    std::memcpy(preview, src, preview_len);
  }

  if (src && dst && bytes > 0) {
    std::memcpy(dst, src, static_cast<size_t>(bytes));
  } else if (dst && bytes > 0) {
    std::memset(dst, 0, static_cast<size_t>(bytes));
    fail = true;
  }

  int32_t tell_after = tell_before + bytes;

  if (checksum_ptr && !fail) {
    bytes_checksummed += bytes;
    static bool warned_checksum = false;
    if (!warned_checksum) {
      warned_checksum = true;
      XELOGW("DC3:RCS ReadImpl probe active with checksum object present; "
             "guest checksum validator Update() is not invoked from host "
             "override. Dedicated probe runs may fail checksum validation.");
    }
  }

  bs[0x14] = fail ? 1 : 0;
  xe::store_and_swap<int32_t>(bs + 0x18, tell_after);
  xe::store_and_swap<int32_t>(bs + 0x24, bytes_checksummed);

  static uint32_t read_count = 0;
  ++read_count;
  if (read_count <= 256 || (read_count % 512) == 0 || truncated ||
      lr == kDc3RcsReadCacheStreamAddr) {
    std::string preview_suffix;
    if (preview_len) {
      preview_suffix = " bytes=[" + Dc3FmtBytePreview(preview, preview_len) + "]";
    }
    XELOGI(
        "DC3:RCS ReadImpl #{} this={:08X} LR={:08X} dst={:08X} req={} got={} "
        "tell {}->{} size={} fail={} chk={:08X} chk_bytes={}{}{}",
        read_count, this_ptr, lr, dst_ptr, req_bytes, bytes, tell_before,
        tell_after, size, fail ? 1 : 0, checksum_ptr, bytes_checksummed,
        truncated ? " truncated" : "", src ? "" : " src_xlate_fail", preview_suffix);
  }
}

void Dc3RcsBufStreamSeekImplProbe(cpu::ppc::PPCContext* ppc_context,
                                  kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) return;
  auto* memory = kernel_state->memory();
  if (!memory) return;

  uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
  int32_t offset = static_cast<int32_t>(ppc_context->r[4]);
  int32_t seek_type = static_cast<int32_t>(ppc_context->r[5]);
  uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
  auto* bs = memory->TranslateVirtual<uint8_t*>(this_ptr);
  if (!bs) return;

  bool fail = bs[0x14] != 0;
  int32_t tell_before = xe::load_and_swap<int32_t>(bs + 0x18);
  int32_t size = xe::load_and_swap<int32_t>(bs + 0x1C);
  int32_t pos = tell_before;
  switch (seek_type) {
    case 0:
      pos = offset;
      break;
    case 1:
      pos = tell_before + offset;
      break;
    case 2:
      pos = size + offset;
      break;
    default:
      return;
  }

  int32_t tell_after = tell_before;
  if (pos < 0 || pos > size) {
    fail = true;
  } else {
    tell_after = pos;
    xe::store_and_swap<int32_t>(bs + 0x18, tell_after);
  }
  bs[0x14] = fail ? 1 : 0;

  static uint32_t seek_count = 0;
  ++seek_count;
  if (seek_count <= 128 || (seek_count % 256) == 0 || fail) {
    XELOGI(
        "DC3:RCS SeekImpl #{} this={:08X} LR={:08X} type={} off={} tell {}->{} "
        "size={} fail={}",
        seek_count, this_ptr, lr, seek_type, offset, tell_before, tell_after,
        size, fail ? 1 : 0);
  }
}

uint32_t LookupStubAddr(
    const std::unordered_map<std::string, uint32_t>* manifest,
    const char* name, uint32_t fallback) {
  if (manifest) {
    auto it = manifest->find(name);
    if (it != manifest->end()) {
      return it->second;
    }
  }
  return fallback;
}

void Dc3ErrnoExtern(cpu::ppc::PPCContext* ppc_context,
                    kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (!ppc_context) return;
  ppc_context->r[3] = g_dc3_errno_guest_ptr;
}

Dc3HackApplyResult& GetResult(Dc3HackPackSummary& summary, Dc3HackCategory category) {
  for (auto& result : summary.results) {
    if (result.category == category) {
      return result;
    }
  }
  summary.results.emplace_back();
  summary.results.back().category = category;
  return summary.results.back();
}

bool PatchStub8(Memory* memory, uint32_t address, uint32_t return_value,
                const char* name) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  auto* mem = memory->TranslateVirtual<uint8_t*>(address);
  if (!mem) return false;
  uint32_t w0 = xe::load_and_swap<uint32_t>(mem);
  if (w0 == 0x00000000) return false;
  heap->Protect(address, 8, kMemoryProtectRead | kMemoryProtectWrite);
  uint32_t li_instr = 0x38600000 | (return_value & 0xFFFF);
  xe::store_and_swap<uint32_t>(mem + 0, li_instr);
  xe::store_and_swap<uint32_t>(mem + 4, 0x4E800020);
  XELOGI("DC3: Stubbed {} at {:08X} (li r3,{}; blr)", name, address, return_value);
  return true;
}

bool PatchStub8Resolved(Memory* memory,
                        const std::unordered_map<std::string, uint32_t>* manifest,
                        uint32_t fallback_address, uint32_t return_value,
                        const char* name) {
  uint32_t address = LookupStubAddr(manifest, name, fallback_address);
  if (address != fallback_address) {
    XELOGI("DC3: Resolved hack-pack stub '{}' {:08X} -> {:08X} via manifest",
           name, fallback_address, address);
    if (!PatchStub8(memory, address, return_value, name)) {
      XELOGW("DC3: Manifest-resolved stub '{}' at {:08X} could not be patched; "
             "retrying fallback {:08X}",
             name, address, fallback_address);
      return PatchStub8(memory, fallback_address, return_value, name);
    }
    return true;
  }
  return PatchStub8(memory, fallback_address, return_value, name);
}

bool PatchCheckedNop(Memory* memory, uint32_t address, uint32_t expected_word,
                     const char* name) {
  constexpr uint32_t kPpcNop = 0x60000000;
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    XELOGW("DC3: {} bypass skipped at {:08X} (no heap)", name, address);
    return false;
  }
  auto* mem = memory->TranslateVirtual<uint8_t*>(address);
  if (!mem) {
    XELOGW("DC3: {} bypass skipped at {:08X} (translate failed)", name, address);
    return false;
  }
  uint32_t current = xe::load_and_swap<uint32_t>(mem);
  if (current != expected_word && current != kPpcNop) {
    XELOGW(
        "DC3: {} bypass skipped at {:08X} (expected {:08X}, found {:08X}; "
        "layout drift or code changed)",
        name, address, expected_word, current);
    return false;
  }
  heap->Protect(address, 4, kMemoryProtectRead | kMemoryProtectWrite);
  if (current != kPpcNop) {
    xe::store_and_swap<uint32_t>(mem, kPpcNop);
  }
  XELOGI("DC3: Debug-only MemMgr assert bypass active at {:08X} ({})", address,
         name);
  return true;
}

bool PatchCheckedWord(Memory* memory, uint32_t address, uint32_t expected_word,
                      uint32_t replacement_word, const char* name,
                      const char* success_label) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    XELOGW("DC3: {} skipped at {:08X} (no heap)", name, address);
    return false;
  }
  auto* mem = memory->TranslateVirtual<uint8_t*>(address);
  if (!mem) {
    XELOGW("DC3: {} skipped at {:08X} (translate failed)", name, address);
    return false;
  }
  uint32_t current = xe::load_and_swap<uint32_t>(mem);
  if (current != expected_word && current != replacement_word) {
    XELOGW(
        "DC3: {} skipped at {:08X} (expected {:08X}, found {:08X}; layout "
        "drift or code changed)",
        name, address, expected_word, current);
    return false;
  }
  heap->Protect(address, 4, kMemoryProtectRead | kMemoryProtectWrite);
  if (current != replacement_word) {
    xe::store_and_swap<uint32_t>(mem, replacement_word);
  }
  XELOGI("DC3: {} at {:08X}", success_label ? success_label : name, address);
  return true;
}

uint32_t Dc3EnsureScratchBuffer(kernel::KernelState* kernel_state,
                                uint32_t* guest_ptr, uint32_t size,
                                const char* label) {
  if (!kernel_state || !guest_ptr) return 0;
  if (*guest_ptr) return *guest_ptr;
  auto* memory = kernel_state->memory();
  if (!memory) return 0;
  *guest_ptr = memory->SystemHeapAlloc(size, 0x10);
  if (*guest_ptr) {
    XELOGI("DC3: Allocated {} scratch buffer at {:08X} ({} bytes)", label,
           *guest_ptr, size);
  } else {
    XELOGW("DC3: Failed to allocate {} scratch buffer ({} bytes)", label, size);
  }
  return *guest_ptr;
}

void Dc3OutputLBridgeExtern(cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) return;
  auto* memory = kernel_state->memory();
  if (!memory) return;

  uint32_t file_ptr = static_cast<uint32_t>(ppc_context->r[3]);
  uint32_t format_ptr = static_cast<uint32_t>(ppc_context->r[4]);
  uint32_t locale_ptr = static_cast<uint32_t>(ppc_context->r[5]);
  uint32_t arg_ptr = static_cast<uint32_t>(ppc_context->r[6]);

  auto* file = (file_ptr && file_ptr < 0xF0000000)
                   ? memory->TranslateVirtual<uint8_t*>(file_ptr)
                   : nullptr;
  auto* fmt = (format_ptr && format_ptr < 0xF0000000)
                  ? memory->TranslateVirtual<const char*>(format_ptr)
                  : nullptr;
  if (!file || !fmt) {
    ppc_context->r[3] = static_cast<uint64_t>(-1);
    return;
  }

  uint32_t flags = xe::load_and_swap<uint32_t>(file + 0x0C);
  bool is_string_stream = (flags & kDc3IoStrgFlag) != 0;

  if (is_string_stream) {
    uint32_t dst_ptr = xe::load_and_swap<uint32_t>(file + 0x00);
    int32_t dst_cnt = xe::load_and_swap<int32_t>(file + 0x04);
    if (!dst_ptr || dst_cnt <= 0) {
      ppc_context->r[3] = static_cast<uint64_t>(-1);
      return;
    }

    cpu::ppc::PPCContext tmp = *ppc_context;
    tmp.r[3] = dst_ptr;
    tmp.r[4] = static_cast<uint32_t>(dst_cnt);
    tmp.r[5] = format_ptr;
    tmp.r[6] = arg_ptr;
    kernel::xboxkrnl::_vsnprintf_entry(&tmp, kernel_state);
    int32_t count = static_cast<int32_t>(tmp.r[3]);

    int32_t advance = 0;
    if (count > 0) {
      advance = std::min<int32_t>(count, dst_cnt);
    }
    xe::store_and_swap<uint32_t>(file + 0x00, dst_ptr + advance);
    xe::store_and_swap<int32_t>(file + 0x04,
                                std::max<int32_t>(0, dst_cnt - advance));
    ppc_context->r[3] = static_cast<uint64_t>(count);

    static uint32_t str_count = 0;
    ++str_count;
    if (str_count <= 32 || (str_count % 500) == 0) {
      XELOGI(
          "DC3: _output_l string FILE={:08X} flags={:08X} cnt={} fmt='{}' "
          "arg={:08X} -> {}",
          file_ptr, flags, dst_cnt, fmt, arg_ptr, count);
    }
    return;
  }

  uint32_t scratch =
      Dc3EnsureScratchBuffer(kernel_state, &g_dc3_output_l_scratch_narrow,
                             kDc3TempNarrowBufSize, "_output_l");
  if (!scratch) {
    ppc_context->r[3] = 0;
    return;
  }
  cpu::ppc::PPCContext tmp = *ppc_context;
  tmp.r[3] = scratch;
  tmp.r[4] = kDc3TempNarrowBufSize - 1;
  tmp.r[5] = format_ptr;
  tmp.r[6] = arg_ptr;
  kernel::xboxkrnl::_vsnprintf_entry(&tmp, kernel_state);
  int32_t count = static_cast<int32_t>(tmp.r[3]);

  auto* buf = memory->TranslateVirtual<char*>(scratch);
  if (buf) {
    int32_t emit = count;
    if (emit < 0) {
      emit = static_cast<int32_t>(::strnlen(buf, kDc3TempNarrowBufSize - 1));
    } else if (emit > static_cast<int32_t>(kDc3TempNarrowBufSize - 1)) {
      emit = kDc3TempNarrowBufSize - 1;
    }
    if (emit > 0) {
      std::fwrite(buf, 1, static_cast<size_t>(emit), stderr);
      std::fflush(stderr);
    }
  }
  ppc_context->r[3] = static_cast<uint64_t>(count < 0 ? 0 : count);

  static uint32_t console_count = 0;
  ++console_count;
  if (console_count <= 32 || (console_count % 500) == 0) {
    XELOGI(
        "DC3: _output_l console FILE={:08X} flags={:08X} locale={:08X} "
        "fmt={:08X} arg={:08X} -> {}",
        file_ptr, flags, locale_ptr, format_ptr, arg_ptr, count);
  }
}

void Dc3WOutputLBridgeExtern(cpu::ppc::PPCContext* ppc_context,
                             kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) return;
  auto* memory = kernel_state->memory();
  if (!memory) return;

  uint32_t file_ptr = static_cast<uint32_t>(ppc_context->r[3]);
  uint32_t format_ptr = static_cast<uint32_t>(ppc_context->r[4]);
  uint32_t arg_ptr = static_cast<uint32_t>(ppc_context->r[6]);
  auto* file = (file_ptr && file_ptr < 0xF0000000)
                   ? memory->TranslateVirtual<uint8_t*>(file_ptr)
                   : nullptr;
  if (!file || !format_ptr) {
    ppc_context->r[3] = 0;
    return;
  }
  uint32_t flags = xe::load_and_swap<uint32_t>(file + 0x0C);
  bool is_string_stream = (flags & kDc3IoStrgFlag) != 0;

  if (is_string_stream) {
    // Defer wide string-file emulation for now; returning 0 avoids CRT spin
    // and keeps debug runs moving. Narrow MakeString paths are handled by _output_l.
    ppc_context->r[3] = 0;
    static bool warned = false;
    if (!warned) {
      warned = true;
      XELOGW(
          "DC3: _woutput_l string-stream path not fully emulated yet; returning 0");
    }
    return;
  }

  uint32_t scratch = Dc3EnsureScratchBuffer(kernel_state, &g_dc3_output_l_scratch_wide,
                                            kDc3TempWideBufChars * 2, "_woutput_l");
  if (!scratch) {
    ppc_context->r[3] = 0;
    return;
  }
  cpu::ppc::PPCContext tmp = *ppc_context;
  tmp.r[3] = scratch;
  tmp.r[4] = kDc3TempWideBufChars - 1;
  tmp.r[5] = format_ptr;
  tmp.r[6] = arg_ptr;
  kernel::xboxkrnl::_vsnwprintf_entry(&tmp, kernel_state);
  int32_t count = static_cast<int32_t>(tmp.r[3]);
  auto* wbuf = memory->TranslateVirtual<xe::be<uint16_t>*>(scratch);
  if (wbuf) {
    int32_t emit = count;
    if (emit < 0) emit = 0;
    emit = std::min<int32_t>(emit, kDc3TempWideBufChars - 1);
    std::u16string u16;
    u16.reserve(static_cast<size_t>(emit));
    for (int32_t i = 0; i < emit; ++i) {
      u16.push_back(static_cast<char16_t>(uint16_t(wbuf[i])));
    }
    auto utf8 = xe::to_utf8(u16);
    if (!utf8.empty()) {
      std::fwrite(utf8.data(), 1, utf8.size(), stderr);
      std::fflush(stderr);
    }
  }
  ppc_context->r[3] = static_cast<uint64_t>(count < 0 ? 0 : count);
}

void RegisterDc3WriteBridges(const Dc3HackContext& ctx, Dc3HackApplyResult& debug_result) {
  if (!ctx.processor) {
    debug_result.skipped++;
    return;
  }
  struct WriteBridge {
    uint32_t addr;
    const char* name;
  };
  const WriteBridge bridges[] = {
      {0x83617E1C, "_write_nolock"},
      {0x8361805C, "_write"},
  };
  for (const auto& bridge : bridges) {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      int fd = static_cast<int>(ppc_context->r[3]);
      uint32_t buf_addr = static_cast<uint32_t>(ppc_context->r[4]);
      uint32_t count = static_cast<uint32_t>(ppc_context->r[5]);
      if (!memory || !buf_addr || buf_addr >= 0xF0000000 || count > (1u << 20)) {
        ppc_context->r[3] = static_cast<uint64_t>(-1);
        return;
      }
      auto* src = memory->TranslateVirtual<const uint8_t*>(buf_addr);
      if (!src) {
        ppc_context->r[3] = static_cast<uint64_t>(-1);
        return;
      }
      FILE* out = (fd == 1) ? stdout : stderr;
      size_t written = std::fwrite(src, 1, count, out);
      std::fflush(out);
      ppc_context->r[3] = static_cast<uint64_t>(written);
    };
    ctx.processor->RegisterGuestFunctionOverride(bridge.addr, handler, bridge.name);
    XELOGI("DC3: Registered {} host bridge at {:08X}", bridge.name, bridge.addr);
    debug_result.applied++;
  }
}

void RegisterDc3LocaleBootstrapBridges(const Dc3HackContext& ctx,
                                       Dc3HackApplyResult& debug_result) {
  if (!ctx.processor || !ctx.is_decomp_layout) {
    debug_result.skipped++;
    return;
  }
  struct Bridge {
    uint32_t addr;
    const char* name;
  };
  const Bridge bridges[] = {
      {0x83409AA8, "GetSystemLanguage"},
      {0x83409F68, "GetSystemLocale"},
  };
  auto handler = [](cpu::ppc::PPCContext* ppc_context,
                    kernel::KernelState* kernel_state) {
    (void)kernel_state;
    if (!ppc_context) return;
    static uint32_t call_count = 0;
    ++call_count;
    if (call_count <= 32 || (call_count % 256) == 0) {
      uint32_t sym_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      const char* sym_s = nullptr;
      if (kernel_state && kernel_state->memory() && sym_ptr && sym_ptr < 0xF0000000) {
        sym_s = kernel_state->memory()->TranslateVirtual<const char*>(sym_ptr);
      }
      if (sym_s) {
        XELOGI("DC3: Locale bootstrap bridge (identity) LR={:08X} sym='{}' ({:08X})",
               lr, sym_s, sym_ptr);
      } else {
        XELOGI("DC3: Locale bootstrap bridge (identity) LR={:08X} sym={:08X}", lr,
               sym_ptr);
      }
    }
    // Symbol is a 32-bit pointer wrapper in this build; return the input
    // symbol unchanged to preserve caller defaults (e.g. eng/usa) and avoid
    // recursive config/locale lookups during early boot.
    ppc_context->r[3] = static_cast<uint32_t>(ppc_context->r[3]);
  };
  for (const auto& bridge : bridges) {
    ctx.processor->RegisterGuestFunctionOverride(bridge.addr, handler,
                                                 bridge.name);
    XELOGI("DC3: Registered {} bootstrap identity bridge at {:08X}", bridge.name,
           bridge.addr);
    debug_result.applied++;
  }
}

void RegisterDc3OutputFormatterBridges(const Dc3HackContext& ctx,
                                       Dc3HackApplyResult& debug_result) {
  if (!ctx.processor) {
    debug_result.skipped++;
    return;
  }

  // Do not consume generic hack-pack stub manifest remaps for CRT formatters.
  // The manifest may contain duplicate-name entries that resolve to unrelated
  // implementations, causing the bridge to register on the wrong function and
  // silently miss the live _output_l/_woutput_l path.
  if (ctx.hack_pack_stubs) {
    auto it = ctx.hack_pack_stubs->find("_output_l");
    if (it != ctx.hack_pack_stubs->end() && it->second != kDc3OutputLAddr) {
      XELOGW("DC3: Ignoring manifest _output_l remap {:08X} -> {:08X}; using "
             "map-synced CRT address",
             kDc3OutputLAddr, it->second);
    }
    it = ctx.hack_pack_stubs->find("_woutput_l");
    if (it != ctx.hack_pack_stubs->end() && it->second != kDc3WOutputLAddr) {
      XELOGW("DC3: Ignoring manifest _woutput_l remap {:08X} -> {:08X}; using "
             "map-synced CRT address",
             kDc3WOutputLAddr, it->second);
    }
  }
  const uint32_t output_l_addr = kDc3OutputLAddr;
  const uint32_t woutput_l_addr = kDc3WOutputLAddr;

  ctx.processor->RegisterGuestFunctionOverride(output_l_addr, Dc3OutputLBridgeExtern,
                                               "DC3:_output_l(bridge)");
  XELOGI("DC3: Registered _output_l bridge at {:08X}", output_l_addr);
  debug_result.applied++;

  ctx.processor->RegisterGuestFunctionOverride(woutput_l_addr, Dc3WOutputLBridgeExtern,
                                               "DC3:_woutput_l(bridge)");
  XELOGI("DC3: Registered _woutput_l bridge at {:08X}", woutput_l_addr);
  debug_result.applied++;
}

void RegisterDc3FindArrayOverride(const Dc3HackContext& ctx,
                                  Dc3HackApplyResult& debug_result) {
  if (!ctx.is_decomp_layout || !ctx.processor) {
    debug_result.skipped++;
    return;
  }
  const std::string mode = cvars::dc3_debug_findarray_override_mode;
  if (mode.empty() || mode == "off") {
    XELOGI("DC3: FindArray override disabled (dc3_debug_findarray_override_mode=off)");
    debug_result.skipped++;
    return;
  }
  if (mode != "log_only" && mode != "stub_on_fail" && mode != "null_on_fail") {
    XELOGW("DC3: Unknown FindArray override mode '{}'; expected off|log_only|"
           "stub_on_fail|null_on_fail. Leaving original behavior active.",
           mode);
    debug_result.skipped++;
    return;
  }

  static uint32_t s_find_array_stub = 0;
  constexpr uint32_t kFindArrayAddr = 0x83543424;
  auto find_array_handler = [](cpu::ppc::PPCContext* ppc_context,
                               kernel::KernelState* kernel_state) {
    if (!ppc_context || !kernel_state) return;
    auto* memory = kernel_state->memory();
    if (!memory) return;

    const std::string mode_local = cvars::dc3_debug_findarray_override_mode;
    const bool probe_only = (mode_local == "log_only");
    const bool use_stub_on_fail = (mode_local == "stub_on_fail");
    const bool force_null_on_fail = (mode_local == "null_on_fail");
    if (!probe_only && !use_stub_on_fail && !force_null_on_fail) {
      return;
    }

    if (use_stub_on_fail && !s_find_array_stub) {
      s_find_array_stub = memory->SystemHeapAlloc(0x20, 0x10);
      if (s_find_array_stub) {
        if (auto* stub = memory->TranslateVirtual<uint8_t*>(s_find_array_stub)) {
          std::memset(stub, 0, 0x20);
          xe::store_and_swap<uint32_t>(stub + 0x0, 0);   // nodes
          xe::store_and_swap<int16_t>(stub + 0x8, 0);    // size
          xe::store_and_swap<int16_t>(stub + 0xA, 0);    // line
          xe::store_and_swap<int16_t>(stub + 0xC, 0);    // deprecated
          xe::store_and_swap<int16_t>(stub + 0xE, 1);    // refs
          XELOGI("DC3: Allocated FindArray stub DataArray at {:08X} (size=0)",
                 s_find_array_stub);
        }
      }
    }

    uint32_t da_addr = static_cast<uint32_t>(ppc_context->r[3]);
    uint32_t sym_addr = static_cast<uint32_t>(ppc_context->r[4]);
    uint32_t fail_flag = static_cast<uint32_t>(ppc_context->r[5]);

    char sym_str[64] = {};
    std::string sym_repr = "<null>";
    if (sym_addr && sym_addr < 0xF0000000) {
      if (auto* s = memory->TranslateVirtual<const char*>(sym_addr)) {
        std::strncpy(sym_str, s, sizeof(sym_str) - 1);
        bool printable = true;
        for (size_t i = 0; i < sizeof(sym_str) && sym_str[i]; ++i) {
          const unsigned char c = static_cast<unsigned char>(sym_str[i]);
          if (c < 0x20 || c > 0x7E) {
            printable = false;
            break;
          }
        }
        if (printable && sym_str[0]) {
          sym_repr = sym_str;
        } else {
          sym_repr = std::string("<bin:") +
                     Dc3FmtBytePreview(reinterpret_cast<const uint8_t*>(s), 16) +
                     ">";
        }
      } else {
        sym_repr = "<unmapped>";
      }
    }

    uint32_t found_addr = 0;
    if (da_addr && da_addr < 0xF0000000) {
      if (auto* da = memory->TranslateVirtual<uint8_t*>(da_addr)) {
        uint32_t nodes_ptr = xe::load_and_swap<uint32_t>(da + 0x0);
        int16_t da_size = xe::load_and_swap<int16_t>(da + 0x8);
        if (nodes_ptr && nodes_ptr < 0xF0000000 && da_size > 0 && da_size < 0x2000) {
          if (auto* nodes = memory->TranslateVirtual<uint8_t*>(nodes_ptr)) {
            for (int i = 0; i < da_size; ++i) {
              uint8_t* n = nodes + i * 8;
              uint32_t val = xe::load_and_swap<uint32_t>(n + 0);
              uint32_t type = xe::load_and_swap<uint32_t>(n + 4);
              if (type != 16 || !val || val >= 0xF0000000) continue;
              auto* sub = memory->TranslateVirtual<uint8_t*>(val);
              if (!sub) continue;
              uint32_t sub_nodes = xe::load_and_swap<uint32_t>(sub + 0x0);
              int16_t sub_size = xe::load_and_swap<int16_t>(sub + 0x8);
              if (!sub_nodes || sub_nodes >= 0xF0000000 || sub_size <= 0 ||
                  sub_size >= 0x2000) {
                continue;
              }
              auto* sn = memory->TranslateVirtual<uint8_t*>(sub_nodes);
              if (!sn) continue;
              uint32_t sn_val = xe::load_and_swap<uint32_t>(sn + 0);
              uint32_t sn_type = xe::load_and_swap<uint32_t>(sn + 4);
              if (sn_type != 5 || !sn_val || sn_val >= 0xF0000000) continue;
              auto* sn_str = memory->TranslateVirtual<const char*>(sn_val);
              if (sn_str && std::strcmp(sn_str, sym_str) == 0) {
                found_addr = val;
                break;
              }
            }
          }
        }
      }
    }

    uint32_t result = 0;
    const char* result_kind = "null";
    if (found_addr) {
      result = found_addr;
      result_kind = "real";
    } else if (use_stub_on_fail && fail_flag && s_find_array_stub) {
      result = s_find_array_stub;
      result_kind = "stub";
    } else if (probe_only || force_null_on_fail) {
      result = 0;
      result_kind = "null";
    } else {
      result = (fail_flag ? s_find_array_stub : 0);
      result_kind = (result ? "stub" : "null");
    }
    ppc_context->r[3] = result;

    static uint32_t log_count = 0;
    ++log_count;
    bool interesting_miss = (fail_flag != 0 && found_addr == 0);
    bool interesting_bin = sym_repr.rfind("<bin:", 0) == 0;
    if (interesting_miss || interesting_bin || log_count <= 200 ||
        (log_count % 1000) == 0) {
      XELOGI(
          "DC3: FindArray mode={} LR={:08X} da={:08X} sym={} fail={} found={:08X} -> {} "
          "{:08X}",
          mode_local, static_cast<uint32_t>(ppc_context->lr), da_addr, sym_repr,
          fail_flag,
          found_addr, result_kind, result);
    }
  };

  ctx.processor->RegisterGuestFunctionOverride(kFindArrayAddr, find_array_handler,
                                               "DC3:FindArray");
  XELOGI("DC3: Registered FindArray(Symbol,bool) override at {:08X} (mode={})",
         kFindArrayAddr, mode);
  debug_result.applied++;
}

void RegisterDc3ReadCacheStreamProbe(const Dc3HackContext& ctx,
                                     Dc3HackApplyResult& debug_result) {
  if (!ctx.processor) {
    debug_result.skipped++;
    return;
  }

  ctx.processor->RegisterGuestFunctionOverride(kDc3RcsBufStreamReadImplAddr,
                                               Dc3RcsBufStreamReadImplProbe,
                                               "DC3:RCS:BufStream::ReadImpl");
  ctx.processor->RegisterGuestFunctionOverride(kDc3RcsBufStreamSeekImplAddr,
                                               Dc3RcsBufStreamSeekImplProbe,
                                               "DC3:RCS:BufStream::SeekImpl");
  XELOGW("DC3: ReadCacheStream invasive probe active via BufStream overrides "
         "(ReadImpl={:08X}, SeekImpl={:08X}, ReadCacheStream={:08X}); may perturb "
         "checksum/parser behavior in dedicated probe runs",
         kDc3RcsBufStreamReadImplAddr, kDc3RcsBufStreamSeekImplAddr,
         kDc3RcsReadCacheStreamAddr);
  debug_result.applied += 2;
}

void RegisterDc3DataArraySafetyOverrides(const Dc3HackContext& ctx,
                                         Dc3HackApplyResult& debug_result) {
  if (!ctx.processor || !ctx.is_decomp_layout) {
    debug_result.skipped++;
    return;
  }

  constexpr uint32_t kMergedDataArrayNodeAddr = 0x835421A4;
  auto merged_dataarray_node_handler = [](cpu::ppc::PPCContext* ppc_context,
                                          kernel::KernelState* kernel_state) {
    if (!ppc_context || !kernel_state) return;
    auto* memory = kernel_state->memory();
    if (!memory) return;

    uint32_t da_addr = static_cast<uint32_t>(ppc_context->r[3]);
    int32_t index = static_cast<int32_t>(ppc_context->r[4]);
    uint32_t lr = static_cast<uint32_t>(ppc_context->lr);

    uint32_t result = 0;
    uint32_t nodes_ptr = 0;
    int16_t size = 0;
    bool oob = true;

    if (da_addr && da_addr < 0xF0000000) {
      if (auto* da = memory->TranslateVirtual<uint8_t*>(da_addr)) {
        nodes_ptr = xe::load_and_swap<uint32_t>(da + 0x0);
        size = xe::load_and_swap<int16_t>(da + 0x8);
        int size_i = static_cast<int>(size);
        if (nodes_ptr && nodes_ptr < 0xF0000000 && size_i >= 0 &&
            size_i < 0x4000 && index >= 0 && index < size_i) {
          result = nodes_ptr + static_cast<uint32_t>(index) * 8;
          oob = false;
        }
      }
    }

    if (oob) {
      // Targeted handling for Rnd::SetupFont() when SystemConfig("rnd","font")
      // is missing and the function blindly indexes a null DataArray.
      constexpr uint32_t kSetupFontNodeSourceLR = 0x8317FF40;
      constexpr uint32_t kSetupFontNodeDestLR = 0x8318001C;
      bool use_setupfont_empty_array_node = (lr == kSetupFontNodeSourceLR);
      bool use_setupfont_lvalue_node = (lr == kSetupFontNodeDestLR);

      uint32_t safe = 0;
      if (use_setupfont_empty_array_node) {
        uint32_t empty_arr =
            Dc3EnsureScratchBuffer(kernel_state, &g_dc3_safe_empty_dataarray, 0x20,
                                   "safe_empty_dataarray");
        uint32_t empty_node = Dc3EnsureScratchBuffer(
            kernel_state, &g_dc3_safe_empty_array_datanode, 8,
            "safe_empty_array_datanode");
        if (empty_arr) {
          if (auto* da = memory->TranslateVirtual<uint8_t*>(empty_arr)) {
            std::memset(da, 0, 0x20);
            xe::store_and_swap<uint32_t>(da + 0x0, 0);    // nodes
            xe::store_and_swap<int16_t>(da + 0x8, 0);     // size
            xe::store_and_swap<int16_t>(da + 0xA, 0);     // line
            xe::store_and_swap<int16_t>(da + 0xC, 0);     // deprecated
            xe::store_and_swap<int16_t>(da + 0xE, 0x7FFF); // refs (sticky)
          }
        }
        if (empty_node) {
          if (auto* n = memory->TranslateVirtual<uint8_t*>(empty_node)) {
            // DataNode {value=<empty array>, type=kDataArray(16)}
            xe::store_and_swap<uint32_t>(n + 0x0, empty_arr);
            xe::store_and_swap<uint32_t>(n + 0x4, 16);
          }
          safe = empty_node;
        }
      } else if (use_setupfont_lvalue_node) {
        safe = Dc3EnsureScratchBuffer(kernel_state, &g_dc3_safe_lvalue_datanode, 8,
                                      "safe_lvalue_datanode");
        if (safe) {
          if (auto* n = memory->TranslateVirtual<uint8_t*>(safe)) {
            // Assignment sink for mFont->Node(i+98) when mFont is null.
            xe::store_and_swap<uint32_t>(n + 0x0, 0);
            xe::store_and_swap<uint32_t>(n + 0x4, 0);
          }
        }
      } else {
        safe = Dc3EnsureScratchBuffer(kernel_state, &g_dc3_safe_null_datanode, 8,
                                      "safe_null_datanode");
        if (safe) {
          if (auto* n = memory->TranslateVirtual<uint8_t*>(safe)) {
            // DataNode {value=0, type=0}; preserves "Data 0 is not Array"
            // diagnostics while avoiding OOB DataNode* returns.
            xe::store_and_swap<uint32_t>(n + 0x0, 0);
            xe::store_and_swap<uint32_t>(n + 0x4, 0);
          }
        }
      }
      result = safe;

      static uint32_t oob_log_count = 0;
      ++oob_log_count;
      if (oob_log_count <= 200 || (oob_log_count % 1000) == 0) {
        XELOGW(
            "DC3: merged_DataArrayNode OOB intercepted LR={:08X} da={:08X} "
            "idx={} size={} nodes={:08X} -> safe={:08X}{}",
            lr, da_addr, index, static_cast<int>(size), nodes_ptr, result,
            use_setupfont_empty_array_node
                ? " [SetupFont empty-array source]"
                : (use_setupfont_lvalue_node ? " [SetupFont lvalue sink]" : ""));
      }

      static bool logged_setupfont_null = false;
      if ((use_setupfont_empty_array_node || use_setupfont_lvalue_node) &&
          da_addr == 0 && !logged_setupfont_null) {
        logged_setupfont_null = true;
        XELOGW("DC3: Rnd::SetupFont missing SystemConfig(\"rnd\",\"font\"); "
               "using safety sentinels to skip font table synthesis");
      }
    }

    ppc_context->r[3] = result;
  };

  ctx.processor->RegisterGuestFunctionOverride(kMergedDataArrayNodeAddr,
                                               merged_dataarray_node_handler,
                                               "DC3:merged_DataArrayNode(safe)");
  XELOGI("DC3: Registered merged_DataArrayNode safety override at {:08X}",
         kMergedDataArrayNodeAddr);
  debug_result.applied++;
}

void ApplyDc3ImportAndRuntimeStopgaps(const Dc3HackContext& ctx,
                                      Dc3HackPackSummary& summary) {
  auto& import_result = GetResult(summary, Dc3HackCategory::kImports);
  auto& stopgap_result =
      GetResult(summary, Dc3HackCategory::kDecompRuntimeStopgap);
  auto& debug_result = GetResult(summary, Dc3HackCategory::kDebug);
  auto& crt_result = GetResult(summary, Dc3HackCategory::kCrt);

  Memory* memory = ctx.memory;
  auto* module = ctx.module;
  if (!memory || !module) {
    import_result.failed++;
    stopgap_result.failed++;
    debug_result.failed++;
    crt_result.failed++;
    return;
  }

  if (cvars::dc3_debug_read_cache_stream_step_override) {
    RegisterDc3ReadCacheStreamProbe(ctx, debug_result);
  } else {
    XELOGI("DC3: ReadCacheStream step override disabled (default; non-invasive)");
  }

  // Stub XapiCallThreadNotifyRoutines and XRegisterThreadNotifyRoutine.
  const uint32_t kNotifyFuncs[] = {0x8302CEE0};
  for (uint32_t addr : kNotifyFuncs) {
    auto* heap = memory->LookupHeap(addr);
    if (heap) {
      heap->Protect(addr, 8, kMemoryProtectRead | kMemoryProtectWrite);
      auto* p = memory->TranslateVirtual<uint8_t*>(addr);
      xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
      xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
      XELOGI("DC3: Stubbed XapiCallThreadNotifyRoutines at {:08X} "
             "(li r3,0; blr)",
             addr);
      import_result.applied++;
    } else {
      import_result.skipped++;
    }
  }

  // The decomp XEX specifies a 256KB stack which overflows during init.
  if (module->stack_size() < 4 * 1024 * 1024) {
    XELOGI("DC3: Increasing main thread stack from {}KB to 4096KB",
           module->stack_size() / 1024);
    module->set_stack_size(4 * 1024 * 1024);
    stopgap_result.applied++;
  } else {
    stopgap_result.skipped++;
  }

  // Make the full PE image writable (0x82000000-0x83ED0000).
  // The decomp XEX has global DataNode/DataArray objects in .data sections
  // (e.g. 0x834880E0) that the game writes to at runtime.  The linker marks
  // these sections read-only because they contain initialized data, but the
  // game expects them writable.  Also covers RODATA/BSS workaround.
  {
    auto* heap = memory->LookupHeap(0x82000000);
    if (heap) {
      heap->Protect(0x82000000, 0x1ED0000,
                    kMemoryProtectRead | kMemoryProtectWrite);
      XELOGI("DC3: Made full image 0x82000000-0x83ED0000 writable");
      stopgap_result.applied++;

      // Temporary debug-only progression tool. These assertions trip after
      // known config/data corruption and should not be left enabled by default.
      if (cvars::dc3_debug_memmgr_assert_nop_bypass) {
        XELOGW("DC3: MemMgr assert nop bypass ENABLED (temporary debug mode)");
        struct MemPatch {
          uint32_t addr;
          uint32_t expected_word;
          const char* name;
        };
        const MemPatch mem_patches[] = {
            {0x83447AF4, 0x481032B9, "MemInit line 690: Debug::Fail call"},
            {0x83446A24, 0x48104389, "MemAlloc line 961: Debug::Fail call"},
        };
        for (const auto& p : mem_patches) {
          if (PatchCheckedNop(memory, p.addr, p.expected_word, p.name)) {
            stopgap_result.applied++;
          } else {
            stopgap_result.failed++;
          }
        }
      } else {
        XELOGI("DC3: MemMgr assert nop bypass disabled (default)");
        stopgap_result.skipped++;
      }

    // Initialize STLport std::list<bool> gConditional sentinel (0x83C7D354).
    {
      constexpr uint32_t kGConditionalAddr = 0x83C7D354;
      auto* pcond = memory->TranslateVirtual<uint8_t*>(kGConditionalAddr);
      if (pcond) {
        xe::store_and_swap<uint32_t>(pcond + 0, kGConditionalAddr); // _M_next
        xe::store_and_swap<uint32_t>(pcond + 4, kGConditionalAddr); // _M_prev
        XELOGI("DC3: Initialized gConditional sentinel at {:08X}", kGConditionalAddr);
        stopgap_result.applied++;
      }
    }

    } else {
      stopgap_result.skipped++;
    }
  }

  // Redirect DECOMP Debug::Print to XELOG so MILO_LOG output remains visible
  // without stubbing the CRT formatter path.
  if (ctx.processor) {
    constexpr uint32_t kDebugPrint = 0x835499C4;
    auto debug_print_handler = [](cpu::ppc::PPCContext* ppc_context,
                                  kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t str_ptr = static_cast<uint32_t>(ppc_context->r[4]);
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      if (memory && str_ptr && str_ptr < 0xF0000000) {
        if (auto* s = memory->TranslateVirtual<const char*>(str_ptr)) {
          XELOGI("DC3: Debug::Print this={:08X} LR={:08X}: {}", this_ptr, lr, s);
          return;
        }
      }
      XELOGI("DC3: Debug::Print this={:08X} LR={:08X} str={:08X}",
             this_ptr, lr, str_ptr);
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kDebugPrint, debug_print_handler, "DC3:Debug::Print(decomp)");
    XELOGI("DC3: Registered Debug::Print redirect at DECOMP {:08X}", kDebugPrint);
  }

  // Restore Debug::Fail visibility while keeping the runtime moving.
  if (ctx.processor) {
    constexpr uint32_t kDebugFail = 0x8354ADAC;
    auto fail_handler = [](cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t r3 = static_cast<uint32_t>(ppc_context->r[3]);  // Debug*
      uint32_t r4 = static_cast<uint32_t>(ppc_context->r[4]);  // msg
      static int fail_count = 0;
      fail_count++;
      XELOGE("DC3: Debug::Fail called! LR={:08X} r3={:08X} r4={:08X}", lr, r3, r4);
      if (memory) {
        static bool logged_debug_obj = false;
        if (!logged_debug_obj) {
          logged_debug_obj = true;
          if (auto* dbg = memory->TranslateVirtual<uint8_t*>(r3)) {
            uint32_t vtbl = xe::load_and_swap<uint32_t>(dbg + 0x00);
            uint32_t w1 = xe::load_and_swap<uint32_t>(dbg + 0x04);
            uint32_t w2 = xe::load_and_swap<uint32_t>(dbg + 0x08);
            uint32_t w3 = xe::load_and_swap<uint32_t>(dbg + 0x0C);
            XELOGE("DC3: TheDebug probe @{:08X}: vtbl={:08X} w1={:08X} w2={:08X} w3={:08X}",
                   r3, vtbl, w1, w2, w3);
          } else {
            XELOGE("DC3: TheDebug probe failed to translate object at {:08X}", r3);
          }
        }
        if (r4 && r4 < 0xF0000000) {
          if (auto* msg = memory->TranslateVirtual<const char*>(r4)) {
            XELOGE("DC3: Debug::Fail message: {}", msg);
          }
        }
      }
      if (fail_count <= 200) {
        XELOGW("DC3: Debug::Fail returning to caller (count={})", fail_count);
      } else if (fail_count == 201) {
        XELOGE("DC3: Debug::Fail called >200 times, spinning forever");
        while (true) {
        }
      }
      ppc_context->r[3] = 0;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kDebugFail, fail_handler, "DC3:Debug::Fail(log)");
    XELOGI("DC3: Registered Debug::Fail handler at {:08X} (logs LR + hangs)",
           kDebugFail);
  }

  // Map guard/overflow pages as readable zeros.
  // The decomp has stubs that return -1, causing game code to dereference
  // r31=0xFFFFFFFF in tight loops.  Guest addresses near 0xFFFFFFFF map to
  // file offsets past the shm backing size, so the kernel can't serve the
  // pages.  Also pre-map the 0x7F000000-0x7FFFFFFF GPU writeback region
  // and other unmapped areas to avoid tens of thousands of page faults
  // during register scanning.
#if defined(__linux__)
  {
    struct { uint32_t start; uint32_t size; const char* name; } regions[] = {
      {0x7F000000, 0x01000000, "GPU writeback 0x7F000000"},
      {0xFFFFF000, 0x00002000, "top-of-address-space guard"},
    };
    for (auto& r : regions) {
      uint8_t* host = memory->TranslateVirtual<uint8_t*>(r.start);
      void* result = mmap(host, r.size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
      if (result != MAP_FAILED) {
        XELOGI("DC3: Mapped {} ({} bytes)", r.name, r.size);
        stopgap_result.applied++;
      } else {
        XELOGW("DC3: Failed to map {}: {}", r.name, strerror(errno));
        stopgap_result.skipped++;
      }
    }
  }
#endif

  // Skip the Win32 FileIsLocal assert spam on "game:" drive. Xenia paths use
  // game/content devices, and returning from Debug::Fail causes a deep assert
  // storm that can overflow the guest stack before boot progresses.
  if (ctx.is_decomp_layout) {
    const uint32_t kFileIsLocalAssertBranch = 0x82B176F0;
    const uint32_t kExpected = 0x40820040;      // conditional branch to skip assert
    const uint32_t kUnconditional = 0x48000040; // always skip assert block
    if (PatchCheckedWord(memory, kFileIsLocalAssertBranch, kExpected,
                         kUnconditional, "FileIsLocal(game:) assert bypass",
                         "DC3: FileIsLocal game-drive assert bypass active")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Stub XMP overrides.
  // Do NOT stub _output_l/_woutput_l here: they are core CRT formatters used
  // by printf/sprintf/MakeString and stubbing them breaks string formatting.
  {
    struct OutputFunc {
      uint32_t address;
      const char* name;
    };
    OutputFunc output_funcs[] = {
        {0x835FAA8C, "XMPOverrideBackgroundMusic"},
        {0x835FAB64, "XMPRestoreBackgroundMusic"},
    };
    for (const auto& func : output_funcs) {
      if (PatchStub8Resolved(memory, ctx.hack_pack_stubs, func.address, 0,
                             func.name)) {
        debug_result.applied++;
      } else {
        debug_result.skipped++;
      }
    }
  }

  RegisterDc3WriteBridges(ctx, debug_result);
  RegisterDc3LocaleBootstrapBridges(ctx, debug_result);
  RegisterDc3OutputFormatterBridges(ctx, debug_result);
  RegisterDc3DataArraySafetyOverrides(ctx, debug_result);

  // Debug/Holmes/String-related stubs and deadlock breakers.
  {
    struct DebugFunc {
      uint32_t address;
      const char* name;
      uint32_t return_value;
    };
    DebugFunc debug_funcs[] = {
        {0x8393E7B0, "XGetLocale", 0},
        {0x8393E9B8, "XTLGetLanguage", 1},
        {0x838DF0DC, "DebugBreak", 0},
        // Keep the game-side locale mapping helpers live; only the XTL/XGet
        // platform queries need stubbing. Stubbing these caused downstream
        // null-locale config paths and stack-corrupting failure cascades.
        // {0x83409AA8, "GetSystemLanguage", 0},
        // {0x83409F68, "GetSystemLocale", 0},
        {0x830EDFF4, "DataNode::Print", 0},
        // Keep meta-material creation live. Stubbing this causes downstream
        // "Couldn't instantiate class Mat" failures and DataArray assert
        // cascades during renderer bootstrap.
        // {0x83140608, "RndMat::CreateMetaMaterial", 0},
        {0x8310CD88, "HolmesClientInit", 0},
        {0x8310D058, "HolmesClientReInit", 0},
        {0x8310D0D8, "HolmesClientPoll", 0},
        {0x8310C958, "HolmesClientPollInternal", 0},
        {0x8310CA70, "HolmesClientInitOpcode", 0},
        {0x8310BF14, "HolmesClientTerminate", 0},
        {0x8310B284, "CanUseHolmes", 0},
        {0x8310B2F8, "UsingHolmes", 0},
        {0x8310B154, "ProtocolDebugString", 0},
        {0x8310B314, "HolmesSetFileShare", 0},
        {0x8310B36C, "HolmesFileHostName", 0},
        {0x8310B378, "HolmesFileShare", 0},
        {0x8310B384, "HolmesResolveIP", 0},
        {0x8310B7E0, "BeginCmd", 0},
        {0x8310B830, "CheckForResponse", 0},
        {0x8310BA34, "WaitForAnyResponse", 0},
        {0x8310C664, "EndCmd", 0},
        {0x8310C76C, "CheckReads", 0},
        {0x8310C850, "CheckInput", 0},
        {0x8310C8F0, "WaitForResponse", 0},
        {0x8310E13C, "WaitForReads", 0},
        {0x8310C9B8, "HolmesClientPollKeyboard", 0},
        {0x8310CA10, "HolmesClientPollJoypad", 0},
        {0x8310D53C, "HolmesClientOpen", 0},
        {0x8310DA24, "HolmesClientRead", 0},
        {0x8310DB5C, "HolmesClientReadDone", 0},
        {0x8310D778, "HolmesClientWrite", 0},
        {0x8310D8E8, "HolmesClientTruncate", 0},
        {0x8310E1D8, "HolmesClientClose", 0},
        {0x8310D244, "HolmesClientGetStat", 0},
        {0x8310D150, "HolmesClientSysExec", 0},
        {0x8310D35C, "HolmesClientMkDir", 0},
        {0x8310D44C, "HolmesClientDelete", 0},
        {0x8310E360, "HolmesClientEnumerate", 0},
        {0x8310DC14, "HolmesClientCacheFile", 0},
        {0x8310DDE4, "HolmesClientCacheResource", 0},
        {0x8310BBC0, "HolmesToLocal", 0},
        {0x8310BCB0, "HolmesFlushStreamBuffer", 0},
        {0x8310BD44, "DumpHolmesLog", 0},
        {0x8310DF18, "HolmesClientStackTrace", 0},
        {0x8310E048, "HolmesClientSendMessage", 0},
    };
    for (const auto& func : debug_funcs) {
      if (PatchStub8Resolved(memory, ctx.hack_pack_stubs, func.address,
                             func.return_value, func.name)) {
        debug_result.applied++;
      } else {
        debug_result.skipped++;
      }
    }
  }

  // Stub String::operator+=(const char*).
  {
    const uint32_t kStringOpPlusEq = 0x834BE118;
    auto* heap = memory->LookupHeap(kStringOpPlusEq);
    if (heap) {
      auto* mem = memory->TranslateVirtual<uint8_t*>(kStringOpPlusEq);
      if (mem && xe::load_and_swap<uint32_t>(mem) != 0x00000000) {
        heap->Protect(kStringOpPlusEq, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(mem, 0x4E800020);
        XELOGI("DC3: Stubbed String::operator+=(const char*) at {:08X} "
               "(blr — prevents unbounded PE image corruption)",
               kStringOpPlusEq);
        debug_result.applied++;
      } else {
        debug_result.skipped++;
      }
    } else {
      debug_result.skipped++;
    }
  }

  // NOTE: The following decomp stopgaps (String::~String, NUISPEECH::CSpCfgInst,
  // recursive error-report helper, debug/assert helper) had hardcoded addresses
  // from a previous XEX build. After relinking (more units set to matching), these
  // addresses now point to unrelated code. Disabled until addresses are refreshed
  // from a new MAP file.
  //
  // Stale addresses (do NOT re-enable without verifying against current MAP):
  //   0x834BE094 - was String::~String, now mid-function bl
  //   0x82B324A0 - was NUISPEECH::CSpCfgInst, now zeroed out
  //   0x83346A2C - was recursive error-report, now function epilogue
  //   0x834B1240 - was debug/assert helper, now mid-function bctrl

  // Decomp-only CRT/TLS stopgap: guest `_errno` currently returns a bogus
  // handle-like pointer (observed `0x400006A8`) on the decomp build, which
  // feeds `_vsnprintf_l` invalid-parameter loops. Provide a stable guest int*
  // backing store via a guest extern override.
  if (ctx.is_decomp_layout && ctx.processor) {
    const uint32_t kErrnoAddr = 0x835B2D68;
    auto* p_errno_fn = memory->TranslateVirtual<uint8_t*>(kErrnoAddr);
    if (p_errno_fn && xe::load_and_swap<uint32_t>(p_errno_fn) != 0x00000000) {
      uint32_t errno_ptr = g_dc3_errno_guest_ptr;
      if (!errno_ptr) {
        errno_ptr = memory->SystemHeapAlloc(4, 4);
        if (errno_ptr) {
          if (auto* p_errno = memory->TranslateVirtual<uint8_t*>(errno_ptr)) {
            xe::store_and_swap<uint32_t>(p_errno, 0);
          }
          g_dc3_errno_guest_ptr = errno_ptr;
        }
      }
      if (errno_ptr) {
        ctx.processor->RegisterGuestFunctionOverride(kErrnoAddr, Dc3ErrnoExtern,
                                                     "_errno (decomp stopgap)");
        XELOGI("DC3: Registered decomp stopgap guest override for _errno at {:08X} -> {:08X}",
               kErrnoAddr, errno_ptr);
        stopgap_result.applied++;
      } else {
        XELOGW("DC3: Failed to allocate guest errno backing storage for decomp _errno override");
        stopgap_result.failed++;
      }
    } else {
      stopgap_result.skipped++;
    }
  }

  // Decomp-only CRT stopgap: _invalid_parameter_noinfo (invarg.obj) checks
  // __pInvalidArgHandler function pointer (at 0x83C75734 in this build).
  // When it's NULL (CRT not fully initialized), it traps with tw 0x16
  // (EINVAL) which crashes the JIT host. Stub it to just return.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, 0x835D428C, 0,
                   "_invalid_parameter_noinfo (CRT trap stopgap)")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
  }

  // Decomp-only formatting guard: in the current decomp build, TaskMgr ctor
  // -> FormatString::operator<< -> Hx_snprintf enters _vsnprintf_l invalid-
  // parameter recursion. Patch the _vsnprintf_l call inside Hx_snprintf so
  // Hx_snprintf's own error path still runs (it null-terminates and returns
  // -1) instead of bypassing local cleanup at the higher-level callsite.
  if (ctx.is_decomp_layout) {
    const uint32_t kHxSnprintfVsnprintfCall = 0x83477FBC;
    auto* heap = memory->LookupHeap(kHxSnprintfVsnprintfCall);
    auto* p = memory->TranslateVirtual<uint8_t*>(kHxSnprintfVsnprintfCall);
    if (heap && p) {
      uint32_t w = xe::load_and_swap<uint32_t>(p);
      if (w == 0x4813BE35) {
        heap->Protect(kHxSnprintfVsnprintfCall, 4,
                      kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(p, 0x3860FFFF);  // li r3,-1
        XELOGI("DC3: Patched Hx_snprintf _vsnprintf_l call {:08X} -> li r3,-1",
               kHxSnprintfVsnprintfCall);
        stopgap_result.applied++;
      } else if (w == 0x3860FFFF) {
        stopgap_result.applied++;
      } else {
        XELOGW("DC3: Unexpected Hx_snprintf _vsnprintf_l callsite word {:08X} at {:08X}",
               w, kHxSnprintfVsnprintfCall);
        stopgap_result.skipped++;
      }
    } else {
      stopgap_result.skipped++;
    }
  }

  // NOTE: We previously stubbed `except_data_82910450` at `0x82910448`, but
  // thread-6 telemetry later showed the `0x82910438..0x82910450` range on a
  // live NavListSortMgr path. Stubbing this 8-byte region to `li r3,0; blr`
  // can create a stable loop (returns without the surrounding epilogue). Keep
  // the original bytes until we root-cause the control-flow corruption that
  // reaches this region.

  // Stub unresolved import entries (PE thunks + XEX markers).
  {
    const uint32_t kTextStart = 0x822E0000;
    const uint32_t kTextSize = 0x171A414;
    const uint32_t kIdataStart = 0x822D8400;
    const uint32_t kIdataEnd = 0x822D8A34;
    const uint32_t kThunkAreaStart = 0x8395B000;
    const uint32_t kThunkAreaEnd = 0x8395D000;

    int pe_thunks_stubbed = 0;
    int xex_markers_stubbed = 0;

    for (uint32_t off = 0; off + 16 <= kTextSize; off += 4) {
      uint32_t addr = kTextStart + off;
      auto* p = memory->TranslateVirtual<uint8_t*>(addr);
      if (!p) continue;

      uint32_t w0 = xe::load_and_swap<uint32_t>(p);
      uint32_t w1 = xe::load_and_swap<uint32_t>(p + 4);
      uint32_t w2 = xe::load_and_swap<uint32_t>(p + 8);
      uint32_t w3 = xe::load_and_swap<uint32_t>(p + 12);

      bool need_stub = false;
      if ((w0 >> 16) == 0x3D60 && (w1 >> 16) == 0x816B && w2 == 0x7D6903A6 &&
          w3 == 0x4E800420) {
        uint32_t hi = w0 & 0xFFFF;
        int16_t lo = static_cast<int16_t>(w1 & 0xFFFF);
        uint32_t iat_addr = (hi << 16) + lo;
        if (iat_addr >= kIdataStart && iat_addr < kIdataEnd) {
          need_stub = true;
          pe_thunks_stubbed++;
          XELOGI("DC3: Stubbed unrewritten PE import thunk at {:08X} "
                 "(IAT {:08X})",
                 addr, iat_addr);
        }
      }

      if (!need_stub && addr >= kThunkAreaStart && addr < kThunkAreaEnd &&
          (w0 >> 24) == 0x01 && w1 == 0 && w2 == 0 && w3 == 0) {
        need_stub = true;
        xex_markers_stubbed++;
        uint16_t ordinal = w0 & 0xFFFF;
        XELOGI("DC3: Stubbed unresolved XEX import marker at {:08X} "
               "(ordinal 0x{:04X})",
               addr, ordinal);
      }
      if (!need_stub) continue;

      auto* heap = memory->LookupHeap(addr);
      if (!heap) {
        import_result.failed++;
        continue;
      }
      heap->Protect(addr, 16, kMemoryProtectRead | kMemoryProtectWrite);
      xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
      xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
      xe::store_and_swap<uint32_t>(p + 8, 0x60000000);
      xe::store_and_swap<uint32_t>(p + 12, 0x60000000);
    }
    XELOGI("DC3: Import thunk cleanup: {} PE thunks + {} XEX markers = {} "
           "total stubbed",
           pe_thunks_stubbed, xex_markers_stubbed,
           pe_thunks_stubbed + xex_markers_stubbed);
    import_result.applied += pe_thunks_stubbed + xex_markers_stubbed;
  }

  // Zero page mapping (all zeros) + null-deref guard below virtual_membase.
  {
    auto* heap = memory->LookupHeap(0x00000000);
    if (heap) {
      heap->Protect(0x00000000, 0x10000, kMemoryProtectRead | kMemoryProtectWrite);
      auto* base = memory->TranslateVirtual<uint8_t*>(0x00000000);
      std::memset(base, 0, 0x10000);
      XELOGI("DC3: Mapped zero page 0x0-0x10000 (all zeros — null "
             "object reads return 0, null checks work correctly)");
      stopgap_result.applied++;
    } else {
      stopgap_result.failed++;
    }

#if defined(__linux__)
    {
      auto* vmbase = memory->virtual_membase();
      void* guard_base = vmbase - 0x10000;
      void* result =
          mmap(guard_base, 0x10000, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (result != MAP_FAILED) {
        XELOGI("DC3: Mapped null-deref guard page at {:p} "
               "(64KB below virtual_membase {:p})",
               result, (void*)vmbase);
        stopgap_result.applied++;
      } else {
        XELOGI("DC3: Could not map null-deref guard page below "
               "virtual_membase (errno={})",
               errno);
        stopgap_result.skipped++;
      }
    }
#else
    stopgap_result.skipped++;
#endif
  }

  // CRT sanitizer + injection.
  {
    const uint32_t kCodeStart = 0x822C0000;
    struct CrtTable {
      uint32_t start;
      uint32_t end;
      const char* name;
    };
    // Source-of-truth is the current decomp MAP. Some manifests/symbols files
    // can lag relinks and point at stale constructor tables.
    constexpr uint32_t kXcA = 0x83ADED98;
    constexpr uint32_t kXcZ = 0x83ADF3B0;
    constexpr uint32_t kXiA = 0x83ADF3B4;
    constexpr uint32_t kXiZ = 0x83ADF3C0;
    if (ctx.crt_sentinels) {
      uint32_t m_xc_a = LookupStubAddr(ctx.crt_sentinels, "__xc_a", kXcA);
      uint32_t m_xc_z = LookupStubAddr(ctx.crt_sentinels, "__xc_z", kXcZ);
      uint32_t m_xi_a = LookupStubAddr(ctx.crt_sentinels, "__xi_a", kXiA);
      uint32_t m_xi_z = LookupStubAddr(ctx.crt_sentinels, "__xi_z", kXiZ);
      if (m_xc_a != kXcA || m_xc_z != kXcZ || m_xi_a != kXiA || m_xi_z != kXiZ) {
        XELOGW(
            "DC3: Manifest CRT sentinels differ from map-synced constants; "
            "using map values (__xc_a={:08X} __xc_z={:08X} __xi_a={:08X} __xi_z={:08X})",
            kXcA, kXcZ, kXiA, kXiZ);
      }
    }
    CrtTable tables[] = {
        {kXcA, kXcZ, "__xc_a..__xc_z (C++ constructors)"},
        {kXiA, kXiZ, "__xi_a..__xi_z (C initializers)"},
    };
    const int32_t bisect_max = cvars::dc3_crt_bisect_max;
    if (bisect_max >= 0) {
      XELOGI("DC3: CRT bisect mode ACTIVE - only allowing constructor "
             "indices 0..{} (inclusive)",
             bisect_max);
    }

    std::set<int> skip_set;
    auto parse_skip_list = [&](const std::string& str) {
      size_t pos = 0;
      while (pos < str.size()) {
        size_t comma = str.find(',', pos);
        if (comma == std::string::npos) comma = str.size();
        std::string token = str.substr(pos, comma - pos);
        if (!token.empty()) {
          size_t dash = token.find('-');
          if (dash != std::string::npos && dash > 0) {
            int lo = std::stoi(token.substr(0, dash));
            int hi = std::stoi(token.substr(dash + 1));
            for (int i = lo; i <= hi; ++i) skip_set.insert(i);
          } else {
            skip_set.insert(std::stoi(token));
          }
        }
        pos = comma + 1;
      }
    };
    if (cvars::dc3_crt_skip_nui && cvars::dc3_crt_skip_indices.empty()) {
      parse_skip_list("75,98-101,210-328");
      XELOGI("DC3: Auto-NUI skip enabled (75,98-101,210-328)");
    }
    if (!cvars::dc3_crt_skip_indices.empty()) {
      parse_skip_list(cvars::dc3_crt_skip_indices);
    }
    if (!skip_set.empty()) {
      XELOGI("DC3: CRT skip list has {} entries", skip_set.size());
    }

    for (const auto& table : tables) {
      auto* crt_heap = memory->LookupHeap(table.start);
      if (crt_heap) {
        uint32_t page_start = table.start & ~0xFFFu;
        uint32_t page_end = (table.end + 0xFFFu) & ~0xFFFu;
        crt_heap->Protect(page_start, page_end - page_start,
                          kMemoryProtectRead | kMemoryProtectWrite);
      }
      int nullified_oob = 0;
      int nullified_bisect = 0;
      int nullified_skip = 0;
      int valid_count = 0;
      int already_null = 0;
      int total = (table.end - table.start) / 4;
      int index = 0;
      for (uint32_t addr = table.start; addr < table.end; addr += 4, ++index) {
        auto* p = memory->TranslateVirtual<uint8_t*>(addr);
        uint32_t entry = xe::load_and_swap<uint32_t>(p);
        if (entry == 0) {
          already_null++;
          continue;
        }
        if (entry < kCodeStart) {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-oob, not in code section)",
                 index, entry);
          xe::store_and_swap<uint32_t>(p, 0);
          nullified_oob++;
        } else if (bisect_max >= 0 && index > bisect_max) {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-bisect, index > {})",
                 index, entry, bisect_max);
          xe::store_and_swap<uint32_t>(p, 0);
          nullified_bisect++;
        } else if (!skip_set.empty() && skip_set.count(index)) {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-skip, in skip list)", index,
                 entry);
          auto* fn = memory->TranslateVirtual<uint8_t*>(entry);
          if (fn) {
            XELOGI("DC3:   PPC instructions at {:08X}:", entry);
            for (int i = 0; i < 8; ++i) {
              uint32_t insn = xe::load_and_swap<uint32_t>(fn + i * 4);
              XELOGI("DC3:     {:08X}: {:08X}", entry + i * 4, insn);
            }
          }
          xe::store_and_swap<uint32_t>(p, 0);
          nullified_skip++;
        } else {
          XELOGI("DC3: CRT[{:3d}] = {:08X} (valid)", index, entry);
          valid_count++;
        }
      }
      XELOGI("DC3: CRT table {}: {} total entries, {} already null, {} valid, "
             "{} nullified-oob, {} nullified-bisect, {} nullified-skip",
             table.name, total, already_null, valid_count, nullified_oob,
             nullified_bisect, nullified_skip);
      crt_result.applied++;
    }
  }

  // Diagnostics: check JIT indirection table state for import thunk area.
  {
    auto* code_cache = reinterpret_cast<uint8_t*>(0x80000000);
    uint32_t check_addrs[] = {
        0x8395C668,
        0x8395C000,
        0x8395B000,
        0x822E0000,
    };
    for (auto addr : check_addrs) {
      uint32_t* slot =
          reinterpret_cast<uint32_t*>(code_cache + (addr - 0x80000000));
      XELOGI("DC3: Indirection table [{:08X}] = {:08X}", addr, *slot);
      import_result.applied++;
    }
  }

  RegisterDc3FindArrayOverride(ctx, debug_result);
}
}  // namespace

const char* Dc3HackCategoryName(Dc3HackCategory category) {
  switch (category) {
    case Dc3HackCategory::kCrt:
      return "crt";
    case Dc3HackCategory::kSkeleton:
      return "skeleton";
    case Dc3HackCategory::kDebug:
      return "debug";
    case Dc3HackCategory::kDecompRuntimeStopgap:
      return "decomp_stopgap";
    case Dc3HackCategory::kImports:
      return "imports";
    default:
      return "unknown";
  }
}

void Dc3MaybeCleanStaleContentCache(const std::filesystem::path& content_root) {
  auto dc3_content = content_root / "373307D9";
  if (std::filesystem::exists(dc3_content)) {
    XELOGI("DC3: Cleaning stale content cache at {}", xe::path_to_utf8(dc3_content));
    std::error_code ec;
    std::filesystem::remove_all(dc3_content, ec);
    if (ec) {
      XELOGI("DC3: Failed to clean content cache: {}", ec.message());
    }
  }
}

Dc3HackPackSummary ApplyDc3HackPack(const Dc3HackContext& ctx) {
  Dc3HackPackSummary summary;
  // Seed categories so emulator summary logging is stable even before skeleton
  // extraction lands.
  GetResult(summary, Dc3HackCategory::kImports);
  GetResult(summary, Dc3HackCategory::kDecompRuntimeStopgap);
  GetResult(summary, Dc3HackCategory::kDebug);
  GetResult(summary, Dc3HackCategory::kCrt);
  GetResult(summary, Dc3HackCategory::kSkeleton);

  ApplyDc3ImportAndRuntimeStopgaps(ctx, summary);
  return summary;
}

Dc3HackApplyResult ApplyDc3SkeletonHackPack(const Dc3HackContext& ctx) {
  Dc3HackApplyResult result;
  result.category = Dc3HackCategory::kSkeleton;

  if (!ctx.memory) {
    result.failed++;
    return result;
  }
  if (!cvars::fake_kinect_data || ctx.is_decomp_layout) {
    result.skipped++;
    return result;
  }

  Memory* memory = ctx.memory;
  const uint32_t kGetNextFrameAddr = 0x829C2790;
  const uint32_t kSkeletonFrameSize = 0xAB0;  // 2736 bytes
  const uint32_t kDataSize = kSkeletonFrameSize + 4;  // +4 for counter

  uint32_t data_guest_addr = memory->SystemHeapAlloc(kDataSize, 0x10);
  if (!data_guest_addr) {
    XELOGW("DC3: Failed to allocate guest memory for fake skeleton data");
    result.failed++;
    return result;
  }
  const uint32_t kSkeletonDataAddr = data_guest_addr;
  const uint32_t kCounterAddr = data_guest_addr + kSkeletonFrameSize;

  auto* heap = memory->LookupHeap(kGetNextFrameAddr);
  if (!heap) {
    result.failed++;
    return result;
  }

  heap->Protect(kGetNextFrameAddr, 0x4C, kMemoryProtectRead | kMemoryProtectWrite);
  auto* stub_mem = memory->TranslateVirtual<uint8_t*>(kGetNextFrameAddr);
  auto* counter_mem = memory->TranslateVirtual<uint8_t*>(kCounterAddr);
  auto* data_mem = memory->TranslateVirtual<uint8_t*>(kSkeletonDataAddr);
  if (!stub_mem || !counter_mem || !data_mem) {
    XELOGW("DC3: Failed to translate memory for fake Kinect skeleton injection");
    result.failed++;
    return result;
  }

  uint32_t ppc_stub[] = {
      0x7C882378,                                // mr r8, r4
      0x3CA00000 | (kSkeletonDataAddr >> 16),    // lis r5, hi16(data)
      0x60A50000 | (kSkeletonDataAddr & 0xFFFF), // ori r5, r5, lo16(data)
      0x38C00000 | (kSkeletonFrameSize / 4),     // li r6, word_count
      0x7CC903A6,                                // mtctr r6
      0x80E50000,                                // lwz r7, 0(r5)
      0x90E40000,                                // stw r7, 0(r4)
      0x38A50004,                                // addi r5, r5, 4
      0x38840004,                                // addi r4, r4, 4
      0x4200FFF0,                                // bdnz -16 (to lwz)
      0x3CA00000 | (kCounterAddr >> 16),         // lis r5, hi16(counter)
      0x60A50000 | (kCounterAddr & 0xFFFF),      // ori r5, r5, lo16(counter)
      0x80C50000,                                // lwz r6, 0(r5)
      0x38C60001,                                // addi r6, r6, 1
      0x90C50000,                                // stw r6, 0(r5)
      0x90C80004,                                // stw r6, 4(r8) (timestamp)
      0x90C80008,                                // stw r6, 8(r8) (frame num)
      0x38600000,                                // li r3, 0 (S_OK)
      0x4E800020,                                // blr
  };
  for (size_t i = 0; i < sizeof(ppc_stub) / sizeof(ppc_stub[0]); i++) {
    xe::store_and_swap<uint32_t>(stub_mem + i * 4, ppc_stub[i]);
  }

  xe::store_and_swap<uint32_t>(counter_mem, 0);
  std::memset(data_mem, 0, kSkeletonFrameSize);

  auto write_float = [data_mem](uint32_t offset, float value) {
    xe::store_and_swap<float>(data_mem + offset, value);
  };
  auto write_u32 = [data_mem](uint32_t offset, uint32_t value) {
    xe::store_and_swap<uint32_t>(data_mem + offset, value);
  };

  write_u32(0x0008, 1);
  write_float(0x0014, 1.0f);
  write_float(0x0024, 1.0f);

  const uint32_t skel0 = 0x30;
  write_u32(skel0 + 0x00, 2);
  write_u32(skel0 + 0x04, 1);
  write_u32(skel0 + 0x0C, 0);
  write_float(skel0 + 0x10, 0.0f);
  write_float(skel0 + 0x14, 0.9f);
  write_float(skel0 + 0x18, 2.0f);
  write_float(skel0 + 0x1C, 1.0f);

  struct JointPos {
    float x, y, z;
  };
  JointPos joints[20] = {
      {0.00f, 0.90f, 2.0f},   {0.00f, 1.10f, 2.0f},   {0.00f, 1.35f, 2.0f},
      {0.00f, 1.60f, 2.0f},   {-0.20f, 1.35f, 2.0f},  {-0.50f, 1.35f, 2.0f},
      {-0.75f, 1.35f, 2.0f},  {-0.85f, 1.35f, 2.0f},  {0.20f, 1.35f, 2.0f},
      {0.50f, 1.35f, 2.0f},   {0.75f, 1.35f, 2.0f},   {0.85f, 1.35f, 2.0f},
      {-0.15f, 0.90f, 2.0f},  {-0.15f, 0.50f, 2.0f},  {-0.15f, 0.05f, 2.0f},
      {0.15f, 0.90f, 2.0f},   {0.15f, 0.50f, 2.0f},   {0.15f, 0.05f, 2.0f},
      {-0.15f, 0.00f, 2.0f},  {0.15f, 0.00f, 2.0f},
  };
  const uint32_t joints_offset = skel0 + 0x20;
  for (int j = 0; j < 20; j++) {
    uint32_t off = joints_offset + j * 16;
    write_float(off + 0, joints[j].x);
    write_float(off + 4, joints[j].y);
    write_float(off + 8, joints[j].z);
    write_float(off + 12, 1.0f);
  }
  const uint32_t tracking_offset = skel0 + 0x160;
  for (int j = 0; j < 20; j++) {
    write_u32(tracking_offset + j * 4, 2);
  }

  struct BinaryPatch {
    uint32_t address;
    uint32_t value;
    const char* name;
  };
  BinaryPatch skel_patches[] = {
      {0x8242E74C, 0x3B800021,
       "SkeletonUpdateThread: timeout INFINITE -> 33ms"},
      {0x8242E1B0, 0x60000000, "SkeletonUpdate::Update: NOP IsOverride branch"},
  };
  for (const auto& p : skel_patches) {
    auto* h = memory->LookupHeap(p.address);
    if (!h) {
      result.failed++;
      continue;
    }
    h->Protect(p.address, 4, kMemoryProtectRead | kMemoryProtectWrite);
    auto* m = memory->TranslateVirtual<uint8_t*>(p.address);
    if (!m) {
      result.failed++;
      continue;
    }
    xe::store_and_swap<uint32_t>(m, p.value);
    XELOGI("  Patched {:08X}: {}", p.address, p.name);
    result.applied++;
  }

  XELOGI("DC3: Fake Kinect skeleton data written at {:08X} ({} bytes), "
         "PPC stub at {:08X}, counter at {:08X}",
         kSkeletonDataAddr, kSkeletonFrameSize, kGetNextFrameAddr, kCounterAddr);
  result.applied++;
  return result;
}

}  // namespace xe
