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
DECLARE_bool(dc3_debug_mempool_alloc_probe);
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
// Addresses from build/373307D9/default.map (fresh relink 2026-02-25).
constexpr uint32_t kDc3RcsReadCacheStreamAddr = 0x83115974;
constexpr uint32_t kDc3RcsBufStreamReadImplAddr = 0x82BC2E90;
constexpr uint32_t kDc3RcsBufStreamSeekImplAddr = 0x82BC2F90;
constexpr uint32_t kDc3OutputLAddr = 0x836192F0;
constexpr uint32_t kDc3WOutputLAddr = 0x8361EE64;
constexpr uint32_t kDc3SystemConfig2Addr = 0x8351340C;
// TODO: Recompute SetupFont LR from disassembly. This is the return address
// from the SystemConfig(Symbol,Symbol) call inside SetupFont.
constexpr uint32_t kDc3SetupFontSystemConfigReturnLR = 0x8317FF14;  // STALE
// TODO: SetupFont ctor literals are anonymous string constants; resolve from
// disassembly of SetupFont after relink.
constexpr uint32_t kDc3SetupFontCtor1LiteralAddr = 0x82027684;  // expected "font" STALE
constexpr uint32_t kDc3SetupFontCtor2LiteralAddr = 0x82053BF8;  // expected "rnd" STALE
constexpr uint32_t kDc3PooledFontStringAddr = 0x82017684;  // STALE
constexpr uint32_t kDc3ObjectFactoriesMapAddr = 0x83AE1E00;
constexpr uint32_t kDc3RndMatStaticNameSymAddr = 0x83AEAF2C;
constexpr uint32_t kDc3MetaMaterialStaticNameSymAddr = 0x83AEC3A8;
constexpr uint32_t kDc3GSystemConfigAddr = 0x83C7B2E0;
constexpr uint32_t kDc3GStringTableGlobalAddr = 0x83AE0190;
constexpr uint32_t kDc3GHashTableAddr = 0x83AED4FC;
// MemOrPoolAlloc probe addresses (from default.exe.MAP)
constexpr uint32_t kDc3MemOrPoolAllocAddr = 0x82877950;
constexpr uint32_t kDc3MemAllocAddr = 0x82878158;  // _MemAllocTemp
constexpr uint32_t kDc3PoolAllocAddr = 0x835E61D4;
constexpr uint32_t kDc3StringReserveAddr = 0x82A5B1D0;
// TODO: Recompute String::reserve MemOrPoolAlloc call return LR from disasm.
constexpr uint32_t kDc3StringReserveMemAllocRetLR = 0x82A5BC00;  // STALE
// gChunkAlloc: global ChunkAllocator* used by PoolAlloc
constexpr uint32_t kDc3GChunkAllocAddr = 0x83CB8500;
// FixedSizeAlloc layout offsets
constexpr uint32_t kFsaOffVtable = 0x00;
constexpr uint32_t kFsaOffAllocSizeWords = 0x04;
constexpr uint32_t kFsaOffNumAllocs = 0x08;
constexpr uint32_t kFsaOffMaxAllocs = 0x0C;
constexpr uint32_t kFsaOffNumChunks = 0x10;
constexpr uint32_t kFsaOffFreeList = 0x14;
constexpr uint32_t kFsaOffNodesPerChunk = 0x18;
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

uint32_t Dc3FindArrayLinearBySymbol(Memory* memory, uint32_t da_addr,
                                    uint32_t sym_addr) {
  if (!memory || !da_addr || !sym_addr || da_addr >= 0xF0000000 ||
      sym_addr >= 0xF0000000) {
    return 0;
  }
  auto* sym_s = memory->TranslateVirtual<const char*>(sym_addr);
  if (!sym_s) return 0;
  auto* da = memory->TranslateVirtual<uint8_t*>(da_addr);
  if (!da) return 0;
  uint32_t nodes_ptr = xe::load_and_swap<uint32_t>(da + 0x0);
  int16_t da_size = xe::load_and_swap<int16_t>(da + 0x8);
  if (!nodes_ptr || nodes_ptr >= 0xF0000000 || da_size <= 0 || da_size >= 0x2000) {
    return 0;
  }
  auto* nodes = memory->TranslateVirtual<uint8_t*>(nodes_ptr);
  if (!nodes) return 0;
  for (int i = 0; i < da_size; ++i) {
    uint8_t* n = nodes + i * 8;
    uint32_t val = xe::load_and_swap<uint32_t>(n + 0);
    uint32_t type = xe::load_and_swap<uint32_t>(n + 4);
    if (type != 16 || !val || val >= 0xF0000000) continue;
    auto* sub = memory->TranslateVirtual<uint8_t*>(val);
    if (!sub) continue;
    uint32_t sub_nodes = xe::load_and_swap<uint32_t>(sub + 0x0);
    int16_t sub_size = xe::load_and_swap<int16_t>(sub + 0x8);
    if (!sub_nodes || sub_nodes >= 0xF0000000 || sub_size <= 0 || sub_size >= 0x2000) {
      continue;
    }
    auto* sn = memory->TranslateVirtual<uint8_t*>(sub_nodes);
    if (!sn) continue;
    uint32_t sn_val = xe::load_and_swap<uint32_t>(sn + 0);
    uint32_t sn_type = xe::load_and_swap<uint32_t>(sn + 4);
    if (sn_type != 5 || !sn_val || sn_val >= 0xF0000000) continue;
    auto* sn_str = memory->TranslateVirtual<const char*>(sn_val);
    if (sn_str && std::strcmp(sn_str, sym_s) == 0) {
      return val;
    }
  }
  return 0;
}

std::string Dc3DescribeGuestSymbol(Memory* memory, uint32_t sym_addr,
                                   char* sym_str_out, size_t sym_str_out_len,
                                   bool* out_binary) {
  if (out_binary) *out_binary = false;
  if (sym_str_out && sym_str_out_len) sym_str_out[0] = '\0';
  if (!memory || !sym_addr || sym_addr >= 0xF0000000) return "<null>";
  auto* s = memory->TranslateVirtual<const char*>(sym_addr);
  if (!s) return "<unmapped>";
  if (sym_str_out && sym_str_out_len) {
    std::strncpy(sym_str_out, s, sym_str_out_len - 1);
    sym_str_out[sym_str_out_len - 1] = '\0';
  }
  bool printable = true;
  for (size_t i = 0; i < 64 && s[i]; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c > 0x7E) {
      printable = false;
      break;
    }
  }
  if (printable) {
    if (sym_str_out && sym_str_out[0]) return sym_str_out;
    return "";
  }
  if (out_binary) *out_binary = true;
  return std::string("<bin:") +
         Dc3FmtBytePreview(reinterpret_cast<const uint8_t*>(s), 16) + ">";
}

