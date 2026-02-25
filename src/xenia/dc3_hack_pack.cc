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

// ============================================================================
// Address table + globals
// ============================================================================

// Guest addresses from build/373307D9/default.map.
// Last refreshed: 2026-02-25.
// STALE(date) = not verified against current MAP.
struct Dc3Addresses {
  // CRT sentinels
  uint32_t xc_a = 0x83ADED60;
  uint32_t xc_z = 0x83ADF378;
  uint32_t xi_a = 0x83ADF37C;
  uint32_t xi_z = 0x83ADF388;
  // CRT functions
  uint32_t ioinit = 0x8361ADDC;
  uint32_t cinit = 0x8311A6D4;
  uint32_t errno_fn = 0x83611760;
  uint32_t invalid_parameter_noinfo = 0x8361817C;
  uint32_t call_reportfault = 0x836181B0;
  uint32_t amsg_exit = 0x8360DA24;
  uint32_t report_gsfailure = 0x8361AFD4;
  // CRT formatter
  uint32_t output_l = 0x836192F0;
  uint32_t woutput_l = 0x8361EE64;
  uint32_t hx_snprintf_vsnprintf_call = 0x83477FBC;  // STALE
  // Debug subsystem
  uint32_t debug_print = 0x835466D4;
  uint32_t debug_fail = 0x83547ABC;
  uint32_t debug_do_crucible = 0x83546F00;
  uint32_t datanode_print = 0x825522F0;
  // Import/thunk
  uint32_t xapi_call_thread_notify = 0x8311A828;
  uint32_t text_start = 0x82320000;
  uint32_t text_size = 0x017336D4;
  uint32_t idata_start = 0x8230D800;
  uint32_t idata_end = 0x8230DE34;
  uint32_t thunk_area_start = 0x83A52000;
  uint32_t thunk_area_end = 0x83A54000;
  // Locale
  uint32_t get_system_language = 0x827F808C;
  uint32_t get_system_locale = 0x827F8774;
  uint32_t xget_locale = 0x8393AE6C;
  uint32_t xtl_get_language = 0x8393B074;
  uint32_t debug_break = 0x8393B114;
  // ReadCacheStream probes
  uint32_t rcs_read_cache_stream = 0x83115974;
  uint32_t rcs_bufstream_read_impl = 0x82BC2E90;
  uint32_t rcs_bufstream_seek_impl = 0x82BC2F90;
  // SystemConfig / FindArray / SetupFont
  uint32_t system_config_2 = 0x8351340C;
  uint32_t find_array = 0x83540134;
  uint32_t setup_font_syscfg_return_lr = 0x8317FF14;       // STALE
  uint32_t setup_font_ctor1_literal = 0x82027684;          // STALE
  uint32_t setup_font_ctor2_literal = 0x82053BF8;          // STALE
  uint32_t pooled_font_string = 0x82017684;                // STALE
  uint32_t setup_font_node_source_lr = 0x8317FF40;
  uint32_t setup_font_node_dest_lr = 0x8318001C;
  // Object / factory globals
  uint32_t object_factories_map = 0x83AE1E00;
  uint32_t rndmat_static_name_sym = 0x83AEAF2C;
  uint32_t metamaterial_static_name_sym = 0x83AEC3A8;
  uint32_t g_system_config = 0x83C7B2E0;
  uint32_t g_string_table_global = 0x83AE0190;
  uint32_t g_hash_table = 0x83AED4FC;
  // Memory / allocator probes
  uint32_t mem_or_pool_alloc = 0x82877950;
  uint32_t mem_alloc = 0x82878158;
  uint32_t pool_alloc = 0x835E61D4;
  uint32_t string_reserve = 0x82A5B1D0;
  uint32_t string_reserve_memalloc_ret_lr = 0x82A5BC00;    // STALE
  uint32_t g_chunk_alloc = 0x83CB8500;
  // DataArray / DataNode
  uint32_t merged_dataarray_node = 0x8353EEB4;
  uint32_t string_table_add = 0x82924848;
  uint32_t symbol_preinit = 0x82556E70;
  // TextStream
  uint32_t textstream_op_const_char = 0x829A7240;
  // String ops
  uint32_t string_op_plus_eq = 0x82A5B268;
  // XMP
  uint32_t xmp_override_bg_music = 0x83658ECC;
  uint32_t xmp_restore_bg_music = 0x83658FA4;
  // Write bridges
  uint32_t write_nolock = 0x83614508;
  uint32_t write_fn = 0x83614748;
  // FileIsLocal
  uint32_t file_is_local_assert_branch = 0x82B176F0;       // STALE
  // gConditional sentinel
  uint32_t g_conditional = 0x83C7D354;                     // STALE
  // Holmes trampoline target (reused as PPC code cave)
  uint32_t protocol_debug_string = 0x831EFA44;
  // MemMgr assert addresses (STALE)
  uint32_t meminit_assert = 0x83447AF4;                    // STALE
  uint32_t memalloc_assert = 0x83446A24;                   // STALE
};
constexpr Dc3Addresses kAddr;