void Dc3LogSetupFontLiteralSanity(Memory* memory) {
  static bool logged = false;
  if (logged || !memory) return;
  logged = true;

  auto* l1 = memory->TranslateVirtual<const char*>(kDc3SetupFontCtor1LiteralAddr);
  auto* l2 = memory->TranslateVirtual<const char*>(kDc3SetupFontCtor2LiteralAddr);
  if (!l1 || !l2) {
    XELOGW("DC3: SetupFont literal sanity check could not map literals "
           "ctor1@{:08X}={} ctor2@{:08X}={}",
           kDc3SetupFontCtor1LiteralAddr, static_cast<const void*>(l1),
           kDc3SetupFontCtor2LiteralAddr, static_cast<const void*>(l2));
    return;
  }

  bool l1_bin = false;
  bool l2_bin = false;
  char tmp1[32] = {};
  char tmp2[32] = {};
  std::string l1_desc =
      Dc3DescribeGuestSymbol(memory, kDc3SetupFontCtor1LiteralAddr, tmp1, sizeof(tmp1),
                             &l1_bin);
  std::string l2_desc =
      Dc3DescribeGuestSymbol(memory, kDc3SetupFontCtor2LiteralAddr, tmp2, sizeof(tmp2),
                             &l2_bin);

  XELOGI(
      "DC3: SetupFont literal sanity ctor1/arg2@{:08X}={} ctor2/arg1@{:08X}={}",
      kDc3SetupFontCtor1LiteralAddr, l1_desc, kDc3SetupFontCtor2LiteralAddr, l2_desc);

  const bool l2_is_rnd = (!l2_bin && l2_desc == "rnd");
  const bool l1_is_font = (!l1_bin && l1_desc == "font");
  if (l2_is_rnd && !l1_is_font) {
    XELOGW(
        "DC3: SetupFont arg2 literal mismatch (expected 'font', saw {}). "
        "Current decomp build shows ctor1 literal as non-string data while "
        "ctor2/arg1 remains 'rnd'; investigate decomp object/relink freshness "
        "and PPC hi/ha relocation correctness for string literals.",
        l1_desc);
  }
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
      {0x83614508, "_write_nolock"},
      {0x83614748, "_write"},
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
      {0x827F808C, "GetSystemLanguage"},
      {0x827F8774, "GetSystemLocale"},
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

void RegisterDc3MemOrPoolAllocProbe(const Dc3HackContext& ctx,
                                    Dc3HackApplyResult& debug_result) {
  if (!ctx.processor || !ctx.is_decomp_layout) {
    debug_result.skipped++;
    return;
  }
  if (!cvars::dc3_debug_mempool_alloc_probe) {
    XELOGI("DC3: MemOrPoolAlloc probe disabled (default)");
    debug_result.skipped++;
    return;
  }

  auto handler = [](cpu::ppc::PPCContext* ppc_context,
                    kernel::KernelState* kernel_state) {
    if (!ppc_context || !kernel_state) return;
    auto* memory = kernel_state->memory();
    auto* processor = kernel_state->processor();
    if (!memory || !processor) return;

    uint32_t size = static_cast<uint32_t>(ppc_context->r[3]);
    uint32_t file_ptr = static_cast<uint32_t>(ppc_context->r[4]);
    uint32_t line = static_cast<uint32_t>(ppc_context->r[5]);
    uint32_t name_ptr = static_cast<uint32_t>(ppc_context->r[6]);
    uint32_t caller_lr = static_cast<uint32_t>(ppc_context->lr);

    // Reimplement MemOrPoolAlloc dispatch logic:
    //   size == 0 -> return null
    //   size > 0x80 -> MemAlloc(size, file, line, name, 0)
    //   else -> PoolAlloc(size, size, file, line, name)
    uint64_t result = 0;
    if (size == 0) {
      result = 0;
    } else {
      auto* thread_state = ppc_context->thread_state;
      if (static_cast<int32_t>(size) > 0x80) {
        // MemAlloc(int size, const char* file, int line, const char* name,
        //          int heap = 0)
        uint64_t args[5] = {size, file_ptr, line, name_ptr, 0};
        result = processor->Execute(thread_state, kDc3MemAllocAddr, args, 5);
      } else {
        // PoolAlloc(int size, int pool_size, const char* file, int line,
        //           const char* name)
        uint64_t args[5] = {size, size, file_ptr, line, name_ptr};
        result = processor->Execute(thread_state, kDc3PoolAllocAddr, args, 5);
      }
    }

    uint32_t result32 = static_cast<uint32_t>(result);

    // Check for Execute dispatch failure (0xDEADBABE means function not found).
    if (result == 0xDEADBABE) {
      XELOGW("DC3: MemOrPoolAlloc dispatch FAILED (Execute returned DEADBABE) "
             "LR={:08X} size={} path={}",
             caller_lr, static_cast<int32_t>(size),
             (static_cast<int32_t>(size) > 0x80) ? "MemAlloc" : "PoolAlloc");
    }

    // Sanitize invalid allocator returns. When the MemMgr assert nop bypass is
    // active, MemAlloc can continue past a failed assertion with uninitialized
    // stack data (e.g. 0xFFFFFFFF) instead of halting. Convert these to null
    // so callers that do null-check (or crash cleanly on null) don't dereference
    // garbage pointers in wrapping-address territory.
    if (result32 >= 0xF0000000 && result32 != 0) {
      XELOGW(
          "DC3: MemOrPoolAlloc result sanitized {:08X} -> 0 (suspected garbage "
          "from nop'd assert) LR={:08X} size={}",
          result32, caller_lr, static_cast<int32_t>(size));
      result32 = 0;
    }

    ppc_context->r[3] = result32;

    // Determine if this is interesting enough to log.
    bool is_failure = (result32 == 0xFFFFFFFF || result32 == 0);
    bool is_string_reserve_caller = (caller_lr == kDc3StringReserveMemAllocRetLR);

    static uint32_t total_calls = 0;
    static uint32_t failure_count = 0;
    ++total_calls;
    if (is_failure && result32 != 0) ++failure_count;

    // Log on: any failure, String::reserve caller, or first few calls, or
    // periodic sampling.
    if (is_failure || is_string_reserve_caller || total_calls <= 20 ||
        (total_calls % 5000) == 0) {
      const char* file_str = "<null>";
      const char* name_str = "<null>";
      if (file_ptr && file_ptr < 0xF0000000) {
        auto* f = memory->TranslateVirtual<const char*>(file_ptr);
        if (f) file_str = f;
      }
      if (name_ptr && name_ptr < 0xF0000000) {
        auto* n = memory->TranslateVirtual<const char*>(name_ptr);
        if (n) name_str = n;
      }
      if (is_failure && result32 == 0xFFFFFFFF) {
        XELOGW(
            "DC3: MemOrPoolAlloc FAILURE! LR={:08X} size={} (0x{:X}) "
            "file={} line={} name={} -> {:08X} (raw64={:016X}) path={} "
            "(total={} failures={})",
            caller_lr, static_cast<int32_t>(size), size, file_str, line,
            name_str, result32, result,
            (static_cast<int32_t>(size) > 0x80) ? "MemAlloc" : "PoolAlloc",
            total_calls, failure_count);
        // Dump pool allocator state for PoolAlloc-path failures.
        if (static_cast<int32_t>(size) <= 0x80) {
          int fsa_idx = (static_cast<int>(size) - 1) >> 4;
          uint32_t g_chunk = 0;
          if (auto* gcp = memory->TranslateVirtual<uint8_t*>(kDc3GChunkAllocAddr)) {
            g_chunk = xe::load_and_swap<uint32_t>(gcp);
          }
          XELOGW("DC3: Pool diag: gChunkAlloc={:08X} fsa_idx={}", g_chunk, fsa_idx);
          if (g_chunk && g_chunk < 0xF0000000) {
            // ChunkAllocator::mAllocs is at offset 0, array of 64 pointers
            uint32_t fsa_ptr_addr = g_chunk + fsa_idx * 4;
            uint32_t fsa_ptr = 0;
            if (auto* p = memory->TranslateVirtual<uint8_t*>(fsa_ptr_addr)) {
              fsa_ptr = xe::load_and_swap<uint32_t>(p);
            }
            XELOGW("DC3: Pool diag: FixedSizeAlloc[{}]={:08X}", fsa_idx, fsa_ptr);
            if (fsa_ptr && fsa_ptr < 0xF0000000) {
              auto* fsa = memory->TranslateVirtual<uint8_t*>(fsa_ptr);
              if (fsa) {
                uint32_t alloc_words = xe::load_and_swap<uint32_t>(fsa + kFsaOffAllocSizeWords);
                uint32_t num_allocs = xe::load_and_swap<uint32_t>(fsa + kFsaOffNumAllocs);
                uint32_t max_allocs = xe::load_and_swap<uint32_t>(fsa + kFsaOffMaxAllocs);
                uint32_t num_chunks = xe::load_and_swap<uint32_t>(fsa + kFsaOffNumChunks);
                uint32_t free_list = xe::load_and_swap<uint32_t>(fsa + kFsaOffFreeList);
                uint32_t nodes_per = xe::load_and_swap<uint32_t>(fsa + kFsaOffNodesPerChunk);
                XELOGW(
                    "DC3: Pool diag: allocWords={} numAllocs={} maxAllocs={} "
                    "chunks={} freeList={:08X} nodesPerChunk={}",
                    alloc_words, num_allocs, max_allocs, num_chunks, free_list,
                    nodes_per);
                // Dump first few free list entries
                uint32_t fl = free_list;
                for (int i = 0; i < 5 && fl && fl < 0xF0000000; ++i) {
                  auto* fp = memory->TranslateVirtual<uint8_t*>(fl);
                  if (!fp) break;
                  uint32_t next = xe::load_and_swap<uint32_t>(fp);
                  XELOGW("DC3: Pool diag: freeList[{}]={:08X} -> next={:08X}",
                         i, fl, next);
                  fl = next;
                }
                if (fl >= 0xF0000000 && fl != 0) {
                  XELOGW("DC3: Pool diag: freeList entry {:08X} is INVALID "
                         "(possible corruption)",
                         fl);
                }
              }
            }
          }
        }
      } else if (is_failure && result32 == 0) {
        // size==0 returns null, that's normal
        if (size != 0) {
          XELOGW(
              "DC3: MemOrPoolAlloc returned NULL! LR={:08X} size={} (0x{:X}) "
              "file={} line={} name={} -> 0 (total={} failures={})",
              caller_lr, static_cast<int32_t>(size), size, file_str, line,
              name_str, total_calls, failure_count);
        }
      } else {
        XELOGI(
            "DC3: MemOrPoolAlloc LR={:08X} size={} (0x{:X}) file={} line={} "
            "name={} -> {:08X} (total={})",
            caller_lr, static_cast<int32_t>(size), size, file_str, line,
            name_str, result32, total_calls);
      }
    }
  };

  ctx.processor->RegisterGuestFunctionOverride(kDc3MemOrPoolAllocAddr, handler,
                                               "DC3:MemOrPoolAlloc(probe)");
  XELOGI("DC3: Registered MemOrPoolAlloc probe at {:08X}", kDc3MemOrPoolAllocAddr);
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
  if (mode != "log_only" && mode != "stub_on_fail" && mode != "null_on_fail" &&
      mode != "setupfont_fix") {
    XELOGW("DC3: Unknown FindArray override mode '{}'; expected off|log_only|"
           "stub_on_fail|null_on_fail|setupfont_fix. Leaving original behavior active.",
           mode);
    debug_result.skipped++;
    return;
  }
  if (mode == "setupfont_fix") {
    XELOGI("DC3: FindArray override behavior left original (mode=setupfont_fix); "
           "SetupFont repair is handled in SystemConfig2 probe override");
    debug_result.skipped++;
    return;
  }

  static uint32_t s_find_array_stub = 0;
  constexpr uint32_t kFindArrayAddr = 0x83540134;
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
    bool sym_bin = false;
    std::string sym_repr =
        Dc3DescribeGuestSymbol(memory, sym_addr, sym_str, sizeof(sym_str), &sym_bin);
    uint32_t found_addr = Dc3FindArrayLinearBySymbol(memory, da_addr, sym_addr);

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
    bool interesting_bin = sym_bin;
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

  constexpr uint32_t kMergedDataArrayNodeAddr = 0x8353EEB4;
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

void RegisterDc3SystemConfigProbe(const Dc3HackContext& ctx,
                                  Dc3HackApplyResult& debug_result) {
  if (!ctx.processor || !ctx.is_decomp_layout) {
    debug_result.skipped++;
    return;
  }
  const std::string mode = cvars::dc3_debug_findarray_override_mode;
  if (mode != "log_only" && mode != "setupfont_fix") {
    debug_result.skipped++;
    return;
  }

  auto handler = [](cpu::ppc::PPCContext* ppc_context,
                    kernel::KernelState* kernel_state) {
    if (!ppc_context || !kernel_state) return;
    auto* memory = kernel_state->memory();
    if (!memory) return;

    uint32_t s1 = static_cast<uint32_t>(ppc_context->r[3]);
    uint32_t s2 = static_cast<uint32_t>(ppc_context->r[4]);
    uint32_t caller_lr = static_cast<uint32_t>(ppc_context->lr);

    uint32_t g_system_config = 0;
    if (auto* p = memory->TranslateVirtual<uint8_t*>(kDc3GSystemConfigAddr)) {
      g_system_config = xe::load_and_swap<uint32_t>(p + 0x0);
    }
    uint32_t arr1 = Dc3FindArrayLinearBySymbol(memory, g_system_config, s1);
    uint32_t arr2 = Dc3FindArrayLinearBySymbol(memory, arr1, s2);

    char s1_buf[64] = {};
    char s2_buf[64] = {};
    bool s1_bin = false;
    bool s2_bin = false;
    std::string s1_repr =
        Dc3DescribeGuestSymbol(memory, s1, s1_buf, sizeof(s1_buf), &s1_bin);
    std::string s2_repr =
        Dc3DescribeGuestSymbol(memory, s2, s2_buf, sizeof(s2_buf), &s2_bin);
    const std::string mode_local = cvars::dc3_debug_findarray_override_mode;

    static uint32_t log_count = 0;
    ++log_count;
    bool interesting = (arr2 == 0) || s1_bin || s2_bin;
    if (interesting || log_count <= 100 || (log_count % 1000) == 0) {
      XELOGI(
          "DC3: SystemConfig2 probe mode={} LR={:08X} gSystemConfig={:08X} s1={:08X} "
          "({}) s2={:08X} ({}) -> a1={:08X} a2={:08X}",
          mode_local, caller_lr, g_system_config, s1, s1_repr, s2, s2_repr, arr1,
          arr2);
    }

    if (caller_lr == kDc3SetupFontSystemConfigReturnLR) {
      Dc3LogSetupFontLiteralSanity(memory);
    }

    if (mode_local == "setupfont_fix" &&
        caller_lr == kDc3SetupFontSystemConfigReturnLR && arr1 != 0 && arr2 == 0 &&
        !s1_bin && s1_repr == "rnd" && s2_bin) {
      uint32_t repaired =
          Dc3FindArrayLinearBySymbol(memory, arr1, kDc3PooledFontStringAddr);
      if (repaired) {
        XELOGW(
            "DC3: SetupFont SystemConfig repair applied: substituted arg2 "
            "'font' via pooled literal {:08X} (bad arg2 was {}) -> {:08X}",
            kDc3PooledFontStringAddr, s2_repr, repaired);
        arr2 = repaired;
      } else {
        XELOGW("DC3: SetupFont SystemConfig repair attempted but pooled 'font' "
               "lookup failed (arr1={:08X})",
               arr1);
      }
    }

    if (s2_bin) {
      static uint32_t bin_dump_count = 0;
      ++bin_dump_count;
      if (bin_dump_count <= 32 || (bin_dump_count % 256) == 0) {
        uint32_t g_string_table = 0;
        if (auto* p = memory->TranslateVirtual<uint8_t*>(kDc3GStringTableGlobalAddr)) {
          g_string_table = xe::load_and_swap<uint32_t>(p + 0x0);
        }

        uint32_t ht_entries = 0;
        uint32_t ht_size = 0;
        uint32_t ht_num_entries = 0;
        uint32_t ht_empty = 0;
        uint32_t ht_removed = 0;
        uint8_t ht_own = 0;
        if (auto* h = memory->TranslateVirtual<uint8_t*>(kDc3GHashTableAddr)) {
          ht_entries = xe::load_and_swap<uint32_t>(h + 0x0);
          ht_size = xe::load_and_swap<uint32_t>(h + 0x4);
          ht_own = *(h + 0x8);
          ht_num_entries = xe::load_and_swap<uint32_t>(h + 0xC);
          ht_empty = xe::load_and_swap<uint32_t>(h + 0x10);
          ht_removed = xe::load_and_swap<uint32_t>(h + 0x14);
        }
        XELOGW(
            "DC3: Symbol globals on binary SystemConfig key: gStringTable*={:08X} "
            "gHashTable(entries={:08X} size={} own={} used={} empty={:08X} "
            "removed={:08X})",
            g_string_table, ht_entries, ht_size, ht_own, ht_num_entries, ht_empty,
            ht_removed);

        if (g_string_table && g_string_table < 0xF0000000) {
          if (auto* st = memory->TranslateVirtual<uint8_t*>(g_string_table)) {
            uint32_t v_begin = xe::load_and_swap<uint32_t>(st + 0x0);
            uint32_t v_end = xe::load_and_swap<uint32_t>(st + 0x4);
            uint32_t v_cap = xe::load_and_swap<uint32_t>(st + 0x8);
            uint32_t cur_char = xe::load_and_swap<uint32_t>(st + 0xC);
            int32_t cur_buf = xe::load_and_swap<int32_t>(st + 0x10);
            XELOGW(
                "DC3: StringTable state begin={:08X} end={:08X} cap={:08X} "
                "mCurChar={:08X} mCurBuf={}",
                v_begin, v_end, v_cap, cur_char, cur_buf);
          }
        }
      }
    }

    ppc_context->r[3] = arr2;
  };

  ctx.processor->RegisterGuestFunctionOverride(kDc3SystemConfig2Addr, handler,
                                               "DC3:SystemConfig2(log)");
  XELOGI("DC3: Registered SystemConfig(Symbol,Symbol) probe at {:08X} "
         "(active in FindArray mode={})",
         kDc3SystemConfig2Addr, mode);
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
  const uint32_t kNotifyFuncs[] = {0x8311A828};
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

  // Make the full PE image + heap region writable.
  // The decomp XEX has global DataNode/DataArray objects in .data/.bss sections
  // that the game writes to at runtime.  The linker marks initialized-data
  // sections read-only, but the game expects them writable.
  // PE image extends to 0x83F60000 (SizeOfImage=0x1F53000, rounded to pages).
  {
    auto* heap1 = memory->LookupHeap(0x82000000);
    if (heap1) {
      heap1->Protect(0x82000000, 0x1F60000,
                     kMemoryProtectRead | kMemoryProtectWrite);
      XELOGI("DC3: Made PE image 0x82000000-0x83F60000 writable");
    }
    // Allocate + commit memory past PE image for MemMgr heap growth.
    // The PE image ends at ~0x83F60000 but MemMgr heaps (via malloc ->
    // NtAllocateVirtualMemory) grow into this region.  Protect() alone fails
    // because pages past the image aren't committed.  Use AllocFixed() to
    // reserve + commit the region, same pattern as xex_module.cc patching.
    constexpr uint32_t kHeapExtStart = 0x83F60000;
    constexpr uint32_t kHeapExtSize = 0x1000000;  // 16MB for heap growth
    auto* ext_heap = memory->LookupHeap(kHeapExtStart);
    if (ext_heap) {
      bool ext_ok = ext_heap->AllocFixed(
          kHeapExtStart, kHeapExtSize, 4096,
          kMemoryAllocationReserve | kMemoryAllocationCommit,
          kMemoryProtectRead | kMemoryProtectWrite);
      if (ext_ok) {
        XELOGI("DC3: Allocated {:08X}-{:08X} ({} MB) for heap growth",
               kHeapExtStart, kHeapExtStart + kHeapExtSize,
               kHeapExtSize / (1024 * 1024));
      } else {
        XELOGW("DC3: Failed to allocate heap extension at {:08X}",
               kHeapExtStart);
      }
    }
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
        // TODO: These assert-nop addresses are STALE after the relink.
        // With the heap iteration fix, these asserts may not fire. Recompute
        // from disassembly if needed: MemInit=0x82878530, _MemAllocTemp=0x82878158.
        const MemPatch mem_patches[] = {
            {0x83447AF4, 0x481032B9, "MemInit line 690: Debug::Fail call (STALE)"},
            {0x83446A24, 0x48104389, "MemAlloc line 961: Debug::Fail call (STALE)"},
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

    // Initialize STLport std::list<bool> gConditional sentinel.
    // TODO: gConditional address is STALE after relink. The std::list<bool>
    // static initializer should handle this if CRT init runs correctly.
    {
      constexpr uint32_t kGConditionalAddr = 0x83C7D354;  // STALE
      auto* pcond = memory->TranslateVirtual<uint8_t*>(kGConditionalAddr);
      if (pcond) {
        xe::store_and_swap<uint32_t>(pcond + 0, kGConditionalAddr); // _M_next
        xe::store_and_swap<uint32_t>(pcond + 4, kGConditionalAddr); // _M_prev
        XELOGI("DC3: Initialized gConditional sentinel at {:08X}", kGConditionalAddr);
        stopgap_result.applied++;
      }
    }
  }

  // Redirect DECOMP Debug::Print to XELOG so MILO_LOG output remains visible
  // without stubbing the CRT formatter path.
  if (ctx.processor) {
    constexpr uint32_t kDebugPrint = 0x835466D4;
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
    constexpr uint32_t kDebugFail = 0x83547ABC;
    auto fail_handler = [](cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      // Re-entrancy guard: Fail formatting can trigger MILO_ASSERT → Fail recursion
      // (e.g. TextStream::operator<<(NULL) → assert → Fail → format → ...).
      static bool s_in_fail = false;
      if (s_in_fail) {
        ppc_context->r[3] = 0;
        return;
      }
      s_in_fail = true;
      auto* memory = kernel_state->memory();
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t r3 = static_cast<uint32_t>(ppc_context->r[3]);  // Debug*
      uint32_t r4 = static_cast<uint32_t>(ppc_context->r[4]);  // msg
      static int fail_count = 0;
      fail_count++;
      XELOGE("DC3: Debug::Fail called! LR={:08X} r3={:08X} r4={:08X}", lr, r3, r4);
      if (memory) {
        static bool logged_debug_obj = false;
        auto dump_factory_state = [&](const char* reason) {
          uint8_t hdr[0x20] = {};
          if (auto* map_mem =
                  memory->TranslateVirtual<uint8_t*>(kDc3ObjectFactoriesMapAddr)) {
            std::memcpy(hdr, map_mem, sizeof(hdr));
            XELOGW("DC3: Hmx::Object::sFactories [{}] @{:08X} hdr={}",
                   reason, kDc3ObjectFactoriesMapAddr,
                   Dc3FmtBytePreview(hdr, sizeof(hdr)));
          } else {
            XELOGW("DC3: Hmx::Object::sFactories [{}] @{:08X} <unmapped>", reason,
                   kDc3ObjectFactoriesMapAddr);
          }
          auto dump_static_symbol = [&](const char* label, uint32_t sym_slot_addr) {
            if (auto* p = memory->TranslateVirtual<uint8_t*>(sym_slot_addr)) {
              uint32_t sym_ptr = xe::load_and_swap<uint32_t>(p + 0x0);
              bool sym_bin = false;
              char tmp[64] = {};
              std::string desc = Dc3DescribeGuestSymbol(memory, sym_ptr, tmp, sizeof(tmp),
                                                        &sym_bin);
              XELOGW("DC3: {} static Symbol slot @{:08X} -> {:08X} ({})", label,
                     sym_slot_addr, sym_ptr, desc);
            } else {
              XELOGW("DC3: {} static Symbol slot @{:08X} <unmapped>", label,
                     sym_slot_addr);
            }
          };
          dump_static_symbol("RndMat::StaticClassName", kDc3RndMatStaticNameSymAddr);
          dump_static_symbol("MetaMaterial::StaticClassName",
                             kDc3MetaMaterialStaticNameSymAddr);
        };
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
            if (std::strncmp(msg, "Unknown class ", 14) == 0 ||
                std::strncmp(msg, "Couldn't instantiate class ", 26) == 0) {
              static uint32_t factory_dump_count = 0;
              if (factory_dump_count < 16) {
                ++factory_dump_count;
                dump_factory_state(msg);
              }
            }
          }
        }
      }
      if (fail_count <= 5 || (fail_count % 5000) == 0) {
        XELOGW("DC3: Debug::Fail returning to caller (count={})", fail_count);
      }
      s_in_fail = false;
      ppc_context->r[3] = 0;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kDebugFail, fail_handler, "DC3:Debug::Fail(log)");
    XELOGI("DC3: Registered Debug::Fail handler at {:08X} (logs LR + hangs)",
           kDebugFail);
  }

  // Break recursive crash in Debug::DoCrucible.
  // When an assertion fires (e.g. StringTable not yet initialized), DoCrucible
  // formats error text which creates Symbol objects.  Symbol::Symbol calls
  // StringTable::Add, which asserts again → DoCrucible → Symbol → ... until
  // the guest stack overflows.  Add a re-entrancy guard that returns
  // immediately on recursive calls, breaking the infinite loop.
  if (ctx.processor) {
    constexpr uint32_t kDebugDoCrucible = 0x83546F00;
    auto crucible_handler = [](cpu::ppc::PPCContext* ppc_context,
                               kernel::KernelState* kernel_state) {
      static bool s_in_crucible = false;
      static int s_crucible_count = 0;
      s_crucible_count++;
      if (s_in_crucible) {
        // Recursive entry — break the loop silently.
        ppc_context->r[3] = 0;
        return;
      }
      s_in_crucible = true;
      if (s_crucible_count <= 5 || (s_crucible_count % 5000) == 0) {
        uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
        uint32_t r4 = static_cast<uint32_t>(ppc_context->r[4]);  // msg
        auto* memory = kernel_state ? kernel_state->memory() : nullptr;
        if (memory && r4 && r4 < 0xF0000000) {
          if (auto* msg = memory->TranslateVirtual<const char*>(r4)) {
            XELOGW("DC3: DoCrucible[{}] LR={:08X} msg={}", s_crucible_count,
                   lr, msg);
          }
        } else {
          XELOGW("DC3: DoCrucible[{}] LR={:08X} r4={:08X}", s_crucible_count,
                 lr, r4);
        }
      }
      s_in_crucible = false;
      ppc_context->r[3] = 0;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kDebugDoCrucible, crucible_handler, "DC3:Debug::DoCrucible(guard)");
    XELOGI("DC3: Registered DoCrucible re-entrancy guard at {:08X}",
           kDebugDoCrucible);
  }

  // TextStream::operator<<(const char*) NULL safety.
  // The decomp's MILO_ASSERT(c, 0x52) fires Debug::Fail when passed NULL,
  // and since our Fail handler returns, the code falls through to Print(NULL)
  // which the caller re-enters in a tight loop.  Override the function to
  // silently skip NULL and dispatch Print(c) for non-NULL via the vtable.
  if (ctx.processor && ctx.is_decomp_layout) {
    constexpr uint32_t kTextStreamOpConstChar = 0x829A7240;
    auto ts_handler = [](cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
      uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t c_ptr = static_cast<uint32_t>(ppc_context->r[4]);
      if (!c_ptr) {
        static int null_count = 0;
        if (++null_count <= 5 || (null_count % 10000) == 0) {
          uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
          XELOGW("DC3: TextStream::operator<<(const char*) NULL skip "
                 "(count={} LR={:08X})",
                 null_count, lr);
        }
        ppc_context->r[3] = this_ptr;
        return;
      }
      // Non-NULL: dispatch Print(c) through the vtable (slot 1).
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      if (!memory) {
        ppc_context->r[3] = this_ptr;
        return;
      }
      auto* obj = memory->TranslateVirtual<uint8_t*>(this_ptr);
      if (!obj) {
        ppc_context->r[3] = this_ptr;
        return;
      }
      uint32_t vtbl_addr = xe::load_and_swap<uint32_t>(obj + 0x00);
      auto* vtbl = memory->TranslateVirtual<uint8_t*>(vtbl_addr);
      if (!vtbl) {
        ppc_context->r[3] = this_ptr;
        return;
      }
      uint32_t print_addr = xe::load_and_swap<uint32_t>(vtbl + 0x04);
      auto* processor = ppc_context->processor;
      auto* thread_state = ppc_context->thread_state;
      uint64_t args[] = {this_ptr, c_ptr};
      processor->Execute(thread_state, print_addr, args, 2);
      ppc_context->r[3] = this_ptr;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kTextStreamOpConstChar, ts_handler,
        "DC3:TextStream::operator<<(const char*)(null-safe)");
    XELOGI("DC3: Registered TextStream::operator<<(const char*) NULL safety "
           "at {:08X}",
           kTextStreamOpConstChar);
  }

  // Lazy Symbol::PreInit via StringTable::Add override.
  // gStringTable is NULL because Symbol::PreInit's init_seg(lib) function
  // pointer was placed outside __xc_a..__xc_z by /FORCE linker.  Previous
  // attempts to inject a PPC trampoline into __xc[0] failed (JIT stale cache,
  // guest overrides not firing for bctrl, etc).
  //
  // StringTable::Add IS called via bl (direct call from Symbol::Symbol),
  // so guest function overrides DO fire here.  We intercept the first call,
  // call Symbol::PreInit to initialize gStringTable, then clear our override
  // so subsequent calls go to the real PPC implementation.
  //
  // Forwarding strategy: after calling PreInit, we clear the extern_handler_
  // on the GuestFunction object (via SetupExtern(nullptr)).  This makes
  // the Call() path fall through to CallImpl() which invokes the JIT-compiled
  // PPC code (DemandFunction compiles even extern-behavior functions).
  // We then re-Execute at the same address, which now hits the JIT path.
  if (ctx.processor && ctx.is_decomp_layout) {
    constexpr uint32_t kStringTableAdd = 0x82924848;
    auto st_add_handler = [](cpu::ppc::PPCContext* ppc_context,
                             kernel::KernelState* kernel_state) {
      static bool s_preinit_done = false;
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      auto* processor = ppc_context->processor;
      auto* thread_state = ppc_context->thread_state;

      if (!s_preinit_done && memory) {
        auto* gst_ptr = memory->TranslateVirtual<uint8_t*>(0x83AE0190);
        uint32_t gst_val = gst_ptr ? xe::load_and_swap<uint32_t>(gst_ptr) : 0;
        uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
        uint32_t this_arg = static_cast<uint32_t>(ppc_context->r[3]);
        uint32_t str_arg = static_cast<uint32_t>(ppc_context->r[4]);
        XELOGI("DC3: StringTable::Add first call: gStringTable={:08X} "
               "this={:08X} str={:08X} LR={:08X}",
               gst_val, this_arg, str_arg, lr);
        if (gst_val == 0) {
          XELOGI("DC3: gStringTable=NULL, calling Symbol::PreInit(560000, 80000)");
          uint64_t preinit_args[] = {560000, 80000};
          processor->Execute(thread_state, 0x82556E70, preinit_args, 2);
          gst_val = gst_ptr ? xe::load_and_swap<uint32_t>(gst_ptr) : 0;
          XELOGI("DC3: After PreInit: gStringTable={:08X}", gst_val);
        }
        s_preinit_done = true;

        // Clear our override so future calls go directly to PPC code.
        // SetupExtern(nullptr) sets extern_handler_=nullptr; the Call() path
        // then falls through to CallImpl() which runs JIT-compiled PPC code.
        // DemandFunction already compiled the PPC code even though we set
        // behavior to kExtern.
        auto* fn = processor->LookupFunction(0x82924848);
        if (fn && fn->is_guest()) {
          auto* gfn = static_cast<cpu::GuestFunction*>(fn);
          XELOGI("DC3: StringTable::Add fn={} behavior={} has_extern={}",
                 (void*)gfn, (int)gfn->behavior(),
                 gfn->extern_handler() != nullptr);
          gfn->SetupExtern(nullptr, nullptr);
          XELOGI("DC3: After SetupExtern(nullptr): behavior={} has_extern={} "
                 "status={} is_guest={}",
                 (int)gfn->behavior(),
                 gfn->extern_handler() != nullptr,
                 (int)gfn->status(),
                 gfn->is_guest());
        }
      }

      // Forward this call to the real PPC implementation.
      // Since we just cleared extern_handler_, Execute will use CallImpl.
      uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t str_ptr = static_cast<uint32_t>(ppc_context->r[4]);
      static int fwd_count = 0;
      if (++fwd_count <= 5) {
        XELOGI("DC3: StringTable::Add forwarding[{}]: this={:08X} str={:08X}",
               fwd_count, this_ptr, str_ptr);
      }
      uint64_t args[] = {this_ptr, str_ptr};
      processor->Execute(thread_state, 0x82924848, args, 2);
      ppc_context->r[3] = thread_state->context()->r[3];
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kStringTableAdd, st_add_handler,
        "DC3:StringTable::Add(lazy-PreInit)");
    XELOGI("DC3: Registered StringTable::Add lazy-PreInit override at {:08X}",
           kStringTableAdd);
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
    const uint32_t kFileIsLocalAssertBranch = 0x82B176F0;  // STALE - needs disasm of FileIsLocal@0x82B16AD8
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
        {0x83658ECC, "XMPOverrideBackgroundMusic"},
        {0x83658FA4, "XMPRestoreBackgroundMusic"},
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
  RegisterDc3SystemConfigProbe(ctx, debug_result);
  RegisterDc3MemOrPoolAllocProbe(ctx, debug_result);

  // Debug/Holmes/String-related stubs and deadlock breakers.
  {
    struct DebugFunc {
      uint32_t address;
      const char* name;
      uint32_t return_value;
    };
    DebugFunc debug_funcs[] = {
        {0x8393AE6C, "XGetLocale", 0},
        {0x8393B074, "XTLGetLanguage", 1},
        {0x8393B114, "DebugBreak", 0},
        {0x825522F0, "DataNode::Print", 0},
        {0x831F1678, "HolmesClientInit", 0},
        {0x831F1948, "HolmesClientReInit", 0},
        {0x831F19C8, "HolmesClientPoll", 0},
        {0x831F1248, "HolmesClientPollInternal", 0},
        {0x831F1360, "HolmesClientInitOpcode", 0},
        {0x831F0804, "HolmesClientTerminate", 0},
        {0x831EFB74, "CanUseHolmes", 0},
        {0x831EFBE8, "UsingHolmes", 0},
        {0x831EFA44, "ProtocolDebugString", 0},
        {0x831EFC04, "HolmesSetFileShare", 0},
        {0x831EFC5C, "HolmesFileHostName", 0},
        {0x831EFC68, "HolmesFileShare", 0},
        {0x831EFC74, "HolmesResolveIP", 0},
        {0x831F00D0, "BeginCmd", 0},
        {0x831F0120, "CheckForResponse", 0},
        {0x831F0324, "WaitForAnyResponse", 0},
        {0x831F0F54, "EndCmd", 0},
        {0x831F105C, "CheckReads", 0},
        {0x831F1140, "CheckInput", 0},
        {0x831F11E0, "WaitForResponse", 0},
        {0x831F2A2C, "WaitForReads", 0},
        {0x831F12A8, "HolmesClientPollKeyboard", 0},
        {0x831F1300, "HolmesClientPollJoypad", 0},
        {0x831F1E2C, "HolmesClientOpen", 0},
        {0x831F2314, "HolmesClientRead", 0},
        {0x831F244C, "HolmesClientReadDone", 0},
        {0x831F2068, "HolmesClientWrite", 0},
        {0x831F21D8, "HolmesClientTruncate", 0},
        {0x831F2AC8, "HolmesClientClose", 0},
        {0x831F1B34, "HolmesClientGetStat", 0},
        {0x831F1A40, "HolmesClientSysExec", 0},
        {0x831F1C4C, "HolmesClientMkDir", 0},
        {0x831F1D3C, "HolmesClientDelete", 0},
        {0x831F2C50, "HolmesClientEnumerate", 0},
        {0x831F2504, "HolmesClientCacheFile", 0},
        {0x831F26D4, "HolmesClientCacheResource", 0},
        {0x831F04B0, "HolmesToLocal", 0},
        {0x831F05A0, "HolmesFlushStreamBuffer", 0},
        {0x831F0634, "DumpHolmesLog", 0},
        {0x831F2808, "HolmesClientStackTrace", 0},
        {0x831F2938, "HolmesClientSendMessage", 0},
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
    const uint32_t kStringOpPlusEq = 0x82A5B268;
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
    const uint32_t kErrnoAddr = 0x83611760;
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
  // __pInvalidArgHandler function pointer (at 0x83CB912C in this build).
  // When it's NULL (CRT not fully initialized), it traps with tw 0x16
  // (EINVAL) which crashes the JIT host. Stub it to just return.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, 0x8361817C, 0,
                   "_invalid_parameter_noinfo (CRT trap stopgap)")) {
      stopgap_result.applied++;
    } else {
      stopgap_result.skipped++;
    }
    // Also stub other CRT error paths that contain trap instructions.
    if (PatchStub8(memory, 0x836181B0, 0,
                   "_call_reportfault (CRT trap stopgap)")) {
      stopgap_result.applied++;
    }
    if (PatchStub8(memory, 0x8360DA24, 0,
                   "_amsg_exit (CRT trap stopgap)")) {
      stopgap_result.applied++;
    }
    if (PatchStub8(memory, 0x8361AFD4, 0,
                   "__report_gsfailure (CRT trap stopgap)")) {
      stopgap_result.applied++;
    }
  }

  // Decomp-only formatting guard: in the current decomp build, TaskMgr ctor
  // -> FormatString::operator<< -> Hx_snprintf enters _vsnprintf_l invalid-
  // parameter recursion. Patch the _vsnprintf_l call inside Hx_snprintf so
  // Hx_snprintf's own error path still runs (it null-terminates and returns
  // -1) instead of bypassing local cleanup at the higher-level callsite.
  if (ctx.is_decomp_layout) {
    const uint32_t kHxSnprintfVsnprintfCall = 0x83477FBC;  // STALE - needs disasm of Hx_snprintf@0x83513264
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
    const uint32_t kTextStart = 0x82320000;
    const uint32_t kTextSize = 0x017336D4;   // .text through .xidata end
    const uint32_t kIdataStart = 0x8230D800;
    const uint32_t kIdataEnd = 0x8230DE34;
    const uint32_t kThunkAreaStart = 0x83A52000;  // .xidata region within .text
    const uint32_t kThunkAreaEnd = 0x83A54000;    // past .xidata end

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
    constexpr uint32_t kXcA = 0x83ADED60;
    constexpr uint32_t kXcZ = 0x83ADF378;
    constexpr uint32_t kXiA = 0x83ADF37C;
    constexpr uint32_t kXiZ = 0x83ADF388;
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

    // Inject _ioinit into the __xi_a slot.  With /FORCE linking, the _ioinit
    // function pointer ended up at VA 0x83ADED58 — outside __xi_a..__xi_z
    // (0x83ADF37C..0x83ADF388).  _cinit iterates only the sentinel-bounded
    // range, so it never calls _ioinit, leaving __pioinfo uninitialized.
    // This causes _output_l (CRT printf core) to infinite-loop when it tries
    // to lock via __pioinfo[].
    {
      constexpr uint32_t kIoinitAddr = 0x8361ADDC;
      auto* xi_slot = memory->TranslateVirtual<uint8_t*>(kXiA);
      if (xi_slot) {
        xe::store_and_swap<uint32_t>(xi_slot, kIoinitAddr);
        XELOGI("DC3: Injected _ioinit ({:08X}) into __xi_a slot at {:08X}",
               kIoinitAddr, kXiA);
        crt_result.applied++;
      }
    }

    // Inject Symbol::PreInit into __xc[0] so gStringTable is initialized
    // before any C++ global constructors that create Symbol objects.
    // Without this, Symbol::Symbol dereferences gStringTable (NULL) which
    // reads from the zero-page (mapped as zeros) giving mCurBuf=0 instead
    // of -1, causing StringTable::Add to loop in "Wasted string table" spam.
    if (ctx.processor) {
      auto* xc_first = memory->TranslateVirtual<uint8_t*>(kXcA);
      uint32_t first_entry = xc_first ? xe::load_and_swap<uint32_t>(xc_first) : 1;
      if (first_entry == 0) {
        // Place PPC trampoline inside the CODE section by overwriting
        // ProtocolDebugString (already PatchStub8'd, 304 bytes available).
        // Guest function overrides do NOT fire for indirect calls (bctrl
        // from _cinit), so we must write real PPC code.  The JIT compiles
        // lazily, so overwriting guest memory before any guest code
        // executes ensures our trampoline is what gets compiled.
        constexpr uint32_t kTrampAddr = 0x831EFA44;  // ProtocolDebugString (CODE section)
        auto* heap = memory->LookupHeap(kTrampAddr);
        auto* t = memory->TranslateVirtual<uint8_t*>(kTrampAddr);
        if (heap && t) {
          heap->Protect(kTrampAddr, 80, kMemoryProtectRead | kMemoryProtectWrite);
          // Symbol::PreInit = 0x82556E70
          uint32_t code[] = {
            // Prologue
            0x7C0802A6,  // mflr r0
            0x90010004,  // stw r0, 4(r1)
            0x9421FFF0,  // stwu r1, -16(r1)
            // Check gStringTable at 0x83AE0190
            0x3C6083AE,  // lis r3, 0x83AE
            0x80630190,  // lwz r3, 0x0190(r3)
            0x2C030000,  // cmpwi r3, 0
            0x40820024,  // bne +36 (skip to epilogue at li r3,0)
            // Call Symbol::PreInit(560000=0x88B80, 80000=0x13880)
            0x3C600008,  // lis r3, 0x0008
            0x60638B80,  // ori r3, r3, 0x8B80
            0x3C800001,  // lis r4, 0x0001
            0x60843880,  // ori r4, r4, 0x3880
            0x3D808255,  // lis r12, 0x8255
            0x618C6E70,  // ori r12, r12, 0x6E70
            0x7D8903A6,  // mtctr r12
            0x4E800421,  // bctrl
            // Epilogue (bne lands here)
            0x38600000,  // li r3, 0 (safe return value)
            0x38210010,  // addi r1, r1, 16
            0x80010004,  // lwz r0, 4(r1)
            0x7C0803A6,  // mtlr r0
            0x4E800020,  // blr
          };
          for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); ++i) {
            xe::store_and_swap<uint32_t>(t + i * 4, code[i]);
          }
          xe::store_and_swap<uint32_t>(xc_first, kTrampAddr);
          XELOGI("DC3: Wrote Symbol::PreInit PPC trampoline ({} insns) "
                 "at {:08X} (CODE section), injected into __xc[0] ({:08X})",
                 sizeof(code) / sizeof(code[0]), kTrampAddr, kXcA);
          // Verify: read back __xc[0] and first trampoline instruction
          uint32_t xc0_readback = xe::load_and_swap<uint32_t>(xc_first);
          uint32_t tramp_w0 = xe::load_and_swap<uint32_t>(t);
          uint32_t tramp_w1 = xe::load_and_swap<uint32_t>(t + 4);
          XELOGI("DC3: Verify: __xc[0]={:08X} tramp[0]={:08X} tramp[1]={:08X}",
                 xc0_readback, tramp_w0, tramp_w1);
          crt_result.applied++;
        }
      } else {
        XELOGW("DC3: __xc[0] not null ({:08X}), cannot inject PreInit trampoline",
               first_entry);
      }
    }

    // Diagnostic: dump first 16 instructions of _cinit to understand
    // how it iterates __xc and __xi tables.
    {
      constexpr uint32_t kCinit = 0x8311A6D4;
      auto* ci = memory->TranslateVirtual<uint8_t*>(kCinit);
      if (ci) {
        XELOGI("DC3: _cinit PPC instructions at {:08X}:", kCinit);
        for (int i = 0; i < 60; ++i) {
          uint32_t insn = xe::load_and_swap<uint32_t>(ci + i * 4);
          XELOGI("DC3:   {:08X}: {:08X}", kCinit + i * 4, insn);
        }
      }
    }
  }

  // Diagnostics: check JIT indirection table state for import thunk area.
  {
    auto* code_cache = reinterpret_cast<uint8_t*>(0x80000000);
    uint32_t check_addrs[] = {
        0x82EEB600,  // BINK start (past .text)
        0x82EE7160,  // .text end
        0x82330000,  // .text start
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