// FixedSizeAlloc layout offsets (stable across builds).
constexpr uint32_t kFsaOffAllocSizeWords = 0x04;
constexpr uint32_t kFsaOffNumAllocs = 0x08;
constexpr uint32_t kFsaOffMaxAllocs = 0x0C;
constexpr uint32_t kFsaOffNumChunks = 0x10;
constexpr uint32_t kFsaOffFreeList = 0x14;
constexpr uint32_t kFsaOffNodesPerChunk = 0x18;

// I/O and buffer constants.
constexpr uint32_t kIoStrgFlag = 0x0040;
constexpr uint32_t kTempNarrowBufSize = 4096;
constexpr uint32_t kTempWideBufChars = 2048;

// Mutable global state.
static uint32_t g_dc3_errno_guest_ptr = 0;
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

  auto* l1 = memory->TranslateVirtual<const char*>(kAddr.setup_font_ctor1_literal);
  auto* l2 = memory->TranslateVirtual<const char*>(kAddr.setup_font_ctor2_literal);
  if (!l1 || !l2) {
    XELOGW("DC3: SetupFont literal sanity check could not map literals "
           "ctor1@{:08X}={} ctor2@{:08X}={}",
           kAddr.setup_font_ctor1_literal, static_cast<const void*>(l1),
           kAddr.setup_font_ctor2_literal, static_cast<const void*>(l2));
    return;
  }

  bool l1_bin = false;
  bool l2_bin = false;
  char tmp1[32] = {};
  char tmp2[32] = {};
  std::string l1_desc =
      Dc3DescribeGuestSymbol(memory, kAddr.setup_font_ctor1_literal, tmp1, sizeof(tmp1),
                             &l1_bin);
  std::string l2_desc =
      Dc3DescribeGuestSymbol(memory, kAddr.setup_font_ctor2_literal, tmp2, sizeof(tmp2),
                             &l2_bin);

  XELOGI(
      "DC3: SetupFont literal sanity ctor1/arg2@{:08X}={} ctor2/arg1@{:08X}={}",
      kAddr.setup_font_ctor1_literal, l1_desc, kAddr.setup_font_ctor2_literal, l2_desc);

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
      lr == kAddr.rcs_read_cache_stream) {
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
  bool is_string_stream = (flags & kIoStrgFlag) != 0;

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
                             kTempNarrowBufSize, "_output_l");
  if (!scratch) {
    ppc_context->r[3] = 0;
    return;
  }
  cpu::ppc::PPCContext tmp = *ppc_context;
  tmp.r[3] = scratch;
  tmp.r[4] = kTempNarrowBufSize - 1;
  tmp.r[5] = format_ptr;
  tmp.r[6] = arg_ptr;
  kernel::xboxkrnl::_vsnprintf_entry(&tmp, kernel_state);
  int32_t count = static_cast<int32_t>(tmp.r[3]);

  auto* buf = memory->TranslateVirtual<char*>(scratch);
  if (buf) {
    int32_t emit = count;
    if (emit < 0) {
      emit = static_cast<int32_t>(::strnlen(buf, kTempNarrowBufSize - 1));
    } else if (emit > static_cast<int32_t>(kTempNarrowBufSize - 1)) {
      emit = kTempNarrowBufSize - 1;
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
  bool is_string_stream = (flags & kIoStrgFlag) != 0;

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
                                            kTempWideBufChars * 2, "_woutput_l");
  if (!scratch) {
    ppc_context->r[3] = 0;
    return;
  }
  cpu::ppc::PPCContext tmp = *ppc_context;
  tmp.r[3] = scratch;
  tmp.r[4] = kTempWideBufChars - 1;
  tmp.r[5] = format_ptr;
  tmp.r[6] = arg_ptr;
  kernel::xboxkrnl::_vsnwprintf_entry(&tmp, kernel_state);
  int32_t count = static_cast<int32_t>(tmp.r[3]);
  auto* wbuf = memory->TranslateVirtual<xe::be<uint16_t>*>(scratch);
  if (wbuf) {
    int32_t emit = count;
    if (emit < 0) emit = 0;
    emit = std::min<int32_t>(emit, kTempWideBufChars - 1);
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
      {kAddr.write_nolock, "_write_nolock"},
      {kAddr.write_fn, "_write"},
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
      {kAddr.get_system_language, "GetSystemLanguage"},
      {kAddr.get_system_locale, "GetSystemLocale"},
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
    if (it != ctx.hack_pack_stubs->end() && it->second != kAddr.output_l) {
      XELOGW("DC3: Ignoring manifest _output_l remap {:08X} -> {:08X}; using "
             "map-synced CRT address",
             kAddr.output_l, it->second);
    }
    it = ctx.hack_pack_stubs->find("_woutput_l");
    if (it != ctx.hack_pack_stubs->end() && it->second != kAddr.woutput_l) {
      XELOGW("DC3: Ignoring manifest _woutput_l remap {:08X} -> {:08X}; using "
             "map-synced CRT address",
             kAddr.woutput_l, it->second);
    }
  }
  const uint32_t output_l_addr = kAddr.output_l;
  const uint32_t woutput_l_addr = kAddr.woutput_l;

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
        result = processor->Execute(thread_state, kAddr.mem_alloc, args, 5);
      } else {
        // PoolAlloc(int size, int pool_size, const char* file, int line,
        //           const char* name)
        uint64_t args[5] = {size, size, file_ptr, line, name_ptr};
        result = processor->Execute(thread_state, kAddr.pool_alloc, args, 5);
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
    bool is_string_reserve_caller = (caller_lr == kAddr.string_reserve_memalloc_ret_lr);

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
          if (auto* gcp = memory->TranslateVirtual<uint8_t*>(kAddr.g_chunk_alloc)) {
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

  ctx.processor->RegisterGuestFunctionOverride(kAddr.mem_or_pool_alloc, handler,
                                               "DC3:MemOrPoolAlloc(probe)");
  XELOGI("DC3: Registered MemOrPoolAlloc probe at {:08X}", kAddr.mem_or_pool_alloc);
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

  ctx.processor->RegisterGuestFunctionOverride(kAddr.find_array, find_array_handler,
                                               "DC3:FindArray");
  XELOGI("DC3: Registered FindArray(Symbol,bool) override at {:08X} (mode={})",
         kAddr.find_array, mode);
  debug_result.applied++;
}

void RegisterDc3ReadCacheStreamProbe(const Dc3HackContext& ctx,
                                     Dc3HackApplyResult& debug_result) {
  if (!ctx.processor) {
    debug_result.skipped++;
    return;
  }

  ctx.processor->RegisterGuestFunctionOverride(kAddr.rcs_bufstream_read_impl,
                                               Dc3RcsBufStreamReadImplProbe,
                                               "DC3:RCS:BufStream::ReadImpl");
  ctx.processor->RegisterGuestFunctionOverride(kAddr.rcs_bufstream_seek_impl,
                                               Dc3RcsBufStreamSeekImplProbe,
                                               "DC3:RCS:BufStream::SeekImpl");
  XELOGW("DC3: ReadCacheStream invasive probe active via BufStream overrides "
         "(ReadImpl={:08X}, SeekImpl={:08X}, ReadCacheStream={:08X}); may perturb "
         "checksum/parser behavior in dedicated probe runs",
         kAddr.rcs_bufstream_read_impl, kAddr.rcs_bufstream_seek_impl,
         kAddr.rcs_read_cache_stream);
  debug_result.applied += 2;
}

void RegisterDc3DataArraySafetyOverrides(const Dc3HackContext& ctx,
                                         Dc3HackApplyResult& debug_result) {
  if (!ctx.processor || !ctx.is_decomp_layout) {
    debug_result.skipped++;
    return;
  }

  const uint32_t kMergedDataArrayNodeAddr = kAddr.merged_dataarray_node;
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
      const uint32_t kSetupFontNodeSourceLR = kAddr.setup_font_node_source_lr;
      const uint32_t kSetupFontNodeDestLR = kAddr.setup_font_node_dest_lr;
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
    if (auto* p = memory->TranslateVirtual<uint8_t*>(kAddr.g_system_config)) {
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

    if (caller_lr == kAddr.setup_font_syscfg_return_lr) {
      Dc3LogSetupFontLiteralSanity(memory);
    }

    if (mode_local == "setupfont_fix" &&
        caller_lr == kAddr.setup_font_syscfg_return_lr && arr1 != 0 && arr2 == 0 &&
        !s1_bin && s1_repr == "rnd" && s2_bin) {
      uint32_t repaired =
          Dc3FindArrayLinearBySymbol(memory, arr1, kAddr.pooled_font_string);
      if (repaired) {
        XELOGW(
            "DC3: SetupFont SystemConfig repair applied: substituted arg2 "
            "'font' via pooled literal {:08X} (bad arg2 was {}) -> {:08X}",
            kAddr.pooled_font_string, s2_repr, repaired);
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
        if (auto* p = memory->TranslateVirtual<uint8_t*>(kAddr.g_string_table_global)) {
          g_string_table = xe::load_and_swap<uint32_t>(p + 0x0);
        }

        uint32_t ht_entries = 0;
        uint32_t ht_size = 0;
        uint32_t ht_num_entries = 0;
        uint32_t ht_empty = 0;
        uint32_t ht_removed = 0;
        uint8_t ht_own = 0;
        if (auto* h = memory->TranslateVirtual<uint8_t*>(kAddr.g_hash_table)) {
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

  ctx.processor->RegisterGuestFunctionOverride(kAddr.system_config_2, handler,
                                               "DC3:SystemConfig2(log)");
  XELOGI("DC3: Registered SystemConfig(Symbol,Symbol) probe at {:08X} "
         "(active in FindArray mode={})",
         kAddr.system_config_2, mode);
  debug_result.applied++;
}

// ============================================================================
// Stub data tables (hoisted from function bodies for scanability)
// ============================================================================

struct DebugStubEntry {
  uint32_t address;
  const char* name;
  uint32_t return_value;
};

// Debug/Holmes/String-related stubs and deadlock breakers.
// These functions are stubbed to li r3,<return_value>; blr during boot.
const DebugStubEntry kDebugStubTable[] = {
    {kAddr.xget_locale, "XGetLocale", 0},
    {kAddr.xtl_get_language, "XTLGetLanguage", 1},
    {kAddr.debug_break, "DebugBreak", 0},
    {kAddr.datanode_print, "DataNode::Print", 0},
    // Holmes network/file/poll stubs (not needed for current bring-up).
    {0x831F1678, "HolmesClientInit", 0},
    {0x831F1948, "HolmesClientReInit", 0},
    {0x831F19C8, "HolmesClientPoll", 0},
    {0x831F1248, "HolmesClientPollInternal", 0},
    {0x831F1360, "HolmesClientInitOpcode", 0},
    {0x831F0804, "HolmesClientTerminate", 0},
    {0x831EFB74, "CanUseHolmes", 0},
    {0x831EFBE8, "UsingHolmes", 0},
    {kAddr.protocol_debug_string, "ProtocolDebugString", 0},
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

// ============================================================================
// Per-category apply functions
// ============================================================================

void ApplyImportStopgaps(const Dc3HackContext& ctx,
                         Dc3HackApplyResult& result) {
  Memory* memory = ctx.memory;

  // Stub XapiCallThreadNotifyRoutines.
  const uint32_t kNotifyFuncs[] = {kAddr.xapi_call_thread_notify};
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
      result.applied++;
    } else {
      result.skipped++;
    }
  }

  // Stub unresolved import entries (PE thunks + XEX markers).
  {
    const uint32_t kTextStart = kAddr.text_start;
    const uint32_t kTextSize = kAddr.text_size;
    const uint32_t kIdataStart = kAddr.idata_start;
    const uint32_t kIdataEnd = kAddr.idata_end;
    const uint32_t kThunkAreaStart = kAddr.thunk_area_start;
    const uint32_t kThunkAreaEnd = kAddr.thunk_area_end;

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
        result.failed++;
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
    result.applied += pe_thunks_stubbed + xex_markers_stubbed;
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
      result.applied++;
    }
  }
}

void ApplyDebugStubs(const Dc3HackContext& ctx,
                     Dc3HackApplyResult& result) {
  Memory* memory = ctx.memory;

  if (cvars::dc3_debug_read_cache_stream_step_override) {
    RegisterDc3ReadCacheStreamProbe(ctx, result);
  } else {
    XELOGI("DC3: ReadCacheStream step override disabled (default; non-invasive)");
  }

  // Redirect DECOMP Debug::Print to XELOG.
  if (ctx.processor) {
    const uint32_t kDebugPrint = kAddr.debug_print;
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
    const uint32_t kDebugFail = kAddr.debug_fail;
    auto fail_handler = [](cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      static bool s_in_fail = false;
      if (s_in_fail) {
        ppc_context->r[3] = 0;
        return;
      }
      s_in_fail = true;
      auto* memory = kernel_state->memory();
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t r3 = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t r4 = static_cast<uint32_t>(ppc_context->r[4]);
      static int fail_count = 0;
      fail_count++;
      XELOGE("DC3: Debug::Fail called! LR={:08X} r3={:08X} r4={:08X}", lr, r3, r4);
      if (memory) {
        static bool logged_debug_obj = false;
        auto dump_factory_state = [&](const char* reason) {
          uint8_t hdr[0x20] = {};
          if (auto* map_mem =
                  memory->TranslateVirtual<uint8_t*>(kAddr.object_factories_map)) {
            std::memcpy(hdr, map_mem, sizeof(hdr));
            XELOGW("DC3: Hmx::Object::sFactories [{}] @{:08X} hdr={}",
                   reason, kAddr.object_factories_map,
                   Dc3FmtBytePreview(hdr, sizeof(hdr)));
          } else {
            XELOGW("DC3: Hmx::Object::sFactories [{}] @{:08X} <unmapped>", reason,
                   kAddr.object_factories_map);
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
          dump_static_symbol("RndMat::StaticClassName", kAddr.rndmat_static_name_sym);
          dump_static_symbol("MetaMaterial::StaticClassName",
                             kAddr.metamaterial_static_name_sym);
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
  if (ctx.processor) {
    const uint32_t kDebugDoCrucible = kAddr.debug_do_crucible;
    auto crucible_handler = [](cpu::ppc::PPCContext* ppc_context,
                               kernel::KernelState* kernel_state) {
      static bool s_in_crucible = false;
      static int s_crucible_count = 0;
      s_crucible_count++;
      if (s_in_crucible) {
        ppc_context->r[3] = 0;
        return;
      }
      s_in_crucible = true;
      if (s_crucible_count <= 5 || (s_crucible_count % 5000) == 0) {
        uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
        uint32_t r4 = static_cast<uint32_t>(ppc_context->r[4]);
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
  if (ctx.processor && ctx.is_decomp_layout) {
    const uint32_t kTextStreamOpConstChar = kAddr.textstream_op_const_char;
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

  // Stub XMP overrides.
  {
    struct OutputFunc {
      uint32_t address;
      const char* name;
    };
    OutputFunc output_funcs[] = {
        {kAddr.xmp_override_bg_music, "XMPOverrideBackgroundMusic"},
        {kAddr.xmp_restore_bg_music, "XMPRestoreBackgroundMusic"},
    };
    for (const auto& func : output_funcs) {
      if (PatchStub8Resolved(memory, ctx.hack_pack_stubs, func.address, 0,
                             func.name)) {
        result.applied++;
      } else {
        result.skipped++;
      }
    }
  }

  RegisterDc3WriteBridges(ctx, result);
  RegisterDc3LocaleBootstrapBridges(ctx, result);
  RegisterDc3OutputFormatterBridges(ctx, result);
  RegisterDc3DataArraySafetyOverrides(ctx, result);
  RegisterDc3SystemConfigProbe(ctx, result);
  RegisterDc3MemOrPoolAllocProbe(ctx, result);

  // Apply hoisted debug/Holmes stub table.
  for (const auto& entry : kDebugStubTable) {
    if (PatchStub8Resolved(memory, ctx.hack_pack_stubs, entry.address,
                           entry.return_value, entry.name)) {
      result.applied++;
    } else {
      result.skipped++;
    }
  }

  // Stub String::operator+=(const char*).
  {
    const uint32_t kStringOpPlusEq = kAddr.string_op_plus_eq;
    auto* heap = memory->LookupHeap(kStringOpPlusEq);
    if (heap) {
      auto* mem = memory->TranslateVirtual<uint8_t*>(kStringOpPlusEq);
      if (mem && xe::load_and_swap<uint32_t>(mem) != 0x00000000) {
        heap->Protect(kStringOpPlusEq, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(mem, 0x4E800020);
        XELOGI("DC3: Stubbed String::operator+=(const char*) at {:08X} "
               "(blr — prevents unbounded PE image corruption)",
               kStringOpPlusEq);
        result.applied++;
      } else {
        result.skipped++;
      }
    } else {
      result.skipped++;
    }
  }

  RegisterDc3FindArrayOverride(ctx, result);
}

void ApplyRuntimeStopgaps(const Dc3HackContext& ctx,
                          Dc3HackApplyResult& result) {
  Memory* memory = ctx.memory;
  auto* module = ctx.module;

  // The decomp XEX specifies a 256KB stack which overflows during init.
  if (module->stack_size() < 4 * 1024 * 1024) {
    XELOGI("DC3: Increasing main thread stack from {}KB to 4096KB",
           module->stack_size() / 1024);
    module->set_stack_size(4 * 1024 * 1024);
    result.applied++;
  } else {
    result.skipped++;
  }

  // Make the full PE image + heap region writable.
  {
    auto* heap1 = memory->LookupHeap(0x82000000);
    if (heap1) {
      heap1->Protect(0x82000000, 0x1F60000,
                     kMemoryProtectRead | kMemoryProtectWrite);
      XELOGI("DC3: Made PE image 0x82000000-0x83F60000 writable");
    }
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
    result.applied++;

    if (cvars::dc3_debug_memmgr_assert_nop_bypass) {
      XELOGW("DC3: MemMgr assert nop bypass ENABLED (temporary debug mode)");
      struct MemPatch {
        uint32_t addr;
        uint32_t expected_word;
        const char* name;
      };
      const MemPatch mem_patches[] = {
          {kAddr.meminit_assert, 0x481032B9, "MemInit line 690: Debug::Fail call (STALE)"},
          {kAddr.memalloc_assert, 0x48104389, "MemAlloc line 961: Debug::Fail call (STALE)"},
      };
      for (const auto& p : mem_patches) {
        if (PatchCheckedNop(memory, p.addr, p.expected_word, p.name)) {
          result.applied++;
        } else {
          result.failed++;
        }
      }
    } else {
      XELOGI("DC3: MemMgr assert nop bypass disabled (default)");
      result.skipped++;
    }

    // Initialize STLport std::list<bool> gConditional sentinel.
    {
      const uint32_t kGConditionalAddr = kAddr.g_conditional;  // STALE
      auto* pcond = memory->TranslateVirtual<uint8_t*>(kGConditionalAddr);
      if (pcond) {
        xe::store_and_swap<uint32_t>(pcond + 0, kGConditionalAddr);
        xe::store_and_swap<uint32_t>(pcond + 4, kGConditionalAddr);
        XELOGI("DC3: Initialized gConditional sentinel at {:08X}", kGConditionalAddr);
        result.applied++;
      }
    }
  }

  // Map guard/overflow pages as readable zeros.
#if defined(__linux__)
  {
    struct { uint32_t start; uint32_t size; const char* name; } regions[] = {
      {0x7F000000, 0x01000000, "GPU writeback 0x7F000000"},
      {0xFFFFF000, 0x00002000, "top-of-address-space guard"},
    };
    for (auto& r : regions) {
      uint8_t* host = memory->TranslateVirtual<uint8_t*>(r.start);
      void* mmap_result = mmap(host, r.size, PROT_READ | PROT_WRITE,
                               MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
      if (mmap_result != MAP_FAILED) {
        XELOGI("DC3: Mapped {} ({} bytes)", r.name, r.size);
        result.applied++;
      } else {
        XELOGW("DC3: Failed to map {}: {}", r.name, strerror(errno));
        result.skipped++;
      }
    }
  }
#endif

  // Skip FileIsLocal assert spam on "game:" drive.
  if (ctx.is_decomp_layout) {
    const uint32_t kFileIsLocalAssertBranch = kAddr.file_is_local_assert_branch;
    const uint32_t kExpected = 0x40820040;
    const uint32_t kUnconditional = 0x48000040;
    if (PatchCheckedWord(memory, kFileIsLocalAssertBranch, kExpected,
                         kUnconditional, "FileIsLocal(game:) assert bypass",
                         "DC3: FileIsLocal game-drive assert bypass active")) {
      result.applied++;
    } else {
      result.skipped++;
    }
  }

  // NOTE: Stale addresses disabled (String::~String, NUISPEECH::CSpCfgInst,
  // error-report helper, debug/assert helper). Do NOT re-enable without
  // verifying against current MAP.

  // Decomp-only _errno override.
  if (ctx.is_decomp_layout && ctx.processor) {
    const uint32_t kErrnoAddr = kAddr.errno_fn;
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
        result.applied++;
      } else {
        XELOGW("DC3: Failed to allocate guest errno backing storage for decomp _errno override");
        result.failed++;
      }
    } else {
      result.skipped++;
    }
  }

  // Decomp-only CRT trap stubs.
  if (ctx.is_decomp_layout) {
    if (PatchStub8(memory, kAddr.invalid_parameter_noinfo, 0,
                   "_invalid_parameter_noinfo (CRT trap stopgap)")) {
      result.applied++;
    } else {
      result.skipped++;
    }
    if (PatchStub8(memory, kAddr.call_reportfault, 0,
                   "_call_reportfault (CRT trap stopgap)")) {
      result.applied++;
    }
    if (PatchStub8(memory, kAddr.amsg_exit, 0,
                   "_amsg_exit (CRT trap stopgap)")) {
      result.applied++;
    }
    if (PatchStub8(memory, kAddr.report_gsfailure, 0,
                   "__report_gsfailure (CRT trap stopgap)")) {
      result.applied++;
    }
  }

  // Formatting guard: patch Hx_snprintf _vsnprintf_l callsite.
  if (ctx.is_decomp_layout) {
    const uint32_t kHxSnprintfVsnprintfCall = kAddr.hx_snprintf_vsnprintf_call;
    auto* heap = memory->LookupHeap(kHxSnprintfVsnprintfCall);
    auto* p = memory->TranslateVirtual<uint8_t*>(kHxSnprintfVsnprintfCall);
    if (heap && p) {
      uint32_t w = xe::load_and_swap<uint32_t>(p);
      if (w == 0x4813BE35) {
        heap->Protect(kHxSnprintfVsnprintfCall, 4,
                      kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(p, 0x3860FFFF);
        XELOGI("DC3: Patched Hx_snprintf _vsnprintf_l call {:08X} -> li r3,-1",
               kHxSnprintfVsnprintfCall);
        result.applied++;
      } else if (w == 0x3860FFFF) {
        result.applied++;
      } else {
        XELOGW("DC3: Unexpected Hx_snprintf _vsnprintf_l callsite word {:08X} at {:08X}",
               w, kHxSnprintfVsnprintfCall);
        result.skipped++;
      }
    } else {
      result.skipped++;
    }
  }

  // NOTE: except_data_82910450 stub at 0x82910448 disabled — live NavListSortMgr path.

  // Zero page mapping + null-deref guard.
  {
    auto* heap = memory->LookupHeap(0x00000000);
    if (heap) {
      heap->Protect(0x00000000, 0x10000, kMemoryProtectRead | kMemoryProtectWrite);
      auto* base = memory->TranslateVirtual<uint8_t*>(0x00000000);
      std::memset(base, 0, 0x10000);
      XELOGI("DC3: Mapped zero page 0x0-0x10000 (all zeros)");
      result.applied++;
    } else {
      result.failed++;
    }

#if defined(__linux__)
    {
      auto* vmbase = memory->virtual_membase();
      void* guard_base = vmbase - 0x10000;
      void* mmap_result =
          mmap(guard_base, 0x10000, PROT_READ,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (mmap_result != MAP_FAILED) {
        XELOGI("DC3: Mapped null-deref guard page at {:p} "
               "(64KB below virtual_membase {:p})",
               mmap_result, (void*)vmbase);
        result.applied++;
      } else {
        XELOGI("DC3: Could not map null-deref guard page below "
               "virtual_membase (errno={})",
               errno);
        result.skipped++;
      }
    }
#else
    result.skipped++;
#endif
  }
}

void ApplyCrtPatches(const Dc3HackContext& ctx,
                     Dc3HackApplyResult& result) {
  Memory* memory = ctx.memory;

  // Lazy Symbol::PreInit via StringTable::Add override.
  if (ctx.processor && ctx.is_decomp_layout) {
    const uint32_t kStringTableAdd = kAddr.string_table_add;
    auto st_add_handler = [](cpu::ppc::PPCContext* ppc_context,
                             kernel::KernelState* kernel_state) {
      static bool s_preinit_done = false;
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      auto* processor = ppc_context->processor;
      auto* thread_state = ppc_context->thread_state;

      if (!s_preinit_done && memory) {
        auto* gst_ptr = memory->TranslateVirtual<uint8_t*>(kAddr.g_string_table_global);
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
          processor->Execute(thread_state, kAddr.symbol_preinit, preinit_args, 2);
          gst_val = gst_ptr ? xe::load_and_swap<uint32_t>(gst_ptr) : 0;
          XELOGI("DC3: After PreInit: gStringTable={:08X}", gst_val);
        }
        s_preinit_done = true;

        auto* fn = processor->LookupFunction(kAddr.string_table_add);
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

      uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t str_ptr = static_cast<uint32_t>(ppc_context->r[4]);
      static int fwd_count = 0;
      if (++fwd_count <= 5) {
        XELOGI("DC3: StringTable::Add forwarding[{}]: this={:08X} str={:08X}",
               fwd_count, this_ptr, str_ptr);
      }
      uint64_t args[] = {this_ptr, str_ptr};
      processor->Execute(thread_state, kAddr.string_table_add, args, 2);
      ppc_context->r[3] = thread_state->context()->r[3];
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kStringTableAdd, st_add_handler,
        "DC3:StringTable::Add(lazy-PreInit)");
    XELOGI("DC3: Registered StringTable::Add lazy-PreInit override at {:08X}",
           kStringTableAdd);
  }

  // CRT sanitizer + injection.
  {
    const uint32_t kCodeStart = 0x822C0000;
    struct CrtTable {
      uint32_t start;
      uint32_t end;
      const char* name;
    };
    const uint32_t kXcA = kAddr.xc_a;
    const uint32_t kXcZ = kAddr.xc_z;
    const uint32_t kXiA = kAddr.xi_a;
    const uint32_t kXiZ = kAddr.xi_z;
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
      result.applied++;
    }

    // Inject _ioinit into the __xi_a slot.
    {
      const uint32_t kIoinitAddr = kAddr.ioinit;
      auto* xi_slot = memory->TranslateVirtual<uint8_t*>(kXiA);
      if (xi_slot) {
        xe::store_and_swap<uint32_t>(xi_slot, kIoinitAddr);
        XELOGI("DC3: Injected _ioinit ({:08X}) into __xi_a slot at {:08X}",
               kIoinitAddr, kXiA);
        result.applied++;
      }
    }

    // Inject Symbol::PreInit into __xc[0].
    if (ctx.processor) {
      auto* xc_first = memory->TranslateVirtual<uint8_t*>(kXcA);
      uint32_t first_entry = xc_first ? xe::load_and_swap<uint32_t>(xc_first) : 1;
      if (first_entry == 0) {
        const uint32_t kTrampAddr = kAddr.protocol_debug_string;  // CODE section
        auto* heap = memory->LookupHeap(kTrampAddr);
        auto* t = memory->TranslateVirtual<uint8_t*>(kTrampAddr);
        if (heap && t) {
          heap->Protect(kTrampAddr, 80, kMemoryProtectRead | kMemoryProtectWrite);
          uint32_t code[] = {
            0x7C0802A6,  // mflr r0
            0x90010004,  // stw r0, 4(r1)
            0x9421FFF0,  // stwu r1, -16(r1)
            0x3C6083AE,  // lis r3, 0x83AE
            0x80630190,  // lwz r3, 0x0190(r3)
            0x2C030000,  // cmpwi r3, 0
            0x40820024,  // bne +36
            0x3C600008,  // lis r3, 0x0008
            0x60638B80,  // ori r3, r3, 0x8B80
            0x3C800001,  // lis r4, 0x0001
            0x60843880,  // ori r4, r4, 0x3880
            0x3D808255,  // lis r12, 0x8255
            0x618C6E70,  // ori r12, r12, 0x6E70
            0x7D8903A6,  // mtctr r12
            0x4E800421,  // bctrl
            0x38600000,  // li r3, 0
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
          uint32_t xc0_readback = xe::load_and_swap<uint32_t>(xc_first);
          uint32_t tramp_w0 = xe::load_and_swap<uint32_t>(t);
          uint32_t tramp_w1 = xe::load_and_swap<uint32_t>(t + 4);
          XELOGI("DC3: Verify: __xc[0]={:08X} tramp[0]={:08X} tramp[1]={:08X}",
                 xc0_readback, tramp_w0, tramp_w1);
          result.applied++;
        }
      } else {
        XELOGW("DC3: __xc[0] not null ({:08X}), cannot inject PreInit trampoline",
               first_entry);
      }
    }

    // Diagnostic: dump _cinit instructions.
    {
      const uint32_t kCinit = kAddr.cinit;
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
}

// ============================================================================
// Dispatcher
// ============================================================================

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

  ApplyImportStopgaps(ctx, import_result);
  ApplyDebugStubs(ctx, debug_result);
  ApplyRuntimeStopgaps(ctx, stopgap_result);
  ApplyCrtPatches(ctx, crt_result);
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

}  // namespace xe
