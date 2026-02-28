#include "xenia/dc3_hack_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <set>
#include <string>

#include "config.h"

#if defined(__linux__)
#include <cerrno>
#include <sys/mman.h>
#endif

#include "xenia/base/atomic.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"

DECLARE_int32(dc3_crt_bisect_max);
DECLARE_string(dc3_crt_skip_indices);
DECLARE_bool(dc3_crt_skip_nui);
DECLARE_bool(dc3_debug_read_cache_stream_step_override);
DECLARE_bool(dc3_null_read_cache_stream);
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
//
// These defaults are overridden at runtime by the manifest's address_catalog
// (see Dc3PopulateAddressesFromCatalog below). When adding a new field:
//   1. Add the field + hardcoded default here
//   2. Add the MAP symbol to ADDRESS_CATALOG in dc3-decomp's
//      scripts/build/generate_xenia_dc3_patch_manifest.py
//   3. Add a get() call in Dc3PopulateAddressesFromCatalog() at the bottom
//      of this file
struct Dc3Addresses {
  // CRT sentinels (from manifest crt_sentinels)
  uint32_t xc_a = 0x83341580;
  uint32_t xc_z = 0x83341B98;
  uint32_t xi_a = 0x83341B9C;
  uint32_t xi_z = 0x83341BA8;
  // Decomp .CRT$XCU section (separate from auto_08 __xc table)
  uint32_t crt_xcu_start = 0x83627800;
  uint32_t crt_xcu_end = 0x83627B44;
  // CRT functions
  uint32_t ioinit = 0x82F4AF28;
  uint32_t cinit = 0x82E43F08;
  uint32_t errno_fn = 0x82F41EE0;
  uint32_t invalid_parameter_noinfo = 0x82F484D0;
  uint32_t call_reportfault = 0x82F48504;
  uint32_t amsg_exit = 0x82F3E210;
  uint32_t report_gsfailure = 0x82F4B09C;
  // CRT formatter
  uint32_t output_l = 0x82F4943C;
  uint32_t woutput_l = 0x82F4ECAC;
  uint32_t hx_snprintf_vsnprintf_call = 0x83477FBC;  // STALE — not in manifest
  // Debug subsystem
  uint32_t debug_print = 0x8297D380;
  uint32_t debug_fail = 0x8297DD08;
  uint32_t debug_do_crucible = 0x832A7F80;
  uint32_t datanode_print = 0x8261A900;
  // Import/thunk
  uint32_t xapi_call_thread_notify = 0x82E4405C;
  uint32_t text_start = 0x82450000;
  uint32_t text_size = 0x00E1B88C;
  uint32_t idata_start = 0x82447400;
  uint32_t idata_end = 0x82447A34;
  uint32_t thunk_area_start = 0x82450000;
  uint32_t thunk_area_end = 0x8326B88C;
  // Locale
  uint32_t get_system_language = 0x828307A0;
  uint32_t get_system_locale = 0x82830E88;
  uint32_t xget_locale = 0x83247A8C;
  uint32_t xtl_get_language = 0x83247C94;
  uint32_t debug_break = 0x83247D34;
  // ReadCacheStream probes
  uint32_t rcs_read_cache_stream = 0x82569750;
  uint32_t rcs_bufstream_read_impl = 0x82A5BCF8;
  uint32_t rcs_bufstream_seek_impl = 0x82A5BDF8;
  // SystemConfig / FindArray / SetupFont
  uint32_t system_config_2 = 0x8294CEC8;
  uint32_t find_array = 0x829776E0;
  uint32_t setup_font_syscfg_return_lr = 0x8317FF14;       // STALE — not in manifest
  uint32_t setup_font_ctor1_literal = 0x82027684;          // STALE — not in manifest
  uint32_t setup_font_ctor2_literal = 0x82053BF8;          // STALE — not in manifest
  uint32_t pooled_font_string = 0x82017684;                // STALE — not in manifest
  uint32_t setup_font_node_source_lr = 0x8317FF40;         // STALE — not in manifest
  uint32_t setup_font_node_dest_lr = 0x8318001C;           // STALE — not in manifest
  // Object / factory globals
  uint32_t object_factories_map = 0x833D9728;
  uint32_t register_factory = 0x829308E0;
  uint32_t new_object = 0x82930778;
  uint32_t rndmat_static_name_sym = 0x8338F7F8;
  uint32_t metamaterial_static_name_sym = 0x83394970;
  uint32_t g_system_config = 0x833DE3B8;
  uint32_t read_system_config = 0x8294D968;
  uint32_t g_string_table_global = 0x833957E0;
  uint32_t g_hash_table = 0x833957E4;
  // CriticalSection
  uint32_t critsec_ctor = 0x82778A68;
  uint32_t critsec_enter = 0x82778AA8;
  uint32_t critsec_exit = 0x82778AE8;
  // Memory / allocator
  uint32_t mem_or_pool_alloc = 0x82863120;
  uint32_t mem_alloc = 0x828630F0;
  uint32_t pool_alloc = 0x82A5B300;
  uint32_t mem_free = 0x828628F8;
  uint32_t pool_free = 0x82A5AFE8;
  uint32_t mem_or_pool_free = 0x82862D00;
  uint32_t operator_new = 0x828630F0;
  uint32_t operator_delete = 0x82863108;
  uint32_t g_num_heaps = 0x833D6260;
  uint32_t string_reserve = 0x8298C378;
  uint32_t string_reserve_memalloc_ret_lr = 0x82A5BC00;    // STALE — not in manifest
  uint32_t g_chunk_alloc = 0x83543F90;
  // DataArray / DataNode
  uint32_t merged_dataarray_node = 0x82976790;  // DataArray::Node (DataArray.obj)
  uint32_t string_table_add = 0x828E2240;
  uint32_t symbol_preinit = 0x8261CF08;
  // DTA text parser
  uint32_t data_input = 0x826B0EF0;            // DataInput — noop stub (ALTERNATENAME)
  uint32_t data_read_stream = 0x8256A278;       // DataReadStream (DataFile.obj)
  uint32_t parse_array = 0x826B0EF0;            // ParseArray — noop stub (ALTERNATENAME)
  // TextStream
  uint32_t textstream_op_const_char = 0x8292A2C8;
  // String ops
  uint32_t string_op_plus_eq = 0x8298C410;
  // XMP
  uint32_t xmp_override_bg_music = 0x82F866F8;
  uint32_t xmp_restore_bg_music = 0x82F867D0;
  // Write bridges
  uint32_t write_nolock = 0x82F44888;
  uint32_t write_fn = 0x82F44AC8;
  // FileIsLocal
  uint32_t file_is_local = 0x82A04FF8;
  uint32_t file_is_local_assert_branch = 0x82A05020;  // file_is_local + 0x28
  // File system globals
  uint32_t g_using_cd = 0x833DE3B0;
  uint32_t check_for_archive = 0x8294D868;
  uint32_t file_init = 0x8283B448;
  uint32_t archive_init = 0x829246E0;
  uint32_t the_archive = 0x833D95F0;
  // ArkFile (BlockMgr bypass — all BlockMgr methods are noop'd via ALTERNATENAME)
  uint32_t arkfile_read = 0x82680BB8;
  // Original binary addresses for globals (from symbols.txt).
  // When decomp code writes to the decomp address but original binary code
  // reads from the original address, we must sync both.
  uint32_t g_using_cd_orig = 0x82F652E8;      // STALE — original binary addr
  uint32_t the_archive_orig = 0x82F679FC;      // STALE — original binary addr
  uint32_t system_pre_init_1 = 0x8294EFA0;
  uint32_t system_pre_init_2 = 0x8294F218;
  // CRT functions that block the main thread
  uint32_t mtinit = 0x82F3E4F0;
  uint32_t xregister_thread_notify = 0x82E440CC;
  // gConditional / gDataArrayConditional (BSS, not in MAP)
  // Addresses are extracted at runtime from their ctor functions' PPC code.
  uint32_t g_conditional = 0;
  uint32_t g_conditional_ctor = 0x832645B8;
  uint32_t g_data_array_conditional = 0;
  uint32_t g_data_array_conditional_ctor = 0x83266548;
  // Object / factory (PPC patch targets)
  uint32_t load_meta_materials = 0x82657520;
  uint32_t object_set_name = 0x8292D288;
  // STL container globals (BSS, sentinel init from host)
  uint32_t the_load_mgr = 0x833D5DE0;
  uint32_t auto_timer_stmrs = 0x83510860;
  uint32_t rnd_overlay_soverlays = 0x835118B0;
  uint32_t synth_pollable_spollables = 0x83512698;
  uint32_t midi_parser_sparsers = 0x833D8E48;
  uint32_t rnd_multi_mesh_sproxy = 0x83511B90;
  uint32_t g_caches = 0x833CF920;
  uint32_t g_decompression_queue = 0x833D7478;
  // LoadMgr (async file I/O)
  uint32_t poll_front_loader = 0x826B0EF0;    // noop stub (ALTERNATENAME)
  uint32_t poll_until_loaded = 0x82858D10;
  // Holmes trampoline target (reused as PPC code cave)
  uint32_t protocol_debug_string = 0x8262F6B8;
  // Wind (DC3-specific; RB3 stubs SetWind)
  uint32_t set_wind = 0x826C0A78;
  // RndTransformable
  uint32_t set_dirty_force = 0x828B1F20;
  // Memory_Xbox
  uint32_t alloc_type = 0x83276C08;
  // Rnd
  uint32_t rnd_create_defaults = 0x825CC550;
  // MetaMaterial
  uint32_t create_and_set_meta_mat = 0x825DC5E0;
  uint32_t s_meta_materials = 0x83544BD8;
  // Post-processing / GPU init
  uint32_t ng_postproc_rebuild_tex = 0x8292A818;
  uint32_t ng_dofproc_init = 0x82814A78;
  uint32_t rnd_shadowmap_init = 0x8264D838;
  uint32_t dxrnd_suspend = 0x827C7838;
  uint32_t occlusion_query_mgr_ctor = 0x827C6048;
  uint32_t d3d_device_suspend = 0x830F7A00;
  uint32_t d3d_device_resume = 0x830F7AA0;
  uint32_t dxrnd_init_buffers = 0x827C7F40;
  uint32_t dxrnd_create_post_textures = 0x827C8C78;
  // Audio / Synth
  uint32_t synth360_preinit = 0x826B0EF0;    // noop stub (ALTERNATENAME, not in MAP)
  uint32_t synth_init = 0x8271ADB8;
  // Bink video
  uint32_t bink_start_async_thread = 0x826B0EF0;  // noop stub (ALTERNATENAME)
  uint32_t bink_platform_init = 0x826B0EF0;        // noop stub (ALTERNATENAME, not in MAP)
  // CRT RTTI
  uint32_t rt_dynamic_cast = 0x82F3D2B4;
  // String constants
  uint32_t g_null_str = 0x832F2F18;
  // SkeletonIdentifier (Kinect player identification)
  uint32_t skeleton_identifier_init = 0x826B0EF0;  // noop stub (ALTERNATENAME)
  uint32_t skeleton_identifier_poll = 0x826B0EF0;  // noop stub (ALTERNATENAME)
  // OSCMessenger (Holmes debug networking)
  uint32_t osc_messenger_poll = 0x826B0EF0;        // noop stub (ALTERNATENAME)
  // BinStream::Read — Fail() check (beq to normal path)
  uint32_t binstream_read_fail_check = 0x8258EA50;  // binstream_read + 0x30
  // Rand2 (BinStream encryption PRNG)
  uint32_t rand2_ctor = 0x82DD6FD8;
  uint32_t rand2_int = 0x82DD7000;
  // BinStream::Read — full function
  uint32_t binstream_read = 0x8258EA20;
  // BinStream::ReadEndian — called by all >> operators
  uint32_t binstream_read_endian = 0x8258EC10;
  // operator>>(BinStream&, DataArray*&)
  uint32_t bs_op_dataarray = 0x82978E90;
};
Dc3Addresses kAddr;

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

// --------------------------------------------------------------------------
// Guest-memory pool allocator.
//
// SystemHeapAlloc has a 4KB minimum granularity (virtual page commit),
// which wastes ~500MB when the game makes 125,000+ small allocations
// (16-32 byte DataArray nodes during config parsing).
//
// This pool allocator pre-allocates 1MB chunks from SystemHeapAlloc and
// bump-allocates within them.  Free is a no-op for pool allocations —
// the memory stays allocated until the process exits.  Only allocations
// ≤ kPoolMaxSize go through the pool; larger ones fall through to
// SystemHeapAlloc directly.
// --------------------------------------------------------------------------
class Dc3GuestPool {
 public:
  static constexpr uint32_t kChunkSize = 1024 * 1024;  // 1MB chunks
  static constexpr uint32_t kPoolMaxSize = 512;         // pool allocs ≤ 512B
  static constexpr uint32_t kAlignment = 16;            // 16-byte aligned

  void Init(Memory* mem) { memory_ = mem; }

  // Returns guest address, or 0 on failure.
  uint32_t Alloc(uint32_t size) {
    if (!memory_ || size == 0 || size > kPoolMaxSize) return 0;
    uint32_t aligned = (size + kAlignment - 1) & ~(kAlignment - 1);
    std::lock_guard<std::mutex> lock(mu_);
    if (cursor_ + aligned > chunk_end_) {
      if (!AllocNewChunk()) return 0;
    }
    uint32_t ptr = cursor_;
    cursor_ += aligned;
    total_pool_bytes_ += aligned;
    ++total_pool_allocs_;
    return ptr;
  }

  // Check if an address was allocated from this pool.
  // Used by free() handlers to skip SystemHeapFree for pool addresses.
  bool IsPoolAddress(uint32_t addr) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& chunk : chunks_) {
      if (addr >= chunk.start && addr < chunk.end) return true;
    }
    return false;
  }

  uint64_t total_pool_allocs() const { return total_pool_allocs_; }
  uint64_t total_pool_bytes() const { return total_pool_bytes_; }
  uint32_t chunks_allocated() const { return chunks_allocated_; }

 private:
  struct ChunkRange {
    uint32_t start;
    uint32_t end;
  };

  bool AllocNewChunk() {
    uint32_t chunk = memory_->SystemHeapAlloc(kChunkSize, kAlignment);
    if (!chunk) {
      XELOGW("DC3: Pool allocator failed to allocate {}KB chunk",
             kChunkSize / 1024);
      return false;
    }
    cursor_ = chunk;
    chunk_end_ = chunk + kChunkSize;
    chunks_.push_back({chunk, chunk_end_});
    ++chunks_allocated_;
    return true;
  }

  Memory* memory_ = nullptr;
  mutable std::mutex mu_;
  uint32_t cursor_ = 0;
  uint32_t chunk_end_ = 0;
  std::vector<ChunkRange> chunks_;
  uint32_t chunks_allocated_ = 0;
  uint64_t total_pool_allocs_ = 0;
  uint64_t total_pool_bytes_ = 0;
};

static Dc3GuestPool g_dc3_pool;

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

// Host-side SetDirty_Force with cycle detection.
// Mirrors RndTransformable::SetDirty_Force but breaks on circular mChildren.
void HostSetDirtyForce(Memory* memory, uint32_t this_ptr, int depth) {
  if (depth > 100) {
    XELOGW("DC3: SetDirty_Force recursion depth > 100 on {:08X}", this_ptr);
    return;
  }
  if (!this_ptr || this_ptr >= 0xF0000000) return;
  auto* obj = memory->TranslateVirtual<uint8_t*>(this_ptr);
  if (!obj) return;

  // Set mDirty = true (offset 0xBD)
  obj[0xBD] = 1;

  // mChildren sentinel at offset 0x9C (std::list: next at +0, prev at +4)
  uint32_t sentinel_addr = this_ptr + 0x9C;
  uint32_t node_addr = xe::load_and_swap<uint32_t>(obj + 0x9C);

  if (node_addr == sentinel_addr) return;  // empty list

  // Walk with cycle detection
  std::set<uint32_t> visited;
  int count = 0;
  while (node_addr != sentinel_addr) {
    if (!visited.insert(node_addr).second) {
      XELOGW(
          "DC3: SetDirty_Force CYCLE at node {:08X} on obj {:08X} after {} nodes",
          node_addr, this_ptr, count);
      break;
    }
    if (++count > 10000) {
      XELOGW("DC3: SetDirty_Force >10000 children on obj {:08X}", this_ptr);
      break;
    }
    if (!node_addr || node_addr >= 0xF0000000) {
      XELOGW("DC3: SetDirty_Force bad node {:08X} on obj {:08X}", node_addr,
             this_ptr);
      break;
    }
    auto* node = memory->TranslateVirtual<uint8_t*>(node_addr);
    uint32_t child_ptr = xe::load_and_swap<uint32_t>(node + 0x08);
    if (child_ptr && child_ptr < 0xF0000000) {
      auto* child = memory->TranslateVirtual<uint8_t*>(child_ptr);
      if (child && child[0xBD] == 0) {
        HostSetDirtyForce(memory, child_ptr, depth + 1);
      }
    }
    node_addr = xe::load_and_swap<uint32_t>(node + 0x00);
  }
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

// ============================================================================
// Host-side Rand2 (Park-Miller LCG) — mirrors DC3 Rand2 for DTB decryption
// ============================================================================
class HostRand2 {
 public:
  explicit HostRand2(int seed) {
    if (seed == 0) seed = 1;
    if (seed < 0) seed = -seed;
    seed_ = seed;
  }
  // Returns next value in [1, 2^31-2]. Low 8 bits used as XOR byte.
  int Int() {
    int test = (seed_ % 127773) * 16807 - (seed_ / 127773) * 2836;
    if (test > 0)
      seed_ = test;
    else
      seed_ = test + 0x7FFFFFFF;
    return seed_;
  }
 private:
  int seed_;
};

// ============================================================================
// Host-side ArkFile::Read — bypasses the noop'd BlockMgr layer
//
// All BlockMgr methods (GetAssociatedBlocks, AddTask, Poll, KillBlockRequests)
// resolve to __link_glue_noop via ALTERNATENAME.  This makes ArkFile::Read
// (which calls ReadAsync → BlockMgr) completely broken: ReadAsync enqueues
// nothing, ReadDone loops forever since mNumOutstandingTasks never clears.
//
// This handler replaces the entire ArkFile::Read with a host-side
// implementation that reads directly from .ark files via Xenia's VFS.
//
// Guest ArkFile layout (from ArkFile_p.h):
//   +0x00  vtable ptr
//   +0x04  mArkfileNum (int)
//   +0x08  mByteStart  (u64)   — byte offset within .ark file
//   +0x10  mSize       (int)   — file entry size
//   +0x14  mUCSize     (int)   — uncompressed size
//   +0x18  mNumOutstandingTasks (int)
//   +0x1C  mBytesRead  (int)
//   +0x20  mTell       (int)   — current read position
//   +0x24  mFail       (bool)
//   +0x28  mReadStartTime (float)
//   +0x2C  mReadAhead  (bool)
//   +0x30  mFilename   (String)
//
// Guest Archive layout (from Archive.h):
//   +0x00  mNumArkfiles (int)
//   +0x04  mArkfileSizes (vector<uint>)  — [_Myfirst, _Mylast, _Myend]
//   +0x10  mArkfileNames (vector<String>) — [_Myfirst, _Mylast, _Myend]
//
// Guest String layout: [vtable (0x0), char* mStr (0x4)] = 8 bytes
// ============================================================================
void Dc3ArkFileReadHandler(cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) return;
  auto* memory = kernel_state->memory();
  if (!memory) return;
  auto* mem = memory->virtual_membase();

  // PPC ABI: r3=this, r4=buffer, r5=size
  uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
  uint32_t buf_addr = static_cast<uint32_t>(ppc_context->r[4]);
  int requested = static_cast<int>(ppc_context->r[5]);

  // Read ArkFile member fields (big-endian guest memory).
  int arkfileNum = xe::load_and_swap<int32_t>(mem + this_ptr + 0x04);
  uint64_t byteStart = xe::load_and_swap<uint64_t>(mem + this_ptr + 0x08);
  int size = xe::load_and_swap<int32_t>(mem + this_ptr + 0x10);
  int tell = xe::load_and_swap<int32_t>(mem + this_ptr + 0x20);
  bool fail = *(mem + this_ptr + 0x24) != 0;

  if (fail || tell >= size || requested <= 0) {
    ppc_context->r[3] = 0;
    return;
  }

  // Clamp to remaining file bytes.
  int toRead = std::min(requested, size - tell);
  if (toRead <= 0) {
    ppc_context->r[3] = 0;
    return;
  }

  // Read ark filename from TheArchive->mArkfileNames[arkfileNum].
  // TheArchive is a guest pointer; mArkfileNames is a vector<String> at +0x10.
  uint32_t archive_ptr = xe::load_and_swap<uint32_t>(mem + kAddr.the_archive);
  if (!archive_ptr) {
    XELOGW("DC3: ArkFile::Read — TheArchive is null");
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }
  // vector<String>._Myfirst at Archive+0x10
  uint32_t names_first =
      xe::load_and_swap<uint32_t>(mem + archive_ptr + 0x10);
  if (!names_first) {
    XELOGW("DC3: ArkFile::Read — mArkfileNames._Myfirst is null");
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }
  // String[arkfileNum].mStr — each String is 8 bytes [vtable, mStr].
  // mStr is at offset +0x04 within each String object.
  uint32_t str_ptr =
      xe::load_and_swap<uint32_t>(mem + names_first + arkfileNum * 8 + 4);
  if (!str_ptr) {
    XELOGW("DC3: ArkFile::Read — ark name string pointer is null for idx {}",
            arkfileNum);
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }
  const char* ark_name = reinterpret_cast<const char*>(mem + str_ptr);

  // Validate ark_name: check for reasonable length and ASCII.
  size_t ark_name_len = 0;
  bool ark_name_ok = true;
  for (const char* p = ark_name; *p && ark_name_len < 512; ++p, ++ark_name_len) {
    if (static_cast<unsigned char>(*p) > 127) {
      ark_name_ok = false;
      break;
    }
  }
  if (!ark_name_ok || ark_name_len == 0 || ark_name_len >= 512) {
    XELOGW("DC3: ArkFile::Read — invalid ark name for idx {} "
            "(str_ptr={:08X}, len={})",
            arkfileNum, str_ptr, ark_name_len);
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }

  // Construct VFS path: "d:\\" + ark_name (with / → backslash).
  std::string vfs_path = "d:\\";
  for (const char* p = ark_name; *p; ++p) {
    vfs_path += (*p == '/') ? '\\' : *p;
  }

  // Resolve VFS entry and open for read.
  auto* fs = kernel_state->file_system();
  auto* entry = fs->ResolvePath(vfs_path);
  if (!entry) {
    XELOGW("DC3: ArkFile::Read — VFS path not found: {}", vfs_path);
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }

  vfs::File* vfs_file = nullptr;
  auto status = entry->Open(
      xe::filesystem::FileAccess::kFileReadData, &vfs_file);
  if (XFAILED(status) || !vfs_file) {
    XELOGW("DC3: ArkFile::Read — failed to open {}: {:08X}", vfs_path, status);
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }

  // Read bytes at absolute file offset = byteStart + tell.
  uint64_t file_offset = byteStart + static_cast<uint64_t>(tell);
  void* host_buf = memory->TranslateVirtual(buf_addr);
  size_t bytes_read = 0;
  status = vfs_file->ReadSync(host_buf, toRead, file_offset, &bytes_read);
  vfs_file->Destroy();

  if (XFAILED(status)) {
    XELOGW("DC3: ArkFile::Read — ReadSync failed at offset {}: {:08X}",
            file_offset, status);
    *(mem + this_ptr + 0x24) = 1;
    ppc_context->r[3] = 0;
    return;
  }

  // Update ArkFile fields: mBytesRead and mTell.
  int ibr = static_cast<int>(bytes_read);
  xe::store_and_swap<int32_t>(mem + this_ptr + 0x1C, ibr);       // mBytesRead
  xe::store_and_swap<int32_t>(mem + this_ptr + 0x20, tell + ibr); // mTell

  static int s_log_count = 0;
  if (s_log_count < 40) {
    // Hex dump first 16 bytes of returned data for verification.
    auto* buf_host = reinterpret_cast<const uint8_t*>(host_buf);
    std::string hex;
    for (size_t i = 0; i < std::min<size_t>(bytes_read, 16); ++i) {
      char tmp[4];
      std::snprintf(tmp, sizeof(tmp), "%02X ", buf_host[i]);
      hex += tmp;
    }
    XELOGI("DC3: ArkFile::Read(ark={}, {} bytes @ offset {}) -> {} from {} "
           "[byteStart={} tell={}] data=[{}]",
           arkfileNum, toRead, file_offset, bytes_read, ark_name,
           byteStart, tell, hex);
    ++s_log_count;
  }

  ppc_context->r[3] = static_cast<uint64_t>(bytes_read);
}

// ============================================================================
// Host-side ReadCacheStream diagnostic — decrypts DTB header on host
// ============================================================================
void Dc3ReadCacheStreamDiagnostic(cpu::ppc::PPCContext* ppc_context,
                                  kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) return;
  auto* memory = kernel_state->memory();
  if (!memory) return;

  // ReadCacheStream(BinStream &bs, const char *cc)
  // PPC ABI: r3 = BinStream* (BufStream on stack), r4 = const char* filename
  uint32_t bs_addr = static_cast<uint32_t>(ppc_context->r[3]);
  uint32_t cc_addr = static_cast<uint32_t>(ppc_context->r[4]);

  const char* filename = "<unknown>";
  if (cc_addr && cc_addr < 0xF0000000) {
    auto* p = memory->TranslateVirtual<const char*>(cc_addr);
    if (p) filename = p;
  }

  auto* bs = memory->TranslateVirtual<uint8_t*>(bs_addr);
  if (!bs) {
    XELOGW("DC3:RCS DIAG: BufStream translate fail bs={:08X} file={}",
           bs_addr, filename);
    ppc_context->r[3] = 0;
    return;
  }

  // Dump raw BufStream object as 4-byte words to determine layout
  {
    XELOGI("DC3:RCS DIAG: BufStream raw dump at {:08X} for file={}:",
           bs_addr, filename);
    for (int off = 0; off < 0x38; off += 4) {
      uint32_t w = xe::load_and_swap<uint32_t>(bs + off);
      XELOGI("DC3:RCS DIAG:   +{:02X}: {:08X}  ({})", off, w, static_cast<int32_t>(w));
    }
  }

  // BufStream layout (DC3) — tentative, verify with dump above:
  //   0x00: vtable
  //   0x04: mLittleEndian (bool, 4 bytes with padding)
  //   0x08: mCrypto (Rand2*)
  //   0x0c: mRevStack (vector<ObjVersion>*)
  //   0x10: mBuffer (char*)
  //   0x14: mFail (bool, 4 bytes with padding)
  //   0x18: mTell (int)
  //   0x1c: mSize (int)
  //   0x20: mChecksum (StreamChecksumValidator*)
  //   0x24: mBytesChecksummed (int)
  uint32_t vtable = xe::load_and_swap<uint32_t>(bs + 0x00);
  uint8_t little_endian = bs[0x04];
  uint32_t crypto_ptr = xe::load_and_swap<uint32_t>(bs + 0x08);
  uint32_t buf_ptr = xe::load_and_swap<uint32_t>(bs + 0x10);
  uint8_t fail = bs[0x14];
  int32_t tell = xe::load_and_swap<int32_t>(bs + 0x18);
  int32_t size = xe::load_and_swap<int32_t>(bs + 0x1c);
  uint32_t checksum = xe::load_and_swap<uint32_t>(bs + 0x20);

  XELOGI("DC3:RCS DIAG: file={} bs={:08X} vtbl={:08X} LE={} crypto={:08X} "
         "buf={:08X} fail={} tell={} size={} chk={:08X}",
         filename, bs_addr, vtable, little_endian, crypto_ptr,
         buf_ptr, fail, tell, size, checksum);

  // Check candidate buffer addresses: dump 32 bytes from each pointer-like
  // field to empirically determine which is the actual file data buffer.
  uint32_t candidates[] = {
      xe::load_and_swap<uint32_t>(bs + 0x04),  // +04 (could be ptr if not LE bool)
      xe::load_and_swap<uint32_t>(bs + 0x10),  // +10 (expected mBuffer)
      xe::load_and_swap<uint32_t>(bs + 0x14),  // +14 (mystery constant)
      xe::load_and_swap<uint32_t>(bs + 0x18),  // +18 (expected mTell but looks like ptr)
  };
  for (int ci = 0; ci < 4; ++ci) {
    uint32_t addr = candidates[ci];
    if (!addr || addr >= 0xF0000000) continue;
    auto* p = memory->TranslateVirtual<uint8_t*>(addr);
    if (!p) continue;
    std::string hex;
    for (int i = 0; i < 32; ++i) {
      char tmp[4];
      snprintf(tmp, sizeof(tmp), "%02X", p[i]);
      hex += tmp;
      if ((i % 4) == 3) hex += ' ';
    }
    // Also check if it's a printable string
    bool printable = true;
    int slen = 0;
    for (int i = 0; i < 64 && p[i]; ++i) {
      if (p[i] < 0x20 || p[i] > 0x7e) { printable = false; break; }
      slen++;
    }
    if (printable && slen > 0) {
      char str[65] = {};
      std::memcpy(str, p, std::min(slen, 64));
      XELOGI("DC3:RCS DIAG: ptr[+{:02X}]={:08X} → bytes=[{}] str=\"{}\"",
             ci == 0 ? 0x04 : (ci == 1 ? 0x10 : (ci == 2 ? 0x14 : 0x18)),
             addr, hex, str);
    } else {
      bool all_zero = true;
      for (int i = 0; i < 32; ++i) if (p[i]) { all_zero = false; break; }
      XELOGI("DC3:RCS DIAG: ptr[+{:02X}]={:08X} → bytes=[{}]{}",
             ci == 0 ? 0x04 : (ci == 1 ? 0x10 : (ci == 2 ? 0x14 : 0x18)),
             addr, hex, all_zero ? " ALL_ZERO" : "");
    }
  }

  // Use +10 as buffer address for now (expected layout)
  if (!buf_ptr || buf_ptr >= 0xF0000000) {
    XELOGW("DC3:RCS DIAG: invalid buf_ptr {:08X} — returning nullptr", buf_ptr);
    ppc_context->r[3] = 0;
    return;
  }

  auto* buf = memory->TranslateVirtual<uint8_t*>(buf_ptr);
  if (!buf) {
    XELOGW("DC3:RCS DIAG: buffer translate fail buf={:08X}", buf_ptr);
    ppc_context->r[3] = 0;
    return;
  }

  // For the size, use +1C which consistently holds a reasonable value.
  // If tell looks like a pointer (>0x10000), treat it as 0 (fresh stream).
  int actual_tell = (tell > 0x10000) ? 0 : tell;
  int remaining = size - actual_tell;
  if (remaining <= 0) {
    XELOGW("DC3:RCS DIAG: no bytes remaining (tell={} size={} actual_tell={})",
           tell, size, actual_tell);
    ppc_context->r[3] = 0;
    return;
  }
  int dump_len = std::min(remaining, 128);

  // Dump first 128 raw bytes from buffer start (tell=0)
  {
    std::string hex;
    for (int i = 0; i < dump_len; ++i) {
      char tmp[4];
      snprintf(tmp, sizeof(tmp), "%02X", buf[actual_tell + i]);
      hex += tmp;
      if ((i % 32) == 31 || i == dump_len - 1) {
        XELOGI("DC3:RCS DIAG: raw+{:04X}: {}", i - (i % 32), hex);
        hex.clear();
      } else if ((i % 4) == 3) {
        hex += ' ';
      }
    }
  }

  // Now decrypt the header using host-side Rand2
  if (remaining < 7) {  // need at least 4 (seed) + 1 (bool) + 2 (size)
    XELOGW("DC3:RCS DIAG: buffer too small ({} bytes from tell={}) for DTB header",
           remaining, tell);
    ppc_context->r[3] = 0;
    return;
  }

  // Step 1: Read 4-byte seed (UNENCRYPTED, stored in stream's endian format)
  // BufStream created with littleEndian=true → multi-byte values in LE format
  uint8_t* p = buf + tell;
  int32_t seed;
  if (little_endian) {
    seed = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
  } else {
    seed = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  }
  p += 4;
  XELOGI("DC3:RCS DIAG: seed={} (0x{:08X})", seed, static_cast<uint32_t>(seed));

  HostRand2 rng(seed);

  // Step 2: Decrypt and read 1-byte bool (has root array?)
  uint8_t enc_bool = *p++;
  uint8_t xor_byte = static_cast<uint8_t>(rng.Int());
  uint8_t dec_bool = enc_bool ^ xor_byte;
  XELOGI("DC3:RCS DIAG: bool: enc={:02X} xor={:02X} dec={:02X} ({})",
         enc_bool, xor_byte, dec_bool, dec_bool ? "HAS_ARRAY" : "NULL_ARRAY");

  if (!dec_bool) {
    XELOGW("DC3:RCS DIAG: root array bool is FALSE — DTB says no DataArray! "
           "Encryption seed may be wrong.");
    ppc_context->r[3] = 0;
    return;
  }

  // Step 3: Decrypt 2-byte short (node count / size)
  uint8_t enc_size[2] = {p[0], p[1]};
  uint8_t xor_size[2];
  xor_size[0] = static_cast<uint8_t>(rng.Int());
  xor_size[1] = static_cast<uint8_t>(rng.Int());
  uint8_t dec_size[2] = {static_cast<uint8_t>(enc_size[0] ^ xor_size[0]),
                         static_cast<uint8_t>(enc_size[1] ^ xor_size[1])};
  int16_t node_count;
  if (little_endian) {
    node_count = static_cast<int16_t>(dec_size[0] | (dec_size[1] << 8));
  } else {
    node_count = static_cast<int16_t>((dec_size[0] << 8) | dec_size[1]);
  }
  p += 2;
  XELOGI("DC3:RCS DIAG: size: enc=[{:02X},{:02X}] xor=[{:02X},{:02X}] "
         "dec=[{:02X},{:02X}] node_count={}",
         enc_size[0], enc_size[1], xor_size[0], xor_size[1],
         dec_size[0], dec_size[1], node_count);

  // Step 4: Decrypt 2-byte short (mLine)
  uint8_t enc_line[2] = {p[0], p[1]};
  uint8_t xor_line[2];
  xor_line[0] = static_cast<uint8_t>(rng.Int());
  xor_line[1] = static_cast<uint8_t>(rng.Int());
  uint8_t dec_line[2] = {static_cast<uint8_t>(enc_line[0] ^ xor_line[0]),
                         static_cast<uint8_t>(enc_line[1] ^ xor_line[1])};
  int16_t line;
  if (little_endian) {
    line = static_cast<int16_t>(dec_line[0] | (dec_line[1] << 8));
  } else {
    line = static_cast<int16_t>((dec_line[0] << 8) | dec_line[1]);
  }
  p += 2;
  XELOGI("DC3:RCS DIAG: line: enc=[{:02X},{:02X}] dec=[{:02X},{:02X}] line={}",
         enc_line[0], enc_line[1], dec_line[0], dec_line[1], line);

  // Step 5: Decrypt 2-byte short (mDeprecated) — was 1-byte bool in some versions
  uint8_t enc_dep[2] = {p[0], p[1]};
  uint8_t xor_dep[2];
  xor_dep[0] = static_cast<uint8_t>(rng.Int());
  xor_dep[1] = static_cast<uint8_t>(rng.Int());
  uint8_t dec_dep[2] = {static_cast<uint8_t>(enc_dep[0] ^ xor_dep[0]),
                         static_cast<uint8_t>(enc_dep[1] ^ xor_dep[1])};
  int16_t deprecated;
  if (little_endian) {
    deprecated = static_cast<int16_t>(dec_dep[0] | (dec_dep[1] << 8));
  } else {
    deprecated = static_cast<int16_t>((dec_dep[0] << 8) | dec_dep[1]);
  }
  p += 2;
  XELOGI("DC3:RCS DIAG: deprecated: enc=[{:02X},{:02X}] dec=[{:02X},{:02X}] val={}",
         enc_dep[0], enc_dep[1], dec_dep[0], dec_dep[1], deprecated);

  // Step 6: Decrypt first few node type values to see if they look valid
  for (int n = 0; n < std::min(static_cast<int>(node_count), 4); ++n) {
    if (p + 4 > buf + size) break;
    uint8_t enc_type[4] = {p[0], p[1], p[2], p[3]};
    uint8_t xor_type[4];
    for (int b = 0; b < 4; b++)
      xor_type[b] = static_cast<uint8_t>(rng.Int());
    uint8_t dec_type[4];
    for (int b = 0; b < 4; b++)
      dec_type[b] = enc_type[b] ^ xor_type[b];
    int32_t type;
    if (little_endian) {
      type = dec_type[0] | (dec_type[1] << 8) | (dec_type[2] << 16) | (dec_type[3] << 24);
    } else {
      type = (dec_type[0] << 24) | (dec_type[1] << 16) | (dec_type[2] << 8) | dec_type[3];
    }
    XELOGI("DC3:RCS DIAG: node[{}] type_enc=[{:02X}{:02X}{:02X}{:02X}] "
           "type_dec=[{:02X}{:02X}{:02X}{:02X}] type={}",
           n, enc_type[0], enc_type[1], enc_type[2], enc_type[3],
           dec_type[0], dec_type[1], dec_type[2], dec_type[3], type);
    p += 4;
    // Skip the value bytes — we don't know the size without full parsing
    // Just log the next 4 raw bytes for context
    if (p + 4 <= buf + size) {
      uint8_t next[4] = {p[0], p[1], p[2], p[3]};
      // Don't advance rng or p — just show raw bytes
      XELOGI("DC3:RCS DIAG: node[{}] next_raw=[{:02X}{:02X}{:02X}{:02X}]",
             n, next[0], next[1], next[2], next[3]);
    }
    break;  // Only decode first node type fully (value size varies)
  }

  XELOGI("DC3:RCS DIAG: summary: seed={} bool={} size={} line={} deprecated={} "
         "— {} returning nullptr to let game use fallback path",
         seed, dec_bool, node_count, line, deprecated, filename);

  // Return nullptr — the caller (DataReadFile) will handle it.
  // gSystemConfig will get nullptr which triggers MILO_ASSERT (handled).
  ppc_context->r[3] = 0;
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

// Extract a 32-bit address from the first lis+addi/ori pair in PPC code.
// Used to discover BSS variable addresses from their dynamic initializer
// functions (e.g. ??__EgConditional@@YAXXZ loads the address of gConditional).
uint32_t ExtractPpcLisAddiAddr(Memory* memory, uint32_t fn_addr,
                                int max_insns = 12) {
  if (!fn_addr) return 0;
  auto* base = memory->TranslateVirtual<uint8_t*>(fn_addr);
  if (!base) return 0;
  for (int i = 0; i < max_insns; ++i) {
    uint32_t insn = xe::load_and_swap<uint32_t>(base + i * 4);
    uint32_t opcode = (insn >> 26) & 0x3F;
    if (opcode == 15) {  // addis (lis when rA=0)
      uint32_t rD = (insn >> 21) & 0x1F;
      uint32_t rA = (insn >> 16) & 0x1F;
      if (rA != 0) continue;  // Must be lis (rA=0)
      int16_t simm = static_cast<int16_t>(insn & 0xFFFF);
      uint32_t hi = static_cast<uint32_t>(static_cast<int32_t>(simm)) << 16;
      // Look ahead for addi rD,rD,lo or ori rD,rD,lo
      for (int j = i + 1; j < max_insns && j < i + 4; ++j) {
        uint32_t insn2 = xe::load_and_swap<uint32_t>(base + j * 4);
        uint32_t op2 = (insn2 >> 26) & 0x3F;
        uint32_t rD2 = (insn2 >> 21) & 0x1F;
        uint32_t rA2 = (insn2 >> 16) & 0x1F;
        if (rA2 == rD) {  // source register of addi/ori matches lis register
          if (op2 == 14) {  // addi
            int16_t lo = static_cast<int16_t>(insn2 & 0xFFFF);
            return hi + static_cast<uint32_t>(static_cast<int32_t>(lo));
          } else if (op2 == 24) {  // ori
            return hi | (insn2 & 0xFFFF);
          }
        }
      }
    }
  }
  return 0;
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
    if (str_count <= 200 || (str_count % 500) == 0) {
      XELOGI(
          "DC3: _output_l string #{} FILE={:08X} flags={:08X} cnt={} fmt='{}' "
          "arg={:08X} -> {}",
          str_count, file_ptr, flags, dst_cnt, fmt, arg_ptr, count);
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

// --------------------------------------------------------------------------
// Global address split-brain sync
// --------------------------------------------------------------------------
// The decomp linker places globals at different addresses than the original
// binary.  Original PPC code (CDRead, ArkFilesInit, BlockMgr, etc.) has
// hardcoded addresses baked in.  After decomp code stores a global (e.g.
// TheArchive) at the decomp address, we must copy it to the original binary
// address so that original code can find it.
//
// This is done lazily: on each allocation, we check if TheArchive has been
// stored and sync it.  The check is a single memory read + comparison per
// call, which is negligible overhead.
static bool s_globals_synced = false;
static void Dc3SyncGlobalsIfNeeded(xe::Memory* memory) {
  if (s_globals_synced) return;
  // Check TheArchive at decomp address
  auto* the_archive_mem =
      memory->TranslateVirtual<uint8_t*>(kAddr.the_archive);
  if (!the_archive_mem) return;
  uint32_t archive_val = xe::load_and_swap<uint32_t>(the_archive_mem);
  if (archive_val == 0) return;  // not yet stored by ArchiveInit
  // Sync TheArchive to original binary address
  auto* orig_mem =
      memory->TranslateVirtual<uint8_t*>(kAddr.the_archive_orig);
  if (orig_mem) {
    xe::store_and_swap<uint32_t>(orig_mem, archive_val);
    XELOGI(
        "DC3: Synced TheArchive={:08X} from decomp {:08X} -> original {:08X}",
        archive_val, kAddr.the_archive, kAddr.the_archive_orig);
  }
  s_globals_synced = true;
}

// Replace the game's broken memory allocator functions with SystemHeapAlloc.
// The /FORCE:MULTIPLE linker merged link_glue.obj stubs over the real
// MemMgr.obj implementations, so operator new, MemAlloc, PoolAlloc, etc.
// all point to wrong function bodies.  For example, operator new at
// 0x83406314 is actually the epilogue of MemPrintOverview (ld r31; blr),
// which returns the size argument as a "pointer".  This breaks all
// allocations: new Rand2() fails → DTB decryption broken → config empty →
// MemInit can't configure heaps → everything downstream fails.
void RegisterDc3MemAllocBootstrap(const Dc3HackContext& ctx,
                                  Dc3HackApplyResult& result) {
  if (!ctx.processor || !ctx.is_decomp_layout) {
    result.skipped++;
    return;
  }

  // Initialize the pool allocator for small guest allocations.
  g_dc3_pool.Init(ctx.memory);

  // Minimum valid guest address — anything below this from a free() call
  // is a garbage pointer from the broken operator new (returns sizeof).
  constexpr uint32_t kMinValidPtr = 0x10000;

  // operator new: void* operator new(unsigned int size)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      uint32_t size = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t ptr = g_dc3_pool.Alloc(size);
      if (!ptr && size > 0) {
        ptr = memory->SystemHeapAlloc(size, 0x10);
        if (ptr) {
          auto* dest = memory->TranslateVirtual<uint8_t*>(ptr);
          if (dest) std::memset(dest, 0, size);
        }
      }
      // Lazy sync: copy globals from decomp addresses to original binary
      // addresses once they've been stored (e.g. TheArchive after ArchiveInit).
      Dc3SyncGlobalsIfNeeded(memory);
      static uint32_t s_new_count = 0;
      ++s_new_count;
      if (s_new_count <= 50 || (s_new_count % 5000) == 0) {
        XELOGI("DC3: op_new[{}]: size={} -> {:08X} (LR={:08X})",
               s_new_count, size, ptr, lr);
      }
      ppc_context->r[3] = ptr;
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.operator_new, handler,
                                                 "DC3:op_new(pool)");
    result.applied++;
  }

  // MemAlloc: void* MemAlloc(int size, const char* file, int line,
  //                          const char* name, int align)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      int32_t size = static_cast<int32_t>(ppc_context->r[3]);
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t ptr = 0;
      if (size > 0) {
        ptr = g_dc3_pool.Alloc(static_cast<uint32_t>(size));
        if (!ptr) {
          ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(size), 0x10);
          if (ptr) {
            auto* dest = memory->TranslateVirtual<uint8_t*>(ptr);
            if (dest) std::memset(dest, 0, size);
          }
        }
      }
      static uint32_t s_alloc_count = 0;
      ++s_alloc_count;
      if (s_alloc_count <= 50 || (s_alloc_count % 5000) == 0) {
        XELOGI("DC3: MemAlloc[{}]: size={} -> {:08X} (LR={:08X})",
               s_alloc_count, size, ptr, lr);
      }
      ppc_context->r[3] = ptr;
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.mem_alloc, handler,
                                                 "DC3:MemAlloc(pool)");
    result.applied++;
  }

  // PoolAlloc: void* PoolAlloc(int size, int pool_size, const char* file,
  //                            int line, const char* name)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      int32_t size = static_cast<int32_t>(ppc_context->r[3]);
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t ptr = 0;
      if (size > 0) {
        ptr = g_dc3_pool.Alloc(static_cast<uint32_t>(size));
        if (!ptr) {
          ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(size), 0x10);
          if (ptr) {
            auto* dest = memory->TranslateVirtual<uint8_t*>(ptr);
            if (dest) std::memset(dest, 0, size);
          }
        }
      }
      static uint32_t s_pool_count = 0;
      ++s_pool_count;
      if (s_pool_count <= 50 || (s_pool_count % 5000) == 0) {
        XELOGI("DC3: PoolAlloc[{}]: size={} -> {:08X} (LR={:08X})",
               s_pool_count, size, ptr, lr);
      }
      ppc_context->r[3] = ptr;
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.pool_alloc, handler,
                                                 "DC3:PoolAlloc(pool)");
    result.applied++;
  }

  // MemOrPoolAlloc: void* MemOrPoolAlloc(int size, const char* file,
  //                                      int line, const char* name)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      int32_t size = static_cast<int32_t>(ppc_context->r[3]);
      uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
      uint32_t ptr = 0;
      if (size > 0) {
        ptr = g_dc3_pool.Alloc(static_cast<uint32_t>(size));
        if (!ptr) {
          ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(size), 0x10);
          if (ptr) {
            auto* dest = memory->TranslateVirtual<uint8_t*>(ptr);
            if (dest) std::memset(dest, 0, size);
          }
        }
      }
      static uint32_t s_mpool_count = 0;
      ++s_mpool_count;
      if (s_mpool_count <= 20 || (s_mpool_count % 5000) == 0) {
        XELOGI("DC3: MemOrPoolAlloc[{}]: size={} -> {:08X} (LR={:08X})",
               s_mpool_count, size, ptr, lr);
      }
      ppc_context->r[3] = ptr;
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.mem_or_pool_alloc, handler,
                                                 "DC3:MemOrPoolAlloc(pool)");
    result.applied++;
  }

  // MemFree: void MemFree(void* ptr, const char* file, int line,
  //                       const char* name)
  // Pool allocations are not individually freed — skip SystemHeapFree
  // for addresses within pool chunks.
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      uint32_t ptr = static_cast<uint32_t>(ppc_context->r[3]);
      // Only free addresses in v00000000 heap range (where SystemHeapAlloc puts them).
      // Guest-heap pointers (>=0x40000000) were not allocated by us.
      if (ptr >= kMinValidPtr && ptr < 0x40000000 &&
          !g_dc3_pool.IsPoolAddress(ptr)) {
        memory->SystemHeapFree(ptr);
      }
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.mem_free, handler,
                                                 "DC3:MemFree(pool)");
    result.applied++;
  }

  // PoolFree: void PoolFree(int size, void* ptr, ...)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      uint32_t ptr = static_cast<uint32_t>(ppc_context->r[4]);
      if (ptr >= kMinValidPtr && ptr < 0x40000000 &&
          !g_dc3_pool.IsPoolAddress(ptr)) {
        memory->SystemHeapFree(ptr);
      }
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.pool_free, handler,
                                                 "DC3:PoolFree(pool)");
    result.applied++;
  }

  // MemOrPoolFree: void MemOrPoolFree(int size, void* ptr, ...)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      uint32_t ptr = static_cast<uint32_t>(ppc_context->r[4]);
      if (ptr >= kMinValidPtr && ptr < 0x40000000 &&
          !g_dc3_pool.IsPoolAddress(ptr)) {
        memory->SystemHeapFree(ptr);
      }
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.mem_or_pool_free, handler,
                                                 "DC3:MemOrPoolFree(pool)");
    result.applied++;
  }

  // operator delete: void operator delete(void* ptr)
  {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState* kernel_state) {
      if (!ppc_context || !kernel_state) return;
      auto* memory = kernel_state->memory();
      if (!memory) return;
      uint32_t ptr = static_cast<uint32_t>(ppc_context->r[3]);
      if (ptr >= kMinValidPtr && ptr < 0x40000000 &&
          !g_dc3_pool.IsPoolAddress(ptr)) {
        memory->SystemHeapFree(ptr);
      }
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.operator_delete, handler,
                                                 "DC3:op_delete(pool)");
    result.applied++;
  }

  XELOGI("DC3: Replaced game memory manager with pool allocator "
         "(pool: {}B max, {}MB chunks; "
         "op_new={:08X} MemAlloc={:08X} PoolAlloc={:08X} MemOrPoolAlloc={:08X} "
         "MemFree={:08X} PoolFree={:08X} MemOrPoolFree={:08X} op_delete={:08X})",
         Dc3GuestPool::kPoolMaxSize, Dc3GuestPool::kChunkSize / (1024 * 1024),
         kAddr.operator_new, kAddr.mem_alloc, kAddr.pool_alloc,
         kAddr.mem_or_pool_alloc, kAddr.mem_free, kAddr.pool_free,
         kAddr.mem_or_pool_free, kAddr.operator_delete);

  // Rand2::Rand2(int) — NO host override.
  // Native constructor and Int() both work correctly.
  // Previous host override was at stale address (inside _woutput_s_l) and
  // was suspected of corrupting text formatting, but removing it shows
  // DTB garbling persists — root cause is elsewhere.

  // Rand2::Int() — NO host override.
  // Park-Miller PRNG is pure integer arithmetic; native JIT handles it fine.
  // The host override was catastrophically slow (~350 calls/sec vs millions
  // natively) due to JIT-to-host transition overhead per call.
  // The .hdr file is 333KB = ~333K Rand2::Int calls for decryption.

  // BinStream::Read and ReadEndian overrides REMOVED.
  // The gDataArrayConditional sentinel fix (below) was the real fix for
  // config parsing.  The JIT-compiled BinStream::Read works correctly
  // with the Rand2 host overrides handling XOR decryption PRNG.
  // The BinStream overrides added ~0ms overhead (the 15-second config
  // load is from VFS I/O for included DTB files, not override overhead).

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
    // One-time check of gConditional list and gSystemConfig DataArray
    static bool checked_cond = false;
    if (!checked_cond && g_system_config != 0) {
      checked_cond = true;
      auto* da = memory->TranslateVirtual<uint8_t*>(g_system_config);
      if (da) {
        uint32_t nodes_ptr = xe::load_and_swap<uint32_t>(da + 0x0);
        int16_t da_size = xe::load_and_swap<int16_t>(da + 0x8);
        XELOGI("DC3: gSysCfg addr={:08X} nodes={:08X} size={}",
               g_system_config, nodes_ptr, da_size);
      }
      // gConditional list at 0x83B0EF98 (extracted from ctor at 0x825A0D7C)
      constexpr uint32_t kGCondAddr = 0x83B0EF98;
      auto* gc = memory->TranslateVirtual<uint8_t*>(kGCondAddr);
      if (gc) {
        uint32_t m_next = xe::load_and_swap<uint32_t>(gc + 0);
        uint32_t m_prev = xe::load_and_swap<uint32_t>(gc + 4);
        XELOGI("DC3: gConditional@{:08X}: next={:08X} prev={:08X} "
               "(empty if both={:08X})",
               kGCondAddr, m_next, m_prev, kGCondAddr);
      }
      // Scan for STL list<bool> sentinel patterns in BSS:
      // Look for addresses where [addr]=addr (self-pointing _M_next)
      // This finds all properly-initialized empty lists
      XELOGI("DC3: Scanning BSS 83A00000-83B20000 for list sentinels:");
      for (uint32_t scan = 0x83A00000; scan < 0x83B20000; scan += 4) {
        auto* sp = memory->TranslateVirtual<uint8_t*>(scan);
        if (!sp) continue;
        uint32_t v = xe::load_and_swap<uint32_t>(sp);
        if (v == scan) {
          uint32_t v2 = xe::load_and_swap<uint32_t>(sp + 4);
          if (v2 == scan) {
            XELOGI("DC3:   list sentinel at {:08X} (self-pointing)", scan);
          }
        }
      }
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

    // SetupFont literal corruption repair (robust: match on content, not
    // caller LR).  When SystemConfig("rnd", <binary-garbage>) is called, the
    // second argument is a corrupted font name Symbol.  We substitute the
    // correct "font" sub-array from the "rnd" config section.
    if (arr1 != 0 && arr2 == 0 && !s1_bin && s1_repr == "rnd" && s2_bin) {
      // Allocate a tiny guest buffer with "font\0" for the linear scan.
      static uint32_t s_font_str_addr = 0;
      if (!s_font_str_addr) {
        s_font_str_addr = memory->SystemHeapAlloc(8, 4);
        if (s_font_str_addr) {
          auto* dst = memory->TranslateVirtual<char*>(s_font_str_addr);
          if (dst) {
            std::memcpy(dst, "font", 5);  // "font\0"
          }
        }
      }
      uint32_t repaired = 0;
      if (s_font_str_addr) {
        repaired = Dc3FindArrayLinearBySymbol(memory, arr1, s_font_str_addr);
      }
      if (repaired) {
        XELOGW(
            "DC3: SetupFont SystemConfig repair applied: substituted "
            "'font' for binary arg2 {} LR={:08X} -> {:08X}",
            s2_repr, caller_lr, repaired);
        arr2 = repaired;
      } else {
        XELOGW("DC3: SetupFont SystemConfig repair: 'font' not found in 'rnd' "
               "section (arr1={:08X} LR={:08X})",
               arr1, caller_lr);
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
    // Addresses are fallbacks; manifest resolution overrides them.
    {0x82632000, "HolmesClientInit", 0},
    {0x82630C50, "HolmesClientReInit", 0},
    {0x82631C58, "HolmesClientPoll", 0},
    {0x82630B68, "HolmesClientPollInternal", 0},
    {0x826304C0, "HolmesClientInitOpcode", 0},
    {0x83279E80, "HolmesClientTerminate", 0},
    {0x8262FE80, "CanUseHolmes", 0},
    {0x82630220, "UsingHolmes", 0},
    {kAddr.protocol_debug_string, "ProtocolDebugString", 0},
    {0x83279824, "HolmesSetFileShare", 0},
    {0x8262FFC8, "HolmesFileHostName", 0},
    // HolmesFileShare removed from stub list — real code returns gShareName
    // (empty string ""), which is non-null and prevents iRoot assert crash.
    // Previous null stub broke all file I/O in devkit mode.
    {0x82630240, "HolmesResolveIP", 0},
    {0x8262FE20, "BeginCmd", 0},
    {0x83279A5C, "CheckForResponse", 0},
    {0x83279C60, "WaitForAnyResponse", 0},
    {0x826309B0, "EndCmd", 0},
    {0x83279FE8, "CheckReads", 0},
    {0x82630AC0, "CheckInput", 0},
    {0x8327A0CC, "WaitForResponse", 0},
    {0x8327A194, "WaitForReads", 0},
    {0x82630BF0, "HolmesClientPollKeyboard", 0},
    {0x8327A134, "HolmesClientPollJoypad", 0},
    {0x82631318, "HolmesClientOpen", 0},
    {0x82631718, "HolmesClientRead", 0},
    {0x82632328, "HolmesClientReadDone", 0},
    {0x82631580, "HolmesClientWrite", 0},
    {0x826311B0, "HolmesClientTruncate", 0},
    {0x82631B18, "HolmesClientClose", 0},
    {0x82630E28, "HolmesClientGetStat", 0},
    {0x82630D00, "HolmesClientSysExec", 0},
    {0x82630F70, "HolmesClientMkDir", 0},
    {0x82631090, "HolmesClientDelete", 0},
    {0x826DE348, "HolmesClientEnumerate", 0},
    {0x82631D00, "HolmesClientCacheFile", 0},
    {0x831F34C8, "HolmesClientCacheResource", 0},  // not in MAP, fallback stale
    {0x8262FF00, "HolmesToLocal", 0},
    {0x83279DEC, "HolmesFlushStreamBuffer", 0},
    {0x826302A0, "DumpHolmesLog", 0},
    {0x82631878, "HolmesClientStackTrace", 0},
    {0x826319D8, "HolmesClientSendMessage", 0},
    // FileCache crash workaround: GetFileAll iterates gCaches which has
    // corrupt list nodes after 339-unit promotion.  GetFile dereferences a
    // FileCacheEntry with garbled mStr → fault at 0x70000000.
    // During early init the file cache is empty anyway, so return null.
    {0x82675B50, "FileCache::GetFileAll", 0},
    // CharClip::Init crash workaround: AddBones vector reallocation writes
    // to 0x70000000 (stack guard) due to corrupt vector buffer pointer.
    // This is in the CharInit path during App::App, before SystemInit.
    {0x824951D0, "CharClip::Init", 0},
    // ReadError: called by AsyncFileWin::_OpenAsync when CreateFile fails with
    // an error other than FILE_NOT_FOUND/PATH_NOT_FOUND/NOT_READY.  In CD mode,
    // ReadError calls ThePlatformMgr.SetDiskError(kDiskError) which enters an
    // infinite "reinsert disc" retry loop.  In Xenia, cache paths (cache0:\ etc.)
    // return ERROR_INVALID_NAME instead of FILE_NOT_FOUND, triggering the loop.
    // Stubbing ReadError lets the game gracefully handle missing cache files.
    {0x829571F0, "ReadError", 0},
    // CDReadDone: checks GetOverlappedResult on the global OVERLAPPED used by
    // CDRead for async ARK file reads.  Xenia completes all NtReadFile calls
    // synchronously, but the XAPILIB ReadFile wrapper sets OVERLAPPED.Internal
    // to STATUS_PENDING before calling NtReadFile and Xenia writes the result
    // to a separate stack IO_STATUS_BLOCK — so OVERLAPPED.Internal stays
    // PENDING forever.  GetOverlappedResult then returns ERROR_IO_PENDING and
    // CDReadDone returns false, causing ChunkStream::Eof to spin forever.
    // Returning true (li r3,1; blr) is correct because reads are always done.
    {0x826026E0, "CDReadDone", 1},
    // SetFileChecksumData(FileChecksum*, int): called from App::App to populate
    // a global vector<ChecksumData>.  The vector's STL allocator (StlNodeAlloc)
    // triggers _M_insert_overflow_aux which crashes writing to RWDATA during
    // reallocation.  Not needed for boot testing.
    {0x828E2438, "SetFileChecksumData", 0},
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

  // Stub XRegisterThreadNotifyRoutine (guest-linked copy from
  // xregisterthreadnotifyroutine.obj).  This function registers a thread
  // notification callback with the XDK runtime.  It acquires XapiProcessLock
  // via a kernel critical section that deadlocks under Xenia.  _mtinit calls
  // this during CRT startup, blocking the main thread before _cinit/main().
  // Stubbing it (return 0) lets _mtinit complete while preserving CRT lock
  // initialization (_mtinitlocks / _mtinitlocknum).
  {
    const uint32_t addr = kAddr.xregister_thread_notify;
    auto* heap = memory->LookupHeap(addr);
    if (heap) {
      auto* p = memory->TranslateVirtual<uint8_t*>(addr);
      if (p) {
        uint32_t w0 = xe::load_and_swap<uint32_t>(p);
        if (w0 != 0x00000000) {
          heap->Protect(addr, 8, kMemoryProtectRead | kMemoryProtectWrite);
          xe::store_and_swap<uint32_t>(p + 0, 0x38600000);  // li r3, 0
          xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);  // blr
          XELOGI("DC3: Stubbed XRegisterThreadNotifyRoutine at {:08X} "
                 "(li r3,0; blr) — unblocks _mtinit CRT startup",
                 addr);
          result.applied++;
        }
      }
    }
  }

  // Stub SetWind — DC3 has a real implementation that calls Rand::Gaussian on
  // a null Rand object (the wind simulation structure at 0x83A25DC8 is
  // zero-initialized but never constructed).  RB3 stubs SetWind as a no-op.
  // Called from RndWind::Init → Rnd::PreInit during App::App().
  {
    const uint32_t addr = kAddr.set_wind;
    auto* heap = memory->LookupHeap(addr);
    if (heap) {
      auto* p = memory->TranslateVirtual<uint8_t*>(addr);
      if (p) {
        uint32_t w0 = xe::load_and_swap<uint32_t>(p);
        if (w0 != 0x00000000) {
          heap->Protect(addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
          xe::store_and_swap<uint32_t>(p + 0, 0x4E800020);  // blr
          XELOGI("DC3: Stubbed SetWind at {:08X} (blr) — avoids null "
                 "Rand::Gaussian",
                 addr);
          result.applied++;
        }
      }
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

  // ReadCacheStream: optionally return nullptr to prevent corrupt data loading.
  // Was needed before DataReadFile was decomped; now that the decomp has
  // ReadCacheStream + DataReadFile, the guest code should work natively.
  if (cvars::dc3_null_read_cache_stream) {
    auto handler = [](cpu::ppc::PPCContext* ppc_context,
                      kernel::KernelState*) {
      ppc_context->r[3] = 0;  // return nullptr
    };
    ctx.processor->RegisterGuestFunctionOverride(kAddr.rcs_read_cache_stream,
                                                 handler,
                                                 "DC3:ReadCacheStream(null)");
    result.applied++;
    XELOGI("DC3: ReadCacheStream override returns nullptr (--dc3_null_read_cache_stream=true)");
  } else {
    XELOGI("DC3: ReadCacheStream override DISABLED — guest ReadCacheStream runs natively");
  }

  // DataInput diagnostic: check if the flex scanner is getting any input.
  // DataInput(void* buf, int size) returns bytes read; 0 = EOF.
  // This is a DESTRUCTIVE override — the original DataInput is replaced.
  // We need to forward to gBinStream->Read, but first we need to find
  // gBinStream's address in the original DataFile.obj.
  // Strategy: read the first few instructions of the original DataInput
  // to extract the gBinStream address (it will be a lis/lwz or similar pattern).
  if (ctx.processor && kAddr.data_input) {
    auto* di_code = memory->TranslateVirtual<uint8_t*>(kAddr.data_input);
    if (di_code) {
      // Dump first 8 instructions of DataInput to find gBinStream reference
      XELOGI("DC3: DataInput at {:08X} first 8 instructions:", kAddr.data_input);
      for (int i = 0; i < 8; ++i) {
        uint32_t insn = xe::load_and_swap<uint32_t>(di_code + i * 4);
        XELOGI("DC3: DataInput+{:02X}: {:08X}", i * 4, insn);
      }
      // Try to extract gBinStream address from lis/ori or lis/lwz pattern
      // Pattern: lis rX, hi16 ; lwz rY, lo16(rX)
      uint32_t g_bin_stream_addr = 0;
      for (int i = 0; i < 7; ++i) {
        uint32_t insn0 = xe::load_and_swap<uint32_t>(di_code + i * 4);
        uint32_t insn1 = xe::load_and_swap<uint32_t>(di_code + (i + 1) * 4);
        // lis rX, imm: 001111 XXXXX 00000 IIIIIIIIIIIIIIII = 0x3Cxx0000
        if ((insn0 & 0xFC1F0000) == 0x3C000000) {
          uint32_t hi16 = insn0 & 0xFFFF;
          uint32_t rX = (insn0 >> 21) & 0x1F;
          // lwz rY, lo16(rX): 100000 YYYYY XXXXX DDDDDDDDDDDDDDDD
          if ((insn1 & 0xFC000000) == 0x80000000) {
            uint32_t rX2 = (insn1 >> 16) & 0x1F;
            if (rX2 == rX) {
              int16_t lo16 = static_cast<int16_t>(insn1 & 0xFFFF);
              g_bin_stream_addr = (hi16 << 16) + lo16;
              XELOGI("DC3: DataInput: found gBinStream candidate at {:08X} "
                     "(lis r{},{:04X} + lwz lo16={:04X} at insn+{:02X})",
                     g_bin_stream_addr, rX, hi16,
                     static_cast<uint16_t>(lo16), i * 4);
              break;
            }
          }
        }
      }
    }
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
          // Dump BOTH gHashTable copies (dual /FORCE artifact hypothesis).
          auto dump_ht = [&](const char* label, uint32_t addr) {
            if (auto* h = memory->TranslateVirtual<uint8_t*>(addr)) {
              uint32_t entries = xe::load_and_swap<uint32_t>(h + 0x0);
              uint32_t size = xe::load_and_swap<uint32_t>(h + 0x4);
              uint8_t own = h[0x8];
              uint32_t num = xe::load_and_swap<uint32_t>(h + 0xC);
              XELOGW("DC3: {} @{:08X}: entries={:08X} size={} own={} used={}",
                     label, addr, entries, size, own, num);
            }
          };
          dump_ht("gHashTable(.data)", kAddr.g_hash_table);     // 0x83AE01C4
          dump_ht("gHashTable(.bss)", 0x83AED0FC);              // BSS copy
          // Walk the sFactories red-black tree and dump all entries.
          // STLPort _Rb_tree node layout (PPC big-endian, 24 bytes per node):
          //   +0x00: _M_color  (uint32_t, 0=red, 1=black)
          //   +0x04: _M_parent (uint32_t, ptr to parent node)
          //   +0x08: _M_left   (uint32_t, ptr to left child)
          //   +0x0C: _M_right  (uint32_t, ptr to right child)
          //   +0x10: _M_value  (pair<const Symbol, ObjectFunc*>)
          //     +0x10: Symbol.mStr (uint32_t, ptr to interned string)
          //     +0x14: ObjectFunc* (uint32_t, factory function ptr)
          // Map header (embedded sentinel node + count):
          //   @map+0x00: sentinel._M_color
          //   @map+0x04: sentinel._M_parent = root node
          //   @map+0x08: sentinel._M_left   = leftmost (begin)
          //   @map+0x0C: sentinel._M_right  = rightmost
          //   @map+0x10: _M_node_count
          static bool s_tree_dumped = false;
          if (!s_tree_dumped) {
            s_tree_dumped = true;
            uint32_t sentinel_addr = kAddr.object_factories_map;
            uint32_t root_addr = xe::load_and_swap<uint32_t>(
                memory->TranslateVirtual<uint8_t*>(sentinel_addr) + 0x04);
            uint32_t count = xe::load_and_swap<uint32_t>(
                memory->TranslateVirtual<uint8_t*>(sentinel_addr) + 0x10);
            XELOGW("DC3: sFactories tree walk: root={:08X} count={}", root_addr, count);
            // In-order traversal via stack (iterative).
            std::vector<uint32_t> stack;
            uint32_t cur = root_addr;
            int visited = 0;
            while ((cur && cur != sentinel_addr) || !stack.empty()) {
              while (cur && cur != sentinel_addr) {
                stack.push_back(cur);
                auto* n = memory->TranslateVirtual<uint8_t*>(cur);
                cur = n ? xe::load_and_swap<uint32_t>(n + 0x08) : 0; // left
              }
              if (stack.empty()) break;
              cur = stack.back();
              stack.pop_back();
              auto* n = memory->TranslateVirtual<uint8_t*>(cur);
              if (n) {
                uint32_t sym_ptr = xe::load_and_swap<uint32_t>(n + 0x10);
                uint32_t factory = xe::load_and_swap<uint32_t>(n + 0x14);
                char tmp[64] = {};
                bool sym_bin = false;
                std::string desc = Dc3DescribeGuestSymbol(memory, sym_ptr, tmp,
                                                           sizeof(tmp), &sym_bin);
                if (visited < 60) {
                  XELOGW("DC3: sFactories[{:02d}] node={:08X} sym={:08X}({}) factory={:08X}",
                         visited, cur, sym_ptr, desc, factory);
                }
                cur = xe::load_and_swap<uint32_t>(n + 0x0C); // right
              } else {
                break;
              }
              visited++;
              if (visited > 200) break; // safety
            }
            XELOGW("DC3: sFactories tree walk complete: visited {}/{}", visited, count);
          }
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
            // One-time backtrace for "is not Symbol" to find the per-frame caller
            // Dump gHashTable state when mOwnEntries assertion fires.
            if (std::strstr(msg, "mOwnEntries")) {
              const uint32_t kGHT = kAddr.g_hash_table;
              auto* ht = memory->TranslateVirtual<uint8_t*>(kGHT);
              if (ht) {
                uint32_t e = xe::load_and_swap<uint32_t>(ht + 0x00);
                uint32_t s = xe::load_and_swap<uint32_t>(ht + 0x04);
                uint32_t n = xe::load_and_swap<uint32_t>(ht + 0x0C);
                XELOGE("DC3: gHashTable @{:08X}: mEntries={:08X} mSize={} "
                       "mOwnEntries_byte={:02X} mNumEntries={} "
                       "raw[0..23]={:02X}{:02X}{:02X}{:02X} "
                       "{:02X}{:02X}{:02X}{:02X} "
                       "{:02X}{:02X}{:02X}{:02X} "
                       "{:02X}{:02X}{:02X}{:02X} "
                       "{:02X}{:02X}{:02X}{:02X} "
                       "{:02X}{:02X}{:02X}{:02X}",
                       kGHT, e, s, ht[0x08], n,
                       ht[0], ht[1], ht[2], ht[3],
                       ht[4], ht[5], ht[6], ht[7],
                       ht[8], ht[9], ht[10], ht[11],
                       ht[12], ht[13], ht[14], ht[15],
                       ht[16], ht[17], ht[18], ht[19],
                       ht[20], ht[21], ht[22], ht[23]);
              }
            }
            // One-shot gSystemConfig dump on first "Couldn't find" to see
            // what ReadSystemConfig actually returned.
            if (std::strstr(msg, "Couldn't find") && std::strstr(msg, "in array")) {
              static bool s_config_dumped = false;
              if (!s_config_dumped) {
                s_config_dumped = true;
                // gSystemConfig at kAddr.g_system_config (set by code cave)
                const uint32_t kGSC = kAddr.g_system_config;
                // Also check gUsingCD and gBinStream at runtime
                const uint32_t kUCD = kAddr.g_using_cd;
                auto* ucd_mem = memory->TranslateVirtual<uint8_t*>(kUCD);
                if (ucd_mem) {
                  uint32_t ucd_val = xe::load_and_swap<uint32_t>(ucd_mem);
                  XELOGE("DC3: gUsingCD@{:08X} = {} at first config lookup", kUCD, ucd_val);
                }
                // gBinStream (original DataFile.obj static) at 0x83CA2FC4
                const uint32_t kGBS = 0x83CA2FC4;
                auto* gbs_mem = memory->TranslateVirtual<uint8_t*>(kGBS);
                if (gbs_mem) {
                  uint32_t gbs_val = xe::load_and_swap<uint32_t>(gbs_mem);
                  XELOGE("DC3: gBinStream@{:08X} = {:08X} at first config lookup",
                         kGBS, gbs_val);
                }
                auto* gsc = memory->TranslateVirtual<uint8_t*>(kGSC);
                if (gsc) {
                  uint32_t da_ptr = xe::load_and_swap<uint32_t>(gsc);
                  XELOGE("DC3: gSystemConfig@{:08X} = {:08X}", kGSC, da_ptr);
                  if (da_ptr && da_ptr < 0xF0000000) {
                    auto* da = memory->TranslateVirtual<uint8_t*>(da_ptr);
                    if (da) {
                      // DataArray layout: mNodes@+0, mFile@+4, mSize@+8(short),
                      //   mRefs@+A(short), mLine@+C(short), mDeprecated@+E(short)
                      uint32_t nodes = xe::load_and_swap<uint32_t>(da + 0x0);
                      uint32_t file = xe::load_and_swap<uint32_t>(da + 0x4);
                      int16_t sz = xe::load_and_swap<int16_t>(da + 0x8);
                      int16_t refs = xe::load_and_swap<int16_t>(da + 0xA);
                      int16_t line = xe::load_and_swap<int16_t>(da + 0xC);
                      const char* file_str = "<null>";
                      if (file && file < 0xF0000000) {
                        if (auto* f = memory->TranslateVirtual<const char*>(file))
                          file_str = f;
                      }
                      XELOGE("DC3: gSystemConfig DataArray: nodes={:08X} file='{}' "
                             "size={} refs={} line={}",
                             nodes, file_str, sz, refs, line);
                      // Dump first 8 DataNode entries (each 8 bytes: type@+0, value@+4)
                      if (nodes && nodes < 0xF0000000 && sz > 0) {
                        auto* n = memory->TranslateVirtual<uint8_t*>(nodes);
                        if (n) {
                          int count = std::min((int)sz, 8);
                          for (int i = 0; i < count; ++i) {
                            uint32_t type = xe::load_and_swap<uint32_t>(n + i * 8);
                            uint32_t val = xe::load_and_swap<uint32_t>(n + i * 8 + 4);
                            // Type 5 = Symbol, 16 = Array, 18 = String
                            const char* type_name = "?";
                            switch (type) {
                              case 0: type_name = "Int"; break;
                              case 1: type_name = "Float"; break;
                              case 2: type_name = "Var"; break;
                              case 5: type_name = "Symbol"; break;
                              case 16: type_name = "Array"; break;
                              case 17: type_name = "Command"; break;
                              case 18: type_name = "String"; break;
                              case 32: type_name = "Define"; break;
                              case 33: type_name = "Include"; break;
                            }
                            std::string val_desc;
                            if (type == 5 && val && val < 0xF0000000) {
                              if (auto* s = memory->TranslateVirtual<const char*>(val))
                                val_desc = std::string("'") + s + "'";
                            } else if (type == 16 && val && val < 0xF0000000) {
                              // val is DataArray*, peek its size
                              if (auto* sub = memory->TranslateVirtual<uint8_t*>(val)) {
                                int16_t sub_sz = xe::load_and_swap<int16_t>(sub + 0x8);
                                val_desc = "size=" + std::to_string(sub_sz);
                                // Peek first node of sub-array for symbol name
                                uint32_t sub_nodes = xe::load_and_swap<uint32_t>(sub + 0x0);
                                if (sub_nodes && sub_nodes < 0xF0000000) {
                                  auto* sn = memory->TranslateVirtual<uint8_t*>(sub_nodes);
                                  if (sn) {
                                    uint32_t st = xe::load_and_swap<uint32_t>(sn + 0);
                                    uint32_t sv = xe::load_and_swap<uint32_t>(sn + 4);
                                    if (st == 5 && sv && sv < 0xF0000000) {
                                      if (auto* ss = memory->TranslateVirtual<const char*>(sv))
                                        val_desc += std::string(" first='") + ss + "'";
                                    }
                                  }
                                }
                              }
                            }
                            XELOGE("DC3: gSC node[{}]: type={} ({}) val={:08X} {}",
                                   i, type, type_name, val,
                                   val_desc.empty() ? "" : val_desc.c_str());
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
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

  // =========================================================================
  // Host-side factory system (bypass corrupted guest std::map tree).
  // =========================================================================
  // The guest sFactories map (std::map<Symbol, ObjectFunc*>) becomes corrupted
  // during Rnd::PreInit init calls, causing find() to fail for many classes.
  // We intercept RegisterFactory to capture all registrations in a host-side
  // map keyed by string name, then intercept NewObject to use the host map
  // for lookup and call the factory function via the JIT.
  if (ctx.processor && ctx.is_decomp_layout) {
    // Shared host-side factory map: class name string → guest factory address.
    static std::unordered_map<std::string, uint32_t> s_host_factories;
    static std::mutex s_factory_mutex;

    // Override Hmx::Object::RegisterFactory(Symbol sym, ObjectFunc* func).
    // PPC ABI: r3 = Symbol (struct with single member const char* mStr,
    //          passed by value in r3 as the mStr pointer).
    //          r4 = ObjectFunc* (function pointer).
    // Returns: void.
    const uint32_t kRegisterFactory = kAddr.register_factory;
    auto rf_handler = [](cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
      uint32_t sym_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t factory_fn = static_cast<uint32_t>(ppc_context->r[4]);
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      std::string name;
      if (memory && sym_ptr && sym_ptr < 0xF0000000) {
        if (auto* s = memory->TranslateVirtual<const char*>(sym_ptr)) {
          name = s;
        }
      }
      {
        std::lock_guard<std::mutex> lock(s_factory_mutex);
        bool replaced = s_host_factories.count(name) > 0;
        s_host_factories[name] = factory_fn;
        static int rf_count = 0;
        if (++rf_count <= 80 || (rf_count % 100) == 0) {
          XELOGW("DC3: RegisterFactory[{}] '{}' -> {:08X}{}",
                 rf_count, name, factory_fn, replaced ? " (replaced)" : "");
        }
      }
      // Don't forward — the guest map is corrupt and useless.
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kRegisterFactory, rf_handler, "DC3:RegisterFactory(host)");
    XELOGI("DC3: Registered host-side RegisterFactory override at {:08X}",
           kRegisterFactory);

    // Override Hmx::Object::NewObject(Symbol name).
    // PPC ABI: r3 = Symbol (mStr pointer). Returns: Hmx::Object* in r3.
    const uint32_t kNewObject = kAddr.new_object;
    auto no_handler = [](cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
      uint32_t sym_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      std::string name;
      if (memory && sym_ptr && sym_ptr < 0xF0000000) {
        if (auto* s = memory->TranslateVirtual<const char*>(sym_ptr)) {
          name = s;
        }
      }
      uint32_t factory_fn = 0;
      {
        std::lock_guard<std::mutex> lock(s_factory_mutex);
        auto it = s_host_factories.find(name);
        if (it != s_host_factories.end()) {
          factory_fn = it->second;
        }
      }
      if (!factory_fn) {
        static int miss_count = 0;
        if (++miss_count <= 20 || (miss_count % 100) == 0) {
          uint32_t lr = static_cast<uint32_t>(ppc_context->lr);
          XELOGW("DC3: NewObject('{}') MISS — no factory registered LR={:08X}",
                 name, lr);
        }
        ppc_context->r[3] = 0;
        return;
      }
      // Call the factory function via the JIT.
      // Factory signature: Hmx::Object* NewObject(void) — no args, returns ptr.
      auto* processor = kernel_state->processor();
      auto* fn = processor->QueryFunction(factory_fn);
      if (!fn) {
        fn = processor->ResolveFunction(factory_fn);
      }
      if (fn) {
        // Save/restore volatile registers that NewObject's caller expects.
        uint64_t saved_lr = ppc_context->lr;
        auto* thread_state = ppc_context->thread_state;
        // Call the factory. It will set r3 to the new object pointer.
        fn->Call(thread_state, static_cast<uint32_t>(saved_lr));
        // r3 now has the return value (new Object*). Restore LR.
        ppc_context->lr = saved_lr;
        static int ok_count = 0;
        if (++ok_count <= 20 || (ok_count % 500) == 0) {
          uint32_t result = static_cast<uint32_t>(ppc_context->r[3]);
          auto* memory = kernel_state ? kernel_state->memory() : nullptr;
          uint32_t vtbl = 0;
          if (memory && result && result < 0xF0000000) {
            if (auto* obj = memory->TranslateVirtual<uint8_t*>(result)) {
              vtbl = xe::load_and_swap<uint32_t>(obj + 0x00);
            }
          }
          XELOGW("DC3: NewObject('{}') -> {:08X} vtbl={:08X} via factory {:08X} [{}]",
                 name, result, vtbl, factory_fn, ok_count);
        }
      } else {
        XELOGW("DC3: NewObject('{}') factory {:08X} could not be resolved!",
               name, factory_fn);
        ppc_context->r[3] = 0;
      }
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kNewObject, no_handler, "DC3:NewObject(host)");
    XELOGI("DC3: Registered host-side NewObject override at {:08X}", kNewObject);

    // Override __RTDynamicCast to diagnose + fix RTTI failures.
    // PPC ABI: r3=inptr, r4=VfDelta, r5=SrcType, r6=TargetType, r7=isReference
    // Returns: adjusted pointer or NULL.
    // On Xbox 360 (MSVC PPC), type_info has a mangled name at offset +0x08.
    const uint32_t kRTDynamicCast = kAddr.rt_dynamic_cast;
    auto rtdc_handler = [](cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
      uint32_t inptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t src_type = static_cast<uint32_t>(ppc_context->r[5]);
      uint32_t tgt_type = static_cast<uint32_t>(ppc_context->r[6]);
      uint32_t is_ref = static_cast<uint32_t>(ppc_context->r[7]);
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;

      // Read MSVC type_info mangled names (offset +0x08 on PPC Xbox 360).
      auto read_type_name = [&](uint32_t type_info_addr) -> std::string {
        if (!memory || !type_info_addr || type_info_addr >= 0xF0000000)
          return "<null>";
        // type_info layout: +0x00 vtable, +0x04 spare, +0x08 name (char[])
        if (auto* ti = memory->TranslateVirtual<const char*>(type_info_addr + 0x08))
          return ti;
        return "<unreadable>";
      };
      std::string src_name = read_type_name(src_type);
      std::string tgt_name = read_type_name(tgt_type);

      // If inptr is null, dynamic_cast<T*>(nullptr) = nullptr.
      if (!inptr) {
        ppc_context->r[3] = 0;
        return;
      }

      // For pointer casts in the Milo class hierarchy, just return inptr.
      // Milo uses single inheritance for all factory classes, so no base
      // offset adjustment is needed.  The vtable is already correct.
      // This bypasses broken RTTI from /FORCE linker duplicate type_info.
      if (!is_ref) {
        static int rtdc_count = 0;
        if (++rtdc_count <= 30 || (rtdc_count % 2000) == 0) {
          XELOGW("DC3: __RTDynamicCast[{}] {:08X} src={} tgt={} -> pass-through",
                 rtdc_count, inptr, src_name, tgt_name);
        }
        ppc_context->r[3] = inptr;
        return;
      }

      // For reference casts, also pass through (bad_cast on fail, but we
      // trust the factory system).
      ppc_context->r[3] = inptr;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kRTDynamicCast, rtdc_handler, "DC3:__RTDynamicCast(passthrough)");
    XELOGI("DC3: Registered __RTDynamicCast pass-through at {:08X}", kRTDynamicCast);
  }

  // Host-side SetDirty_Force: prevents infinite loops from corrupted
  // std::list mChildren (same /FORCE linker corruption as sFactories).
  if (ctx.processor && ctx.is_decomp_layout) {
    const uint32_t kSetDirtyForce = kAddr.set_dirty_force;
    auto sdf_handler = [](cpu::ppc::PPCContext* ppc_context,
                          kernel::KernelState* kernel_state) {
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      if (!memory) return;
      uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      HostSetDirtyForce(memory, this_ptr, 0);
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kSetDirtyForce, sdf_handler, "DC3:SetDirty_Force(cycle-safe)");
    XELOGI("DC3: Registered SetDirty_Force cycle-safe override at {:08X}",
           kSetDirtyForce);
  }

  // RndMat::LoadMetaMaterials — stub to prevent shader compilation hang.
  // With config include expansion working, the game enters a fuller init path
  // through NgRnd::PreInit → Rnd::PreInit → RndMat::Init → LoadMetaMaterials.
  // LoadMetaMaterials calls DirLoader::LoadObjects which triggers async I/O
  // (ChunkStream threads fail on headless) and D3DXShader::CombineMERGEs
  // (infinite shader compilation loop). Return null ObjectDir*.
  {
    const uint32_t kLoadMetaMaterials = kAddr.load_meta_materials;
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, kLoadMetaMaterials, 0,
                       "RndMat::LoadMetaMaterials");
  }

  // Object::SetName null-dir guard.
  // Multiple init paths call SetName(name, ObjectDir::Main()) before sMainDir
  // is initialized (e.g. Watcher::Init, Synth::PreInit paths). When dir is null,
  // MILO_ASSERT(dir) fires but Debug::Fail returns, then dir->FindEntry()
  // dereferences null → bctrl to address 0 → thread hangs forever.
  //
  // PPC patch: replace the assert path with a null-guard branch.
  // Original at +0x40: bne cr6,+0x3C  (skip assert if dir non-null)
  // Original at +0x44: li r11,231     (assert line number)
  // Patched  at +0x40: beq cr6,+0xA8  (if dir null → null-name cleanup path)
  // Patched  at +0x44: b +0x38        (if dir non-null → normal registration)
  {
    const uint32_t kSetName = kAddr.object_set_name;
    const uint32_t kPatchAddr0 = kSetName + 0x40;
    const uint32_t kPatchAddr1 = kSetName + 0x44;
    constexpr uint32_t kExpected0 = 0x409A003C;  // bne cr6,+0x3C
    constexpr uint32_t kExpected1 = 0x396000E7;  // li r11,231
    constexpr uint32_t kPatched0 = 0x419A00A8;   // beq cr6,+0xA8 (→ null cleanup)
    constexpr uint32_t kPatched1 = 0x48000038;   // b +0x38 (→ normal path)

    auto* heap = memory->LookupHeap(kPatchAddr0);
    auto* p0 = memory->TranslateVirtual<uint8_t*>(kPatchAddr0);
    auto* p1 = memory->TranslateVirtual<uint8_t*>(kPatchAddr1);
    if (!heap || !p0 || !p1) {
      XELOGW("DC3: Object::SetName patch skipped (no heap/translate)");
      result.failed++;
    } else {
      uint32_t v0 = xe::load_and_swap<uint32_t>(p0);
      uint32_t v1 = xe::load_and_swap<uint32_t>(p1);
      if (v0 == kExpected0 && v1 == kExpected1) {
        heap->Protect(kPatchAddr0, 8, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(p0, kPatched0);
        xe::store_and_swap<uint32_t>(p1, kPatched1);
        XELOGI("DC3: Patched Object::SetName null-dir guard at {:08X}",
               kSetName);
        result.applied++;
      } else {
        XELOGW("DC3: Object::SetName patch mismatch at {:08X}: "
               "expected {:08X}/{:08X} got {:08X}/{:08X}",
               kSetName, kExpected0, kExpected1, v0, v1);
        result.failed++;
      }
    }
  }

  // CreateAndSetMetaMat no-op: sMetaMaterials is NULL because
  // LoadMetaMaterials() can't load metamaterials.milo without proper
  // file system / config. Skip MetaMaterial setup entirely.
  if (ctx.processor && ctx.is_decomp_layout) {
    const uint32_t kCreateAndSetMetaMat = kAddr.create_and_set_meta_mat;
    auto casemm_handler = [](cpu::ppc::PPCContext* ppc_context,
                             kernel::KernelState* kernel_state) {
      static int skip_count = 0;
      if (++skip_count <= 10) {
        XELOGW("DC3: CreateAndSetMetaMat skip #{} (sMetaMaterials=NULL)",
               skip_count);
      }
      // no-op: just return without creating MetaMaterial
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kCreateAndSetMetaMat, casemm_handler,
        "DC3:CreateAndSetMetaMat(noop)");
    XELOGI("DC3: Registered CreateAndSetMetaMat no-op at {:08X}",
           kCreateAndSetMetaMat);
  }

  // AllocType: switch table corrupted by /FORCE linker, causing infinite loop
  // on case 0 (table byte = 0 → jumps back to function start). Override to
  // return a pointer to "main" string which is already in guest .data.
  if (ctx.processor && ctx.is_decomp_layout) {
    const uint32_t kAllocType = kAddr.alloc_type;
    auto at_handler = [](cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
      // Return pointer to the "main" string already in guest memory.
      // gNullStr points to "" which is safe enough.
      ppc_context->r[3] = kAddr.g_null_str;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kAllocType, at_handler, "DC3:AllocType(fixed)");
    XELOGI("DC3: Registered AllocType fixed override at {:08X}", kAllocType);
  }

  // NgPostProc::RebuildTex: allocates GPU textures for post-processing
  // (velocity buffer, bloom). Blocks on null GPU. Skip entirely.
  if (ctx.processor && ctx.is_decomp_layout) {
    auto ngpp_handler = [](cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
      XELOGW("DC3: NgPostProc::RebuildTex() skipped (no GPU)");
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kAddr.ng_postproc_rebuild_tex, ngpp_handler,
        "DC3:NgPostProc::RebuildTex(noop)");
    XELOGI("DC3: Stubbed NgPostProc::RebuildTex at {:08X}",
           kAddr.ng_postproc_rebuild_tex);
  }

  // NgDOFProc::Init: depth-of-field post-processor, needs GPU textures.
  if (ctx.processor && ctx.is_decomp_layout) {
    auto ngdof_handler = [](cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
      XELOGW("DC3: NgDOFProc::Init() skipped (no GPU)");
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kAddr.ng_dofproc_init, ngdof_handler, "DC3:NgDOFProc::Init(noop)");
    XELOGI("DC3: Stubbed NgDOFProc::Init at {:08X}", kAddr.ng_dofproc_init);
  }

  // DxRnd::Suspend: suspends rendering thread. Can deadlock with null GPU
  // when subsequent init code tries to allocate GPU resources.
  if (ctx.processor && ctx.is_decomp_layout) {
    auto suspend_handler = [](cpu::ppc::PPCContext* ppc_context,
                              kernel::KernelState* kernel_state) {
      XELOGW("DC3: DxRnd::Suspend() skipped (no GPU)");
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kAddr.dxrnd_suspend, suspend_handler, "DC3:DxRnd::Suspend(noop)");
    XELOGI("DC3: Stubbed DxRnd::Suspend at {:08X}", kAddr.dxrnd_suspend);
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
  RegisterDc3MemAllocBootstrap(ctx, result);

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

// Apply XDK SDK function overrides from the manifest.
// These are JIT-level overrides (no guest memory modification) for XDK
// library functions (XAudio2, NUI, Speech) that deadlock or crash when
// their hardware backends aren't available (nop APU, no Kinect).
//
// IMPORTANT: The xdk_overrides map may include shared template instantiations
// (MakeString, STL helpers) that are ICF-merged with game code.  We filter
// to only override XDK-internal class methods by checking for known prefixes
// in the mangled symbol name.
void ApplyXdkOverrides(const Dc3HackContext& ctx,
                       Dc3HackApplyResult& result) {
  if (!ctx.processor || !ctx.xdk_overrides || ctx.xdk_overrides->empty()) {
    return;
  }

  // Only override symbols containing these XDK-internal identifiers.
  // This avoids nopping MakeString templates, STL helpers, etc. that
  // are ICF-merged with game code at the same address.
  auto is_xdk_internal = [](const std::string& name) -> bool {
    // XAudio2 internal classes (CX2Engine, CX2SourceVoice, CX2VoiceBase, etc.)
    if (name.find("CX2") != std::string::npos) return true;
    if (name.find("XAUDIO2") != std::string::npos) return true;
    // NUI/Kinect SDK APIs
    if (name.find("@Nui") != std::string::npos ||
        name.find("Nui") == 0) return true;
    // Speech SDK internals
    if (name.find("@CSp") != std::string::npos) return true;
    if (name.find("NUISPEECH") != std::string::npos) return true;
    // NUI runtime internals
    if (name.find("@NUI@@") != std::string::npos) return true;
    if (name.find("@NuiRuntime") != std::string::npos) return true;
    return false;
  };

  // Single handler for all XDK overrides: return 0 (S_OK for HRESULT,
  // null for pointers, no-op for void).  r3 = return value in PPC ABI.
  auto xdk_nop_handler = [](cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState*) {
    ppc_context->r[3] = 0;
  };

  int applied = 0;
  int skipped = 0;
  for (const auto& [name, addr] : *ctx.xdk_overrides) {
    if (!is_xdk_internal(name)) {
      ++skipped;
      continue;
    }
    ctx.processor->RegisterGuestFunctionOverride(addr, xdk_nop_handler);
    ++applied;
  }

  // Also scan xdk_code_ranges for function prologues that aren't in the
  // symbol map (internal/static XDK functions).
  if (ctx.xdk_code_ranges && !ctx.xdk_code_ranges->empty()) {
    auto* memory = ctx.memory;
    int prologue_stubs = 0;
    for (const auto& range : *ctx.xdk_code_ranges) {
      // Scan for PPC function prologues (stwu r1, -N(r1) = 0x9421xxxx)
      // within the range.
      for (uint32_t pc = range.start; pc < range.end; pc += 4) {
        auto* p = memory->TranslateVirtual<uint8_t*>(pc);
        if (!p) continue;
        uint32_t insn = xe::load_and_swap<uint32_t>(p);
        if ((insn & 0xFFFF0000) == 0x94210000) {
          // Found a function prologue — register override if not already
          // covered by the named overrides.
          bool already_covered = false;
          for (const auto& [n, a] : *ctx.xdk_overrides) {
            if (a == pc) { already_covered = true; break; }
          }
          if (!already_covered) {
            ctx.processor->RegisterGuestFunctionOverride(
                pc, xdk_nop_handler);
            ++prologue_stubs;
          }
        }
      }
    }
    if (prologue_stubs > 0) {
      XELOGI("DC3: XDK prologue scan: {} additional internal functions stubbed",
             prologue_stubs);
      applied += prologue_stubs;
    }
  }

  XELOGI("DC3: Applied {} XDK JIT overrides ({} shared templates skipped)",
         applied, skipped);
  result.applied += applied;
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

  // Make the full PE image writable (split into individual page descriptor
  // ranges to avoid "spanning regions" failure when the range extends past
  // the XEX allocation boundary).
  {
    auto* heap1 = memory->LookupHeap(0x82000000);
    if (heap1) {
      // XEX page descriptors for decomp PE (from build_xex.py):
      //   DESC[0]: RODATA  70 pages  0x82000000-0x82460000
      //   DESC[1]: CODE   236 pages  0x82460000-0x83320000
      //   DESC[2]: DATA    58 pages  0x83320000-0x836C0000
      //   DESC[3]: RODATA  36 pages  0x836C0000-0x83900000
      // The XEX loader already set DESC[2] to RW.  We need to also make
      // CODE and RODATA writable for runtime patching.
      struct PageRange {
        uint32_t start;
        uint32_t size;
        const char* name;
      };
      const PageRange ranges[] = {
          {0x82000000, 70 * 0x10000, "RODATA-pre"},   // DESC[0]
          {0x82460000, 236 * 0x10000, "CODE"},         // DESC[1]
          {0x83320000, 58 * 0x10000, "DATA"},           // DESC[2]
          {0x836C0000, 36 * 0x10000, "RODATA-post"},   // DESC[3]
      };
      int ok_count = 0;
      for (const auto& r : ranges) {
        bool ok = heap1->Protect(r.start, r.size,
                                 kMemoryProtectRead | kMemoryProtectWrite);
        if (ok) {
          ok_count++;
        } else {
          XELOGW("DC3: Protect {:08X}-{:08X} ({}) FAILED",
                 r.start, r.start + r.size, r.name);
        }
      }
      XELOGI("DC3: Made PE image writable ({}/4 ranges succeeded)", ok_count);
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

    // Override CriticalSection::CriticalSection — the guest ctor calls
    // RtlInitializeCriticalSection via an IAT import thunk.  The decomp PE
    // has 379 xboxkrnl imports but the XEX only resolves ~196; unresolved
    // IAT entries (including RtlInitializeCriticalSection at 0x823F2D60)
    // remain null, causing CTR=0 → crash on bctr.  This host override
    // bypasses the broken thunk by calling Xenia's kernel implementation
    // directly.
    {
      const uint32_t critsec_ctor = kAddr.critsec_ctor;
      if (critsec_ctor && ctx.processor) {
        auto handler = [](xe::cpu::ppc::PPCContext* ppc_context,
                          xe::kernel::KernelState* kernel_state) {
          auto* memory = kernel_state->memory();
          uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
          auto* base = memory->TranslateVirtual<uint8_t*>(this_ptr);
          // mEntryCount = 0 (offset 0x0, big-endian uint32)
          xe::store_and_swap<uint32_t>(base + 0x00, 0);
          // mCritSec starts at offset 0x4 — this is X_RTL_CRITICAL_SECTION:
          //   X_DISPATCH_HEADER (0x10 bytes) + lock_count + recursion_count +
          //   owning_thread
          // Matches xeRtlInitializeCriticalSection() from xboxkrnl_rtl.cc.
          uint32_t cs_ptr = this_ptr + 4;
          // header.type=1, header.absolute=0, header.size=0, header.inserted=0
          xe::store_and_swap<uint32_t>(base + 0x04, 0x01000000);
          // header.signal_state = 0
          xe::store_and_swap<uint32_t>(base + 0x08, 0);
          // header.wait_list_flink = &header.wait_list_flink (self-referential)
          uint32_t wait_list_ptr = cs_ptr + 0x08;  // offsetof(header, wait_list_flink)
          xe::store_and_swap<uint32_t>(base + 0x0C, wait_list_ptr);
          // header.wait_list_blink = same
          xe::store_and_swap<uint32_t>(base + 0x10, wait_list_ptr);
          // lock_count = -1 (native int32_t, NOT be<> in Xenia's struct)
          *reinterpret_cast<int32_t*>(base + 0x14) = -1;
          // recursion_count = 0 (be<int32_t>)
          xe::store_and_swap<int32_t>(base + 0x18, 0);
          // owning_thread = 0 (be<uint32_t>)
          xe::store_and_swap<uint32_t>(base + 0x1C, 0);
          // Return this
          ppc_context->r[3] = static_cast<uint64_t>(this_ptr);
        };
        ctx.processor->RegisterGuestFunctionOverride(
            critsec_ctor, handler, "CriticalSection::CriticalSection");
        XELOGI("DC3: Registered CriticalSection ctor override at {:08X} "
               "(bypasses unresolved IAT thunk)", critsec_ctor);
        result.applied++;
      } else {
        result.skipped++;
      }
    }

    // Override CriticalSection::Enter — bypasses RtlEnterCriticalSection IAT thunk.
    // Layout: mEntryCount at +0x0, mCritSec (X_RTL_CRITICAL_SECTION) at +0x4.
    // Within mCritSec: lock_count at +0x14 (native int32), recursion_count at
    // +0x18 (be<int32>), owning_thread at +0x1C (be<uint32>).
    {
      const uint32_t addr = kAddr.critsec_enter;
      if (addr && ctx.processor) {
        auto handler = [](xe::cpu::ppc::PPCContext* ppc_context,
                          xe::kernel::KernelState* kernel_state) {
          auto* memory = kernel_state->memory();
          uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
          auto* base = memory->TranslateVirtual<uint8_t*>(this_ptr);
          auto* lock_count = reinterpret_cast<volatile int32_t*>(base + 0x14);
          uint32_t cur_thread =
              xe::kernel::XThread::GetCurrentThread()->guest_object();
          uint32_t owner =
              xe::load_and_swap<uint32_t>(base + 0x1C);
          if (owner == cur_thread) {
            // Already own the lock — re-enter.
            xe::atomic_inc(lock_count);
            int32_t rc = xe::load_and_swap<int32_t>(base + 0x18);
            xe::store_and_swap<int32_t>(base + 0x18, rc + 1);
          } else {
            // Fast uncontended path: CAS lock_count from -1 to 0.
            if (xe::atomic_cas(-1, 0, lock_count)) {
              xe::store_and_swap<uint32_t>(base + 0x1C, cur_thread);
              xe::store_and_swap<int32_t>(base + 0x18, 1);
            } else {
              // Contended — spin then wait (simplified: just spin for boot).
              for (int spin = 0; spin < 4000; spin++) {
                if (xe::atomic_cas(-1, 0, lock_count)) {
                  xe::store_and_swap<uint32_t>(base + 0x1C, cur_thread);
                  xe::store_and_swap<int32_t>(base + 0x18, 1);
                  goto acquired;
                }
              }
              // Waiter path
              xe::atomic_inc(lock_count);
              while (!xe::atomic_cas(0, 0, lock_count)) {
                // Busy-wait (boot is mostly single-threaded)
              }
              xe::store_and_swap<uint32_t>(base + 0x1C, cur_thread);
              xe::store_and_swap<int32_t>(base + 0x18, 1);
              acquired:;
            }
          }
          // mEntryCount++ (be<int32> at +0x0)
          int32_t entry = xe::load_and_swap<int32_t>(base);
          xe::store_and_swap<int32_t>(base, entry + 1);
        };
        ctx.processor->RegisterGuestFunctionOverride(
            addr, handler, "CriticalSection::Enter");
        XELOGI("DC3: Registered CriticalSection::Enter override at {:08X}", addr);
        result.applied++;
      } else {
        result.skipped++;
      }
    }

    // Override CriticalSection::Exit — bypasses RtlLeaveCriticalSection IAT thunk.
    {
      const uint32_t addr = kAddr.critsec_exit;
      if (addr && ctx.processor) {
        auto handler = [](xe::cpu::ppc::PPCContext* ppc_context,
                          xe::kernel::KernelState* kernel_state) {
          auto* memory = kernel_state->memory();
          uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
          auto* base = memory->TranslateVirtual<uint8_t*>(this_ptr);
          auto* lock_count = reinterpret_cast<volatile int32_t*>(base + 0x14);
          // mEntryCount-- (be<int32> at +0x0)
          int32_t entry = xe::load_and_swap<int32_t>(base);
          xe::store_and_swap<int32_t>(base, entry - 1);
          // recursion_count-- at +0x18
          int32_t rc = xe::load_and_swap<int32_t>(base + 0x18);
          if (rc <= 1) {
            // Fully releasing the lock.
            xe::store_and_swap<int32_t>(base + 0x18, 0);
            xe::store_and_swap<uint32_t>(base + 0x1C, 0);  // owning_thread = 0
            xe::atomic_dec(lock_count);
          } else {
            xe::store_and_swap<int32_t>(base + 0x18, rc - 1);
            xe::atomic_dec(lock_count);
          }
        };
        ctx.processor->RegisterGuestFunctionOverride(
            addr, handler, "CriticalSection::Exit");
        XELOGI("DC3: Registered CriticalSection::Exit override at {:08X}", addr);
        result.applied++;
      } else {
        result.skipped++;
      }
    }

    // Initialize STLport std::list<bool> gConditional sentinel.
    // BSS variables don't appear in MAP, so we extract addresses from
    // their dynamic initializer functions' PPC lis+addi instructions.
    {
      uint32_t addr = kAddr.g_conditional;
      XELOGI("DC3: gConditional: addr={:08X} ctor={:08X}",
             addr, kAddr.g_conditional_ctor);
      if (!addr && kAddr.g_conditional_ctor) {
        addr = ExtractPpcLisAddiAddr(memory, kAddr.g_conditional_ctor);
        if (addr) {
          kAddr.g_conditional = addr;
          XELOGI("DC3: Extracted gConditional BSS addr {:08X} from ctor {:08X}",
                 addr, kAddr.g_conditional_ctor);
        } else {
          // Dump first 8 instructions for debugging
          auto* base = memory->TranslateVirtual<uint8_t*>(kAddr.g_conditional_ctor);
          if (base) {
            XELOGW("DC3: PPC extraction failed at {:08X}; insns: {:08X} {:08X} "
                    "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    kAddr.g_conditional_ctor,
                    xe::load_and_swap<uint32_t>(base + 0),
                    xe::load_and_swap<uint32_t>(base + 4),
                    xe::load_and_swap<uint32_t>(base + 8),
                    xe::load_and_swap<uint32_t>(base + 12),
                    xe::load_and_swap<uint32_t>(base + 16),
                    xe::load_and_swap<uint32_t>(base + 20),
                    xe::load_and_swap<uint32_t>(base + 24),
                    xe::load_and_swap<uint32_t>(base + 28));
          }
        }
      }
      if (addr) {
        auto* pcond = memory->TranslateVirtual<uint8_t*>(addr);
        if (pcond) {
          xe::store_and_swap<uint32_t>(pcond + 0, addr);
          xe::store_and_swap<uint32_t>(pcond + 4, addr);
          XELOGI("DC3: Initialized gConditional sentinel at {:08X}", addr);
          result.applied++;
        }
      } else {
        XELOGW("DC3: gConditional address unknown — sentinel init skipped "
               "(ctors should handle it during _cinit)");
        result.skipped++;
      }
    }

    // Initialize STLport std::list<bool> gDataArrayConditional sentinel.
    // This list gates #include/#ifdef expansion in DataArray::Load.
    // Without proper construction, BSS zero-init makes the list appear
    // non-empty (head->next=0 != sentinel), causing DataArrayDefined()
    // to return false and silently skip ALL #include directives.
    {
      uint32_t addr = kAddr.g_data_array_conditional;
      if (!addr && kAddr.g_data_array_conditional_ctor) {
        addr = ExtractPpcLisAddiAddr(memory, kAddr.g_data_array_conditional_ctor);
        if (addr) {
          kAddr.g_data_array_conditional = addr;
          XELOGI("DC3: Extracted gDataArrayConditional BSS addr {:08X} from "
                 "ctor {:08X}", addr, kAddr.g_data_array_conditional_ctor);
        }
      }
      if (addr) {
        auto* pcond = memory->TranslateVirtual<uint8_t*>(addr);
        if (pcond) {
          xe::store_and_swap<uint32_t>(pcond + 0, addr);
          xe::store_and_swap<uint32_t>(pcond + 4, addr);
          XELOGI("DC3: Initialized gDataArrayConditional sentinel at {:08X}",
                 addr);
          result.applied++;
        }
      } else {
        XELOGW("DC3: gDataArrayConditional address unknown — sentinel init "
               "skipped (ctors should handle it during _cinit)");
        result.skipped++;
      }
    }

    // NOP out 2nd and 3rd CreateDefaults calls.
    // Rnd::PreInit calls CreateDefaults once (base factories). Then
    // NgRnd::PreInit and DxRnd::PreInit each call it again to recreate
    // with NG/Dx factories. The RELEASE of first-pass objects crashes
    // in RndCam dtor (likely corrupted ref list from /FORCE linker).
    // Keep only the first call — base objects suffice for boot.
    struct CdPatch {
      uint32_t addr;
      uint32_t expected;
      const char* name;
    };
    const CdPatch cd_patches[] = {
        {0x8285D3D8, 0x488907C9, "NgRnd::PreInit: 2nd CreateDefaults"},
        {0x83272CB4, 0x4BE7AEED, "DxRnd::PreInit: 3rd CreateDefaults"},
    };
    for (const auto& p : cd_patches) {
      if (PatchCheckedNop(memory, p.addr, p.expected, p.name)) {
        result.applied++;
      } else {
        XELOGW("DC3: Failed to NOP {} at {:08X}", p.name, p.addr);
        result.failed++;
      }
    }

    // Patch GPU-dependent init functions to immediately return (blr).
    // These allocate GPU textures/surfaces that block on null GPU.
    // NgPostProc::RebuildTex and NgDOFProc::Init are handled by JIT
    // overrides in ApplyDebugStubs; RndShadowMap::Init creates shadow
    // camera + texture with SetBitmap.
    constexpr uint32_t kPpcBlr = 0x4E800020;
    struct BlrPatch {
      uint32_t addr;
      const char* name;
    };
    const BlrPatch blr_patches[] = {
        {kAddr.rnd_shadowmap_init, "RndShadowMap::Init"},
        {kAddr.occlusion_query_mgr_ctor, "DxRndOcclusionQueryMgr::ctor"},
        {kAddr.dxrnd_init_buffers, "DxRnd::InitBuffers"},
        {kAddr.dxrnd_create_post_textures, "DxRnd::CreatePostTextures"},
        // Audio: SynthPreInit now creates base Synth (no XAudio2 dependency)
        // in the decomp build, so SynthInit runs normally and initializes
        // the Fader system.  XDK JIT overrides handle any stray XAudio2
        // calls.  No blr patches needed for Synth360::PreInit or SynthInit.
        // Bink: BinkStartAsyncThread blocks waiting for thread readiness
        // on nop GPU; PlatformInit may also block on D3D device.
        {kAddr.bink_start_async_thread, "BinkStartAsyncThread"},
        {kAddr.bink_platform_init, "BinkMovieSys::PlatformInit"},
        // LoadMgr::PollFrontLoader and PollUntilLoaded — NOT stubbed.
        // With gUsingCD=1, ARK loading creates DataLoaders. PollFrontLoader
        // must run to complete async loads.  Stubbing causes IsLoaded() asserts.
        // NOTE: Do NOT stub D3DDevice_Suspend/Resume — they pair
        // critical section enter/leave. Stubbing breaks CS ownership.
    };
    for (const auto& bp : blr_patches) {
      auto* heap = memory->LookupHeap(bp.addr);
      auto* mem = memory->TranslateVirtual<uint8_t*>(bp.addr);
      if (heap && mem) {
        uint32_t orig = xe::load_and_swap<uint32_t>(mem);
        heap->Protect(bp.addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(mem, kPpcBlr);
        XELOGI("DC3: Patched {} at {:08X} to blr (was {:08X})",
               bp.name, bp.addr, orig);
        result.applied++;
      } else {
        XELOGW("DC3: Failed to patch {} at {:08X}", bp.name, bp.addr);
        result.failed++;
      }
    }
  }

  // Splash screen stubs: DirLoader::LoadObjects blocks waiting for milo
  // file I/O that never completes on headless.  Stub PrepareNext to return
  // false (no screens) so the splash system is inert.
  {
    // PrepareNext returns bool — 0 = no screens available
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82921430, 0, "Splash::PrepareNext");
    // BeginSplasher creates a render thread and waits for state; skip it
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82922390, 0, "Splash::BeginSplasher");
    // Suspend/Resume wait for splash thread state changes, but the thread
    // was never created (BeginSplasher stubbed).  mThreaded=1 in ctor causes
    // Suspend to call WaitForState(kSuspended) → deadlock.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82920AA8, 0, "Splash::Suspend");
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82920C30, 0, "Splash::Resume");
    // LiveCameraInput::PreInit calls `new LiveCameraInput()` whose ctor
    // calls NuiInitialize/NuiAudioCreate/NuiSkeletonTrackingEnable/
    // NuiImageStreamOpen — all block without Kinect hardware.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x826D30D0, 0, "LiveCameraInput::PreInit");
    // LiveCameraInput::Init also blocks on NUI/Kinect hardware
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x826D31E0, 0, "LiveCameraInput::Init");
    // HasFileChecksumData() — return false to skip DTB checksum validation.
    // The decomp build's DTB files don't match original checksums, triggering
    // dirty disc errors and XamLoaderLaunchTitle (exit to dashboard).
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x828E2100, 0, "HasFileChecksumData");
    // VoiceInputPanel::LoadVoiceContexts reads kinect/speech/voice_contexts
    // config and calls Sym() on each entry.  Without Kinect/speech recognition,
    // this spins forever on "Data is not Symbol" errors during UIManager::Init.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x829844B8, 0, "VoiceInputPanel::LoadVoiceContexts");
    // Fader::UpdateValue iterates FaderGroup set (STL rb-tree at this+0x84).
    // With config parsing fixed (gBinStream cave), real fader data loads but
    // the STL container is corrupt (/FORCE:MULTIPLE BSS shifts).  Infinite
    // loop in _M_increment during SynthInit → SetVolume → UpdateValue.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82697920, 0, "Fader::UpdateValue");
    // Synth::InitSecurity (0x832E164C) calls ByteGrinder::Init which tries to
    // parse DTA data via DataReadString → yylex.  The flex lexer hangs in an
    // infinite loop (corrupt global state or garbage input from /FORCE BSS shifts).
    // Security/DRM init is unnecessary for emulation.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82717380, 0, "Synth::InitSecurity");
    // ShellInput::Init depends on Kinect/gesture subsystems (SpeechMgr,
    // GestureMgr, SkeletonUpdate, DepthBuffer).  TheSpeechMgr->AddSink()
    // hits corrupt MsgSinks lists because SpeechMgr was never initialized.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x8339CC08, 0, "ShellInput::Init");  // not in MAP, fallback stale
    // UIScreen::HasPanel — un-stubbed (Session 41).
    // mPanelList should be valid now that factories are registered and
    // .milo files load as correct types.
    // MoveMgr::Init loads choreography/move category data from config.
    // LoadCategoryData("genres") crashes (SP corruption) because config
    // data for move categories is missing or corrupt in decomp build.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x825B2580, 0, "MoveMgr::Init");
    // HamSongMgr::Init (0x82AF6720) reads song count from config, calls
    // vector::reserve with corrupt/huge size → length_error throw → memcpy
    // crashes reading past stack end (10 SIGSEGVs).  C++ exception handling
    // doesn't work in JIT, so the crash is unrecoverable.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x8298AA18, 0, "HamSongMgr::Init");
    // Locale::Init tries to find 'locale' key in config, then opens
    // devkit:\locale_keep.dta which fails (no devkit: device in emulator).
    // Crashes inside a vector/array operation with null data.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82A5C2F8, 0, "Locale::Init");
    // ObjRef::ReplaceList and list<ObjectDir*>::clear — un-stubbed (Session 41).
    // The "corrupt lists from .milo load failures" should be resolved now that
    // factories are registered and SynthInit properly initializes.
    // UIManager::GotoFirstScreen — re-stubbed: with config include expansion
    // now working, the game enters a fuller init path that includes .milo loading
    // via ChunkStream.  ChunkStream's async I/O threads fail to start
    // (thread creation fails in headless), causing the game to hang forever
    // waiting for the load to complete.  Stub this until async I/O is fixed.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x8284AD00, 0, "UIManager::GotoFirstScreen");
    // DirLoader stubs — un-stubbed (Session 41).
    // ClassAndNameSort and SaveObjects were needed because of corrupt objects
    // from failed .milo loads.  With proper factory registration and SynthInit
    // fixed, .milo files should load as correct types with valid objects.
    // list<RndTransformable*>::remove iterates a corrupt STL list sentinel
    // (/FORCE:MULTIPLE BSS shift).  Infinite loop in StlAlloc.h assert.
    // Called during Rnd::Init → ObjectDir init regardless of data loading.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x828B33D8, 0, "list<RndTransformable*>::remove");
    // FlowManager::Poll (0x831043A4) iterates FlowNodes calling virtual methods
    // via bctrl.  FlowNodes have corrupt vtables (/FORCE:MULTIPLE BSS shifts)
    // dispatching to 0x0C000000 (garbage).  24M+ no-op stub calls per second.
    // Game flow won't advance, but main loop continues.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82563200, 0, "FlowManager::Poll");
    // SkeletonUpdate::InstanceHandle creates a handle wrapper but asserts
    // sInstance != null each frame.  Return null handle (r3=0) to skip.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82544BF8, 0, "SkeletonUpdate::InstanceHandle");
    // SkeletonUpdate::PostUpdate is called from the main loop via
    // SkeletonUpdateHandle.  It calls Update → UpdateCallbacks which
    // iterate Kinect skeleton data that was never initialized, causing
    // corrupt vtable dispatch and DirLoader::SaveObjects infinite sort.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82544C90, 0, "SkeletonUpdate::PostUpdate");
    // SkeletonHistoryArchive::AddToHistory dispatches to garbage addresses
    // (string data interpreted as pointers) from corrupt Skeleton objects.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x828C8AE0, 0, "SkeletonHistoryArchive::AddToHistory");
    // GestureMgr::Poll reads Kinect skeleton data with bounds-check asserts
    // that fire on uninitialized data.  Kinect not available in headless mode.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x827D0EC0, 0, "GestureMgr::Poll");
    // GestureMgr::GetSkeleton and UpdateTrackedSkeletons are called from
    // non-Poll paths (UI character updates) causing per-frame asserts.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x827D1130, 0, "GestureMgr::GetSkeleton");
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x827D1370, 0, "GestureMgr::UpdateTrackedSkeletons");
    // App::DrawRegular tries to render via DxRnd::Present → VdSwap.
    // Without GPU initialization (headless mode), VdSwap crashes on
    // invalid frontbuffer physical address.  Stub to skip rendering.
    PatchStub8Resolved(memory, ctx.hack_pack_stubs, 0x82450D80, 0, "App::DrawRegular");
    // OSCMessenger::Poll is a Holmes debug networking function that polls
    // for OSC (Open Sound Control) messages over UDP.  Hangs forever when
    // no network is available (headless mode).  Use JIT-level override
    // (RegisterGuestFunctionOverride) — guest memory patches are unsafe in
    // /FORCE-linked regions where functions may overlap.
    if (ctx.processor) {
      auto nop_handler = [](cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState*) {
        ppc_context->r[3] = 0;
      };
      ctx.processor->RegisterGuestFunctionOverride(
          kAddr.osc_messenger_poll, nop_handler,
          "DC3:OSCMessenger::Poll");
      XELOGI("DC3: Registered OSCMessenger::Poll JIT override at {:08X}",
             kAddr.osc_messenger_poll);
    }
    // SkeletonIdentifier uses Kinect for player identification during boot.
    // Its Init() loops calling Poll() which never completes without Kinect.
    //
    // IMPORTANT: Do NOT use PatchStub8 / blr patches for these functions!
    // The /FORCE linker creates overlapping functions — a different function
    // (starting at Init+0x7C with its own stwu prologue, called from
    // merged_827F5720 in WaveFile.obj) has its body extending through
    // Poll's MAP entry at 0x8361669C.  Writing li r3,0; blr there corrupts
    // that function's control flow, creating an infinite loop.
    //
    // RegisterGuestFunctionOverride hooks at the JIT dispatch level without
    // modifying guest memory, so overlapping functions are unaffected.
    if (ctx.processor) {
      auto nop_handler = [](cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState*) {
        ppc_context->r[3] = 0;
      };
      ctx.processor->RegisterGuestFunctionOverride(
          kAddr.skeleton_identifier_init, nop_handler,
          "DC3:SkeletonIdentifier::Init");
      ctx.processor->RegisterGuestFunctionOverride(
          kAddr.skeleton_identifier_poll, nop_handler,
          "DC3:SkeletonIdentifier::Poll");
      XELOGI("DC3: Registered SkeletonIdentifier::Init/Poll JIT overrides "
             "at {:08X}/{:08X}",
             kAddr.skeleton_identifier_init, kAddr.skeleton_identifier_poll);
    }
    result.applied += 25;
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

  // Host-side StringTable::Add override (Session 37).
  // The guest heap (MemInit/MemAlloc) isn't initialized during __xc, so
  // StringTable::Add's internal AddBuf() fails when it needs to allocate.
  // Rather than forwarding to PPC code (impossible due to JIT embedding of
  // extern_handler_), we implement Add entirely on the host side.  Strings
  // are written into the pre-allocated guest char buffer so guest code can
  // read the const char* pointers that Symbol stores.
  if (ctx.processor && ctx.is_decomp_layout) {
    const uint32_t kStringTableAdd = kAddr.string_table_add;
    auto st_add_handler = [](cpu::ppc::PPCContext* ppc_context,
                             kernel::KernelState* kernel_state) {
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      if (!memory) {
        ppc_context->r[3] = 0;
        return;
      }
      uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);
      uint32_t str_guest = static_cast<uint32_t>(ppc_context->r[4]);
      if (!this_ptr || !str_guest) {
        ppc_context->r[3] = 0;
        return;
      }
      auto* str = memory->TranslateVirtual<const char*>(str_guest);
      if (!str) {
        ppc_context->r[3] = 0;
        return;
      }
      size_t len = std::strlen(str) + 1;

      // Read StringTable fields from guest memory.
      auto* st = memory->TranslateVirtual<uint8_t*>(this_ptr);
      if (!st) {
        ppc_context->r[3] = 0;
        return;
      }
      uint32_t m_start = xe::load_and_swap<uint32_t>(st + 0x00);
      int32_t cur_buf = static_cast<int32_t>(
          xe::load_and_swap<uint32_t>(st + 0x10));
      uint32_t cur_char = xe::load_and_swap<uint32_t>(st + 0x0C);

      if (cur_buf < 0 || !m_start) {
        // No buffer exists — can't add without MemAlloc.
        static int s_nobuf_count = 0;
        if (++s_nobuf_count <= 5)
          XELOGW("DC3: StringTable::Add: no buffer (this={:08X} mCurBuf={} "
                  "m_start={:08X})",
                  this_ptr, cur_buf, m_start);
        ppc_context->r[3] = str_guest;  // return original string
        return;
      }

      // Read current Buf: mBuffers[mCurBuf]
      auto* bufs = memory->TranslateVirtual<uint8_t*>(m_start);
      if (!bufs) {
        ppc_context->r[3] = str_guest;
        return;
      }
      uint8_t* cur_buf_ptr = bufs + cur_buf * 8;
      uint32_t buf_size = xe::load_and_swap<uint32_t>(cur_buf_ptr + 0);
      uint32_t buf_chars = xe::load_and_swap<uint32_t>(cur_buf_ptr + 4);

      // Deduplicate: scan existing buffer for this string before appending.
      // This ensures each unique string has exactly one address, so that
      // Symbol pointer comparison (operator==, operator<) works correctly.
      uint32_t used = cur_char - buf_chars;
      {
        auto* buf_host = memory->TranslateVirtual<const char*>(buf_chars);
        if (buf_host && used > 0) {
          const char* scan = buf_host;
          const char* scan_end = buf_host + used;
          while (scan < scan_end) {
            if (std::strcmp(scan, str) == 0) {
              // Found existing copy — return its guest address.
              uint32_t existing_addr = buf_chars +
                  static_cast<uint32_t>(scan - buf_host);
              static int s_dedup_count = 0;
              if (++s_dedup_count <= 5 || (s_dedup_count % 5000) == 0) {
                XELOGW("DC3: StringTable::Add dedup[{}]: '{}' already at "
                       "{:08X} (skipping append)",
                       s_dedup_count, str, existing_addr);
              }
              ppc_context->r[3] = existing_addr;
              return;
            }
            // Advance past this string + null terminator.
            scan += std::strlen(scan) + 1;
          }
        }
      }

      if (used + len > buf_size) {
        // Buffer full. Can't allocate new one (heap may not be ready).
        // Return original string pointer — caller will use it as-is.
        static int s_full_count = 0;
        if (++s_full_count <= 5)
          XELOGW("DC3: StringTable::Add: buffer full (this={:08X} used={} "
                  "len={} size={} buf_chars={:08X} str={})",
                  this_ptr, used, len, buf_size, buf_chars, str);
        ppc_context->r[3] = str_guest;
        return;
      }

      // Copy string into guest buffer at mCurChar.
      auto* dst = memory->TranslateVirtual<char*>(cur_char);
      if (!dst) {
        ppc_context->r[3] = str_guest;
        return;
      }
      std::memcpy(dst, str, len);
      uint32_t result_addr = cur_char;

      // Advance mCurChar.
      xe::store_and_swap<uint32_t>(st + 0x0C, cur_char + static_cast<uint32_t>(len));

      static int s_add_count = 0;
      if (++s_add_count <= 10 || (s_add_count % 1000) == 0) {
        XELOGI("DC3: StringTable::Add[{}]: this={:08X} '{}' -> {:08X} "
               "(used={}/{} m_start={:08X} buf_chars={:08X})",
               s_add_count, this_ptr, str, result_addr, used + len,
               buf_size, m_start, buf_chars);
      }
      ppc_context->r[3] = result_addr;
    };
    ctx.processor->RegisterGuestFunctionOverride(
        kStringTableAdd, st_add_handler,
        "DC3:StringTable::Add(host-impl)");
    XELOGI("DC3: Registered host-side StringTable::Add override at {:08X}",
           kStringTableAdd);

    // Override Symbol::PreInit to prevent it from overwriting our host-
    // constructed gStringTable.  PreInit normally calls MemAlloc(20) which
    // returns 0x14 (the size arg) because the heap isn't initialized yet,
    // then stores that to gStringTable.  We skip the entire function body
    // and just return — gStringTable is already set up.
    // Override Symbol::PreInit to:
    //   1. Skip StringTable allocation (host-constructed above)
    //   2. Construct gHashTable on the host side (Resize needs MemAlloc)
    //   3. Skip AddExitCallback (non-critical)
    //
    // gHashTable is KeylessHash<const char*, const char*> at 0x83AE01C4.
    // Layout (24 bytes big-endian):
    //   +0x00  T2*  mEntries
    //   +0x04  int  mSize
    //   +0x08  bool mOwnEntries (padded to 4 bytes)
    //   +0x0C  int  mNumEntries
    //   +0x10  T2   mEmpty    = NULL
    //   +0x14  T2   mRemoved  = 0xFFFFFFFF
    //
    // PreInit calls gHashTable.Resize(80000, nullptr):
    //   NextHashPrime(80000) = 92681 = 0x16A09
    //   Allocates 92681 * 4 = 370724 bytes, fills with mEmpty=0.
    const uint32_t kPreInit = kAddr.symbol_preinit;
    auto preinit_handler = [](cpu::ppc::PPCContext* ppc_context,
                              kernel::KernelState* kernel_state) {
      const uint32_t kGST = kAddr.g_string_table_global;
      const uint32_t kGHT = kAddr.g_hash_table;
      constexpr uint32_t kHashSize = 92681;  // NextHashPrime(80000)
      constexpr uint32_t kEntrySize = 4;     // sizeof(const char*)
      auto* memory = kernel_state ? kernel_state->memory() : nullptr;
      if (!memory) return;

      // Check gStringTable — should be our host construction.
      auto* gst = memory->TranslateVirtual<uint8_t*>(kGST);
      uint32_t gst_val = gst ? xe::load_and_swap<uint32_t>(gst) : 0;

      // Construct gHashTable entries.
      uint32_t entries_alloc = kHashSize * kEntrySize;
      uint32_t entries_addr = memory->SystemHeapAlloc(entries_alloc, 0x10);
      if (entries_addr) {
        // Fill entries with mEmpty = 0 (NULL).
        auto* entries = memory->TranslateVirtual<uint8_t*>(entries_addr);
        if (entries) std::memset(entries, 0, entries_alloc);

        // Write gHashTable fields.
        auto* ht = memory->TranslateVirtual<uint8_t*>(kGHT);
        if (ht) {
          xe::store_and_swap<uint32_t>(ht + 0x00, entries_addr);  // mEntries
          xe::store_and_swap<uint32_t>(ht + 0x04, kHashSize);     // mSize
          // mOwnEntries = true (1, stored as big-endian bool at +0x08)
          // Clear the full 4-byte padding slot then set the bool byte.
          xe::store_and_swap<uint32_t>(ht + 0x08, 0);
          ht[0x08] = 1;  // big-endian: first byte of the 4-byte slot
          xe::store_and_swap<uint32_t>(ht + 0x0C, 0);           // mNumEntries
          xe::store_and_swap<uint32_t>(ht + 0x10, 0);           // mEmpty = NULL
          xe::store_and_swap<uint32_t>(ht + 0x14, 0xFFFFFFFF);  // mRemoved = -1
        }

        // Verify readback.
        uint32_t v_entries = xe::load_and_swap<uint32_t>(ht + 0x00);
        uint32_t v_size = xe::load_and_swap<uint32_t>(ht + 0x04);
        uint8_t  v_own = ht[0x08];
        uint32_t v_num = xe::load_and_swap<uint32_t>(ht + 0x0C);
        uint32_t v_empty = xe::load_and_swap<uint32_t>(ht + 0x10);
        uint32_t v_removed = xe::load_and_swap<uint32_t>(ht + 0x14);
        XELOGW("DC3: Symbol::PreInit intercepted — gStringTable={:08X}, "
                "gHashTable at {:08X}: entries={:08X} size={} own={} "
                "num={} empty={:08X} removed={:08X} "
                "(raw +08: {:02X}{:02X}{:02X}{:02X}) "
                "(caller LR={:08X})",
                gst_val, kGHT, v_entries, v_size, v_own,
                v_num, v_empty, v_removed,
                ht[0x08], ht[0x09], ht[0x0A], ht[0x0B],
                static_cast<uint32_t>(ppc_context->lr));
      } else {
        XELOGW("DC3: Symbol::PreInit intercepted — gStringTable={:08X}, "
                "FAILED to allocate gHashTable entries ({} bytes) "
                "(caller LR={:08X})",
                gst_val, entries_alloc,
                static_cast<uint32_t>(ppc_context->lr));
      }
    };
    ctx.processor->RegisterGuestFunctionOverride(kPreInit, preinit_handler,
        "DC3:Symbol::PreInit(host-impl)");
    XELOGI("DC3: Registered PreInit host-impl at {:08X}", kPreInit);

    // gSystemConfig fix: the /FORCE linker shifts BSS layout, so
    // PreInitSystem stores the config DataArray* at the wrong offset,
    // leaving gSystemConfig = NULL.  We fix this by PPC-patching the
    // end of ReadSystemConfig: replace the final `blr` with a jump to
    // a code cave that stores r3 into gSystemConfig then returns.
    // This replaces the old approach of patching specific stw/lwz
    // instructions in PreInitSystem (which broke on every rebuild).
    if (kAddr.read_system_config != 0 && kAddr.g_system_config != 0) {
      // Find the blr at the end of ReadSystemConfig by scanning forward.
      const uint32_t fn = kAddr.read_system_config;
      auto* fn_mem = memory->TranslateVirtual<uint8_t*>(fn);
      // Use ProtocolDebugString as a code cave (already stubbed to blr).
      const uint32_t cave = kAddr.protocol_debug_string;
      auto* cave_heap = memory->LookupHeap(cave);
      auto* cave_mem = memory->TranslateVirtual<uint8_t*>(cave);
      auto* fn_heap = memory->LookupHeap(fn);
      if (fn_mem && cave_mem && cave_heap && fn_heap) {
        // Write code cave: store r3 to gSystemConfig, then blr.
        //   lis r11, hi(gSystemConfig)
        //   stw r3, lo(gSystemConfig)(r11)
        //   blr
        const uint32_t gsc = kAddr.g_system_config;
        uint32_t hi = (gsc >> 16) & 0xFFFF;
        int16_t lo = static_cast<int16_t>(gsc & 0xFFFF);
        // If lo is negative, we need to adjust hi (addis semantics)
        if (lo < 0) hi += 1;

        cave_heap->Protect(cave, 16,
                           kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(cave_mem + 0,
            0x3D600000 | (hi & 0xFFFF));         // lis r11, hi
        xe::store_and_swap<uint32_t>(cave_mem + 4,
            0x906B0000 | (gsc & 0xFFFF));         // stw r3, lo(r11)
        xe::store_and_swap<uint32_t>(cave_mem + 8, 0x4E800020);  // blr

        // Find ReadSystemConfig's `blr` by scanning backwards from
        // the next function.  ReadSystemConfig is typically ~200 bytes.
        int blr_offset = -1;
        for (int off = 0; off < 0x200; off += 4) {
          uint32_t insn = xe::load_and_swap<uint32_t>(fn_mem + off);
          if (insn == 0x4E800020) {  // blr
            blr_offset = off;
            // Don't break — we want the LAST blr (could be multiple).
          }
        }
        if (blr_offset >= 0) {
          // Replace blr with branch to cave.
          int32_t delta = static_cast<int32_t>(cave - (fn + blr_offset));
          uint32_t b_insn = 0x48000000 | (delta & 0x03FFFFFC);
          fn_heap->Protect(fn + blr_offset, 4,
                           kMemoryProtectRead | kMemoryProtectWrite);
          xe::store_and_swap<uint32_t>(fn_mem + blr_offset, b_insn);
          // Dump the first N instructions of ReadSystemConfig for debugging
          {
            int dump_count = std::min(blr_offset / 4 + 4, 0x200 / 4);
            std::string dump;
            for (int d = 0; d < dump_count; ++d) {
              uint32_t insn = xe::load_and_swap<uint32_t>(fn_mem + d * 4);
              if (!dump.empty()) dump += " ";
              char tmp[16];
              snprintf(tmp, sizeof(tmp), "%08X", insn);
              dump += tmp;
              if ((d % 8) == 7 || d == dump_count - 1) {
                XELOGI("DC3: ReadSystemConfig+{:03X}: {}", d * 4 - (d % 8) * 4, dump);
                dump.clear();
              }
            }
          }
          XELOGI("DC3: Patched ReadSystemConfig blr at {:08X}+{:X} -> "
                 "code cave at {:08X} (stores r3 to gSystemConfig={:08X})",
                 fn, blr_offset, cave, gsc);
          result.applied++;
        } else {
          XELOGW("DC3: Could not find blr in ReadSystemConfig at {:08X}",
                 fn);
        }
      }
    }

    // gBinStream fix: the decomp's DataReadStream won the /FORCE conflict
    // but doesn't set gBinStream (the original static in DataFile.obj at
    // 0x83CA2FC4).  DataInput (flex YY_INPUT) reads gBinStream to get
    // scanner input.  Without this, the text parser gets EOF immediately,
    // producing empty DataArrays for ALL .dta file parsing.
    //
    // Fix: PPC code cave at the start of DataReadStream that stores r3
    // (BinStream* argument) to gBinStream before the function body runs.
    if (kAddr.data_read_stream != 0) {
      const uint32_t kDRS = kAddr.data_read_stream;
      const uint32_t kGBS = 0x83CA2FC4;  // gBinStream (original DataFile.obj static)
      auto* drs_mem = memory->TranslateVirtual<uint8_t*>(kDRS);
      auto* drs_heap = memory->LookupHeap(kDRS);
      // Code cave: use space after gSystemConfig cave in protocol_debug_string
      const uint32_t kCave = kAddr.protocol_debug_string + 12;
      auto* cave_mem = memory->TranslateVirtual<uint8_t*>(kCave);
      auto* cave_heap = memory->LookupHeap(kCave);
      if (drs_mem && drs_heap && cave_mem && cave_heap) {
        // Read the first instruction of DataReadStream (will be displaced)
        uint32_t displaced_insn = xe::load_and_swap<uint32_t>(drs_mem);
        XELOGI("DC3: DataReadStream at {:08X}, first insn: {:08X}",
               kDRS, displaced_insn);

        // Build code cave (4 instructions = 16 bytes):
        //   lis r0, hi16(gBinStream)
        //   stw r3, lo16(gBinStream)(r0)
        //   <displaced instruction>
        //   b DataReadStream+4
        uint32_t gbs_hi = (kGBS >> 16) & 0xFFFF;
        int16_t gbs_lo = static_cast<int16_t>(kGBS & 0xFFFF);
        if (gbs_lo < 0) gbs_hi += 1;  // addis sign adjustment

        cave_heap->Protect(kCave, 16,
                           kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(cave_mem + 0,
            0x3C000000 | (gbs_hi & 0xFFFF));      // lis r0, hi
        xe::store_and_swap<uint32_t>(cave_mem + 4,
            0x90600000 | (kGBS & 0xFFFF));         // stw r3, lo(r0)
        xe::store_and_swap<uint32_t>(cave_mem + 8, displaced_insn);
        // Branch back to DataReadStream+4
        int32_t back_delta = static_cast<int32_t>((kDRS + 4) - (kCave + 12));
        xe::store_and_swap<uint32_t>(cave_mem + 12,
            0x48000000 | (back_delta & 0x03FFFFFC));  // b DataReadStream+4

        // Replace first instruction of DataReadStream with branch to cave
        drs_heap->Protect(kDRS, 4,
                          kMemoryProtectRead | kMemoryProtectWrite);
        int32_t fwd_delta = static_cast<int32_t>(kCave - kDRS);
        xe::store_and_swap<uint32_t>(drs_mem,
            0x48000000 | (fwd_delta & 0x03FFFFFC));  // b cave

        XELOGI("DC3: Patched DataReadStream at {:08X} -> cave at {:08X} "
               "(stores r3 to gBinStream={:08X})",
               kDRS, kCave, kGBS);
        result.applied++;
      } else {
        XELOGW("DC3: Could not patch DataReadStream (addr={:08X}, heap/mem failed)",
               kDRS);
      }
    }

    // Override DataArray::Load to log what size it reads from the stream.
    // DataArray::Load is at 0x8351F540.  We can't re-implement it, but
    // we CAN intercept it to check the first value read from the stream
    // (the node count).  Load(bs) has: this=r3, bs=r4.
    // We'll read the DataArray* (this) before and after to see the change.
    // Actually, we'll override operator>>(BinStream&, DataArray*&) at
    // 0x8351FCD4 to log what bool it reads.
    {
      constexpr uint32_t kOpGtGtDA = 0x8351FCD4;
      auto op_handler = [](cpu::ppc::PPCContext* ppc_context,
                           kernel::KernelState* kernel_state) {
        auto* memory = kernel_state->memory();
        // r3 = BinStream&, r4 = DataArray*& (pointer to pointer)
        uint32_t bs_addr = static_cast<uint32_t>(ppc_context->r[3]);
        uint32_t da_ptr_addr = static_cast<uint32_t>(ppc_context->r[4]);
        // Read first byte from BinStream to check if it would be 'true'
        // Actually, we can't easily do this without knowing BinStream layout.
        // Let's just log the call and return what the original would.
        static int call_count = 0;
        call_count++;
        if (call_count <= 20) {
          XELOGI("DC3: operator>>(BinStream,DataArray*) call #{} "
                 "bs={:08X} da_ptr={:08X} LR={:08X}",
                 call_count, bs_addr, da_ptr_addr,
                 static_cast<uint32_t>(ppc_context->lr));
        }
        // We can't forward to original, so we need to either:
        // 1. Not override (just log)
        // 2. Re-implement
        // For now, return nullptr to flag that it was called.
        // This will break things but gives us diagnostic info.
        if (memory && da_ptr_addr) {
          auto* p = memory->TranslateVirtual<uint8_t*>(da_ptr_addr);
          if (p) {
            xe::store_and_swap<uint32_t>(p, 0);  // *da = nullptr
          }
        }
        ppc_context->r[3] = bs_addr;  // return bs&
      };
      // DON'T register this override - it would break the DTB reading.
      // Instead, just log a note.
      XELOGI("DC3: operator>>(BinStream,DataArray*) at {:08X} (not overridden)",
             kOpGtGtDA);
    }

    // Patch CheckForArchive in guest memory to set gUsingCD=1 and return.
    // gUsingCD=1 = CD mode: ARK archive loaded, files read as .dtb.
    // Writes to BOTH the decomp address and the original binary address
    // so that original code (CDRead) can also see gUsingCD=1.
    {
      const uint32_t kCheckForArchive = kAddr.check_for_archive;
      const uint32_t kUCD = kAddr.g_using_cd;
      const uint32_t kUCDOrig = kAddr.g_using_cd_orig;
      auto* heap = memory->LookupHeap(kCheckForArchive);
      auto* mem = memory->TranslateVirtual<uint8_t*>(kCheckForArchive);
      if (heap && mem) {
        uint32_t w0 = xe::load_and_swap<uint32_t>(mem);
        if (w0 != 0x00000000) {
          heap->Protect(kCheckForArchive, 32,
                        kMemoryProtectRead | kMemoryProtectWrite);
          uint32_t hi = (kUCD >> 16) & 0xFFFF;
          uint32_t lo = kUCD & 0xFFFF;
          uint32_t hi_o = (kUCDOrig >> 16) & 0xFFFF;
          uint32_t lo_o = kUCDOrig & 0xFFFF;
          xe::store_and_swap<uint32_t>(mem + 0,  0x3C600000 | hi);     // lis r3, hi(decomp)
          xe::store_and_swap<uint32_t>(mem + 4,  0x60630000 | lo);     // ori r3, r3, lo(decomp)
          xe::store_and_swap<uint32_t>(mem + 8,  0x38800001);          // li r4, 1
          xe::store_and_swap<uint32_t>(mem + 12, 0x90830000);          // stw r4, 0(r3)
          xe::store_and_swap<uint32_t>(mem + 16, 0x3C600000 | hi_o);   // lis r3, hi(orig)
          xe::store_and_swap<uint32_t>(mem + 20, 0x60630000 | lo_o);   // ori r3, r3, lo(orig)
          xe::store_and_swap<uint32_t>(mem + 24, 0x90830000);          // stw r4, 0(r3)
          xe::store_and_swap<uint32_t>(mem + 28, 0x4E800020);          // blr
          XELOGI("DC3: Patched CheckForArchive at {:08X} to set gUsingCD=1 "
                 "at {:08X} (decomp) + {:08X} (orig)",
                 kCheckForArchive, kUCD, kUCDOrig);
        }
      }
    }
  }

  // -----------------------------------------------------------------------
  // ArkFile::Read host override — bypass noop'd BlockMgr.
  //
  // ArkFile::Read is called via vtable (bctrl).  Registering an override
  // directly at ArkFile::Read's address causes a utf8::invalid_utf8 crash
  // because SetupExtern() on the already-resolved function corrupts state.
  //
  // Workaround: PPC bytepatch + trampoline.  We rewrite ArkFile::Read's
  // entry to:  mflr r0; bl STUB; mtlr r0; blr
  // and place a `blr` stub 0x40 bytes into the function body.  The
  // override is registered at the STUB address (not ArkFile::Read's entry),
  // avoiding the crash.  When bctrl hits ArkFile::Read, the JIT compiles
  // our patched PPC, sees the direct `bl` to an extern stub, and inlines
  // CallExtern — firing our host handler.
  // -----------------------------------------------------------------------
  {
    const uint32_t kArkFileRead = kAddr.arkfile_read;
    if (kArkFileRead && ctx.processor && ctx.memory) {
      auto* mem = ctx.memory->virtual_membase();
      auto* heap = ctx.memory->LookupHeap(kArkFileRead);
      if (heap) {
        // Stub address: 0x40 bytes into ArkFile::Read body (unused after
        // our bytepatch replaces the entry).
        const uint32_t kStubAddr = kArkFileRead + 0x40;

        // Protect both the entry (16 bytes) and the stub location (4 bytes).
        heap->Protect(kArkFileRead, 0x44,
                      kMemoryProtectRead | kMemoryProtectWrite);

        // Bytepatch ArkFile::Read entry: mflr r0; bl STUB; mtlr r0; blr
        const uint32_t bl_offset = kStubAddr - (kArkFileRead + 4);
        const uint32_t bl_insn = 0x48000001 | (bl_offset & 0x03FFFFFC);
        xe::store_and_swap<uint32_t>(mem + kArkFileRead + 0,  0x7C0802A6); // mflr r0
        xe::store_and_swap<uint32_t>(mem + kArkFileRead + 4,  bl_insn);    // bl STUB
        xe::store_and_swap<uint32_t>(mem + kArkFileRead + 8,  0x7C0803A6); // mtlr r0
        xe::store_and_swap<uint32_t>(mem + kArkFileRead + 12, 0x4E800020); // blr

        // Write blr at stub address (fallback if override not applied).
        xe::store_and_swap<uint32_t>(mem + kStubAddr, 0x4E800020); // blr

        // Register override at the STUB address, not ArkFile::Read's entry.
        ctx.processor->RegisterGuestFunctionOverride(
            kStubAddr, Dc3ArkFileReadHandler, "DC3:ArkFile::Read");

        XELOGI("DC3: ArkFile::Read bytepatch at {:08X} → bl {:08X} (stub), "
               "override registered at stub",
               kArkFileRead, kStubAddr);
        result.applied++;
      }
    }
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
      parse_skip_list("75,98-101,142-328");
      XELOGI("DC3: Auto-NUI skip enabled (75,98-101,142-328)");
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

    // Inject decomp .CRT$XCU entries into null slots of __xc table.
    //
    // The decomp build produces a separate .CRT$XCU PE section containing
    // function pointers to the decomp's real ??__E dynamic initializers
    // (std::list ctors, std::map ctors, etc.).  These are OUTSIDE the
    // __xc_a..__xc_z range (.bss sits between them), so _cinit never
    // walks them.  Fix: copy them into null slots created by the NUI skip
    // list, so _cinit picks them up naturally.
    {
      const uint32_t kXcuStart = kAddr.crt_xcu_start;
      const uint32_t kXcuEnd = kAddr.crt_xcu_end;
      int xcu_count = (kXcuEnd - kXcuStart) / 4;
      if (kXcuStart && kXcuEnd > kXcuStart && xcu_count > 0) {
        // Collect null slot addresses in __xc table (post-sanitization).
        std::vector<uint32_t> null_slots;
        for (uint32_t addr = kXcA; addr < kXcZ; addr += 4) {
          auto* p = memory->TranslateVirtual<uint8_t*>(addr);
          uint32_t entry = xe::load_and_swap<uint32_t>(p);
          if (entry == 0) {
            null_slots.push_back(addr);
          }
        }

        // Read .CRT$XCU entries.
        auto* xcu_heap = memory->LookupHeap(kXcuStart);
        if (xcu_heap) {
          uint32_t xcu_page_start = kXcuStart & ~0xFFFu;
          uint32_t xcu_page_end = (kXcuEnd + 0xFFFu) & ~0xFFFu;
          xcu_heap->Protect(xcu_page_start, xcu_page_end - xcu_page_start,
                            kMemoryProtectRead);
        }

        int injected = 0;
        int skipped_null = 0;
        int skipped_no_slot = 0;
        // Use null slots from the end (higher indices run later in _cinit).
        size_t slot_idx = null_slots.size();
        for (int i = xcu_count - 1; i >= 0; --i) {
          auto* xcu_p = memory->TranslateVirtual<uint8_t*>(kXcuStart + i * 4);
          uint32_t xcu_entry = xe::load_and_swap<uint32_t>(xcu_p);
          if (xcu_entry == 0) {
            skipped_null++;
            continue;
          }
          if (slot_idx == 0) {
            skipped_no_slot++;
            continue;
          }
          --slot_idx;
          auto* slot_p =
              memory->TranslateVirtual<uint8_t*>(null_slots[slot_idx]);
          xe::store_and_swap<uint32_t>(slot_p, xcu_entry);
          injected++;
        }
        XELOGI(
            "DC3: .CRT$XCU injection: {} entries from {:08X}..{:08X}, "
            "{} injected into __xc null slots, {} null, {} no-slot",
            xcu_count, kXcuStart, kXcuEnd, injected, skipped_null,
            skipped_no_slot);
        if (skipped_no_slot > 0) {
          XELOGW(
              "DC3: WARNING: {} .CRT$XCU entries could not be injected "
              "(not enough null slots in __xc table!)",
              skipped_no_slot);
        }
        result.applied++;
      }
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

    // Host-side StringTable construction (Session 37).
    // Symbol::PreInit normally allocates gStringTable during __xc via
    // #pragma init_seg(lib), but /FORCE linker placed its pointer outside
    // __xc_a..__xc_z.  The previous PPC trampoline at __xc[0] called PreInit
    // but MemAlloc returned sizeof(StringTable)=0x14 unchanged because the
    // guest heap (MemInit) isn't initialized until main().
    //
    // Instead, construct a valid StringTable in host-managed guest memory
    // (SystemHeapAlloc).  This gives __xc constructors a working StringTable
    // for Symbol::Symbol calls.  Symbol::Init() has a fallback
    // `if (!gStringTable) PreInit(...)` but since we set it here, that path
    // is skipped.  gHashTable is default-constructed at __xc[156] and grows
    // lazily.
    //
    // StringTable layout (20 bytes, big-endian PPC):
    //   +0x00  vector<Buf>._M_start     (ptr to Buf array)
    //   +0x04  vector<Buf>._M_finish    (one past last Buf)
    //   +0x08  vector<Buf>._M_end_of_storage._M_data
    //   +0x0C  mCurChar                 (write cursor into char buffer)
    //   +0x10  mCurBuf                  (current buffer index, 0-based)
    // Buf layout (8 bytes):
    //   +0x00  size   (buffer capacity)
    //   +0x04  chars  (pointer to char data)
    {
      constexpr uint32_t kStringBufSize = 560000;  // matches PreInit(560000, 80000)
      constexpr uint32_t kStringTableObjSize = 20; // sizeof(StringTable) = 0x14
      constexpr uint32_t kBufStructSize = 8;       // sizeof(StringTable::Buf)
      constexpr uint32_t kTotalAlloc =
          kStringTableObjSize + kBufStructSize + kStringBufSize;
      uint32_t base = memory->SystemHeapAlloc(kTotalAlloc, 0x10);
      if (base) {
        uint32_t st_addr = base;
        uint32_t buf_addr = base + kStringTableObjSize;
        uint32_t chars_addr = base + kStringTableObjSize + kBufStructSize;
        auto* st = memory->TranslateVirtual<uint8_t*>(st_addr);
        auto* buf = memory->TranslateVirtual<uint8_t*>(buf_addr);
        auto* chars = memory->TranslateVirtual<uint8_t*>(chars_addr);
        if (st && buf && chars) {
          std::memset(chars, 0, kStringBufSize);
          // Write Buf[0]: {size=560000, chars=chars_addr}
          xe::store_and_swap<uint32_t>(buf + 0, kStringBufSize);
          xe::store_and_swap<uint32_t>(buf + 4, chars_addr);
          // Write StringTable object.
          xe::store_and_swap<uint32_t>(st + 0x00, buf_addr);                  // _M_start
          xe::store_and_swap<uint32_t>(st + 0x04, buf_addr + kBufStructSize); // _M_finish
          xe::store_and_swap<uint32_t>(st + 0x08, buf_addr + kBufStructSize); // _M_end_of_storage
          xe::store_and_swap<uint32_t>(st + 0x0C, chars_addr);               // mCurChar
          xe::store_and_swap<uint32_t>(st + 0x10, 0);                        // mCurBuf = 0
          // Write gStringTable pointer.
          auto* gst = memory->TranslateVirtual<uint8_t*>(kAddr.g_string_table_global);
          if (gst) {
            xe::store_and_swap<uint32_t>(gst, st_addr);
          }
          XELOGI("DC3: Host-constructed StringTable at {:08X} "
                 "(Buf={:08X} chars={:08X} size={}) -> gStringTable={:08X}",
                 st_addr, buf_addr, chars_addr, kStringBufSize,
                 kAddr.g_string_table_global);
          // Verify readback: dump the StringTable fields as guest code sees them.
          uint32_t v_start = xe::load_and_swap<uint32_t>(st + 0x00);
          uint32_t v_finish = xe::load_and_swap<uint32_t>(st + 0x04);
          uint32_t v_eos = xe::load_and_swap<uint32_t>(st + 0x08);
          uint32_t v_curchar = xe::load_and_swap<uint32_t>(st + 0x0C);
          uint32_t v_curbuf = xe::load_and_swap<uint32_t>(st + 0x10);
          uint32_t v_buf_size = xe::load_and_swap<uint32_t>(buf + 0);
          uint32_t v_buf_chars = xe::load_and_swap<uint32_t>(buf + 4);
          uint32_t v_gst = gst ? xe::load_and_swap<uint32_t>(gst) : 0;
          XELOGI("DC3: StringTable verify: gStringTable*={:08X} "
                 "_M_start={:08X} _M_finish={:08X} _M_eos={:08X} "
                 "mCurChar={:08X} mCurBuf={} "
                 "Buf[0].size={} Buf[0].chars={:08X}",
                 v_gst, v_start, v_finish, v_eos,
                 v_curchar, static_cast<int32_t>(v_curbuf),
                 v_buf_size, v_buf_chars);
          result.applied++;
        }
      } else {
        XELOGW("DC3: Failed to allocate {} bytes for host StringTable",
               kTotalAlloc);
      }
    }

    // Host-side gHashTable construction (Session 37).
    // gHashTable is KeylessHash<const char*, const char*> at kAddr.g_hash_table.
    // Must be initialized before __xc because Symbol::Symbol (called during __xc)
    // accesses gHashTable. The __xc constructor for gHashTable may not have run
    // yet, leaving BSS-zero memory where mOwnEntries=0 causes assertions.
    // PreInit handler also writes this, but we need it even earlier.
    {
      constexpr uint32_t kHashSize = 92681;  // NextHashPrime(80000)
      constexpr uint32_t kEntrySize = 4;     // sizeof(const char*)
      uint32_t entries_alloc = kHashSize * kEntrySize;
      uint32_t entries_addr = memory->SystemHeapAlloc(entries_alloc, 0x10);
      if (entries_addr) {
        auto* entries = memory->TranslateVirtual<uint8_t*>(entries_addr);
        if (entries) std::memset(entries, 0, entries_alloc);
        auto* ht = memory->TranslateVirtual<uint8_t*>(kAddr.g_hash_table);
        if (ht) {
          xe::store_and_swap<uint32_t>(ht + 0x00, entries_addr);  // mEntries
          xe::store_and_swap<uint32_t>(ht + 0x04, kHashSize);     // mSize
          xe::store_and_swap<uint32_t>(ht + 0x08, 0);
          ht[0x08] = 1;                                           // mOwnEntries
          xe::store_and_swap<uint32_t>(ht + 0x0C, 0);             // mNumEntries
          xe::store_and_swap<uint32_t>(ht + 0x10, 0);             // mEmpty=NULL
          xe::store_and_swap<uint32_t>(ht + 0x14, 0xFFFFFFFF);    // mRemoved=-1
          XELOGI("DC3: Host-constructed gHashTable at {:08X} "
                 "(entries={:08X} size={})",
                 kAddr.g_hash_table, entries_addr, kHashSize);
          result.applied++;
        }
      }
    }

    // Host-side STL container initialization (Session 40).
    // The decomp build's /FORCE:MULTIPLE linker merges link_glue.obj stub
    // __xc dynamic initializers (all ICF'd to 'blr') over the real .obj
    // initializers. This leaves global std::list and std::map objects in
    // BSS-zero state (sentinel nodes not self-referencing), causing infinite
    // loops when code iterates them.
    //
    // Fix: write the empty-container sentinel values from the host.
    //
    // std::list layout (STLport, PPC32):
    //   +0x00: _M_next (ptr) — empty: points to self
    //   +0x04: _M_prev (ptr) — empty: points to self
    //
    // std::map/_Rb_tree layout (STLport, PPC32):
    //   +0x00: _M_color  (u8) — empty: 1 (red)
    //   +0x04: _M_parent (ptr) — empty: 0
    //   +0x08: _M_left   (ptr) — empty: points to self
    //   +0x0C: _M_right  (ptr) — empty: points to self
    //   +0x10: _M_node_count — empty: 0
    {
      // Lists: write self-referencing sentinel (_M_next = _M_prev = self).
      // Includes both static globals (BSS) and member lists inside globals
      // whose constructors were skipped (e.g. TheLoadMgr).
      const uint32_t kTheLoadMgr = kAddr.the_load_mgr;
      const uint32_t stl_lists[] = {
          // Static globals (BSS, __xc initializers ICF'd to blr)
          kAddr.auto_timer_stmrs,        // AutoTimer::sTimers
          kAddr.rnd_overlay_soverlays,    // RndOverlay::sOverlays
          kAddr.synth_pollable_spollables, // SynthPollable::sPollables
          kAddr.midi_parser_sparsers,     // MidiParser::sParsers
          kAddr.rnd_multi_mesh_sproxy,    // RndMultiMesh::sProxyPool
          kAddr.g_caches,                 // gCaches (FileCache.obj)
          kAddr.g_decompression_queue,     // gDecompressionQueue (ChunkStream.obj)
          // TheLoadMgr member lists (CRT skipped in bisect range)
          kTheLoadMgr + 0x00,  // mLoaders
          kTheLoadMgr + 0x10,  // mFactories
          kTheLoadMgr + 0x20,  // mLoading
      };
      // Also set TheLoadMgr.mPlatform = kPlatformXBox (2).
      // Without this, PlatformSymbol() returns "" and archive paths
      // become "gen/main_.hdr" instead of "gen/main_xbox.hdr".
      {
        auto* lm = memory->TranslateVirtual<uint8_t*>(kTheLoadMgr);
        if (lm) {
          xe::store_and_swap<uint32_t>(lm + 0x08, 2);  // mPlatform = kPlatformXBox
        }
      }
      int list_count = 0;
      for (uint32_t addr : stl_lists) {
        auto* p = memory->TranslateVirtual<uint8_t*>(addr);
        if (p) {
          uint32_t cur_next = xe::load_and_swap<uint32_t>(p + 0);
          if (cur_next == 0) {
            xe::store_and_swap<uint32_t>(p + 0, addr);  // _M_next = self
            xe::store_and_swap<uint32_t>(p + 4, addr);  // _M_prev = self
            list_count++;
          }
        }
      }

      // Map: Hmx::Object::sFactories (_Rb_tree sentinel).
      // The std::map address from the catalog IS the _Rb_tree sentinel node.
      {
        const uint32_t addr = kAddr.object_factories_map;
        auto* p = memory->TranslateVirtual<uint8_t*>(addr);
        if (p) {
          uint32_t cur_parent = xe::load_and_swap<uint32_t>(p + 0x04);
          if (cur_parent == 0 &&
              xe::load_and_swap<uint32_t>(p + 0x08) == 0) {
            p[0x00] = 1;  // _M_color = red (big-endian byte)
            xe::store_and_swap<uint32_t>(p + 0x04, 0);     // _M_parent = NULL
            xe::store_and_swap<uint32_t>(p + 0x08, addr);  // _M_left = self
            xe::store_and_swap<uint32_t>(p + 0x0C, addr);  // _M_right = self
            xe::store_and_swap<uint32_t>(p + 0x10, 0);     // _M_node_count = 0
            list_count++;
          }
        }
      }

      if (list_count > 0) {
        XELOGI("DC3: Host-initialized {} STL containers (lists + maps) "
               "— fixes ICF-merged nop __xc initializers from link_glue.obj",
               list_count);
        result.applied += list_count;
      }
    }

    // PPC patch: fix gSystemConfig store offset in PreInitSystem.
    // The /FORCE:MULTIPLE linker shifted BSS layout so PreInitSystem stores
    // gSystemConfig at r30-8 (=0x83A927E8) instead of r30-16 (=0x83A927E0).
    // Without this fix, gSystemConfig points to the wrong address, causing
    // all FindArray calls during SystemPreInit to fail ("Couldn't find 'system'
    // in array"), which prevents MemInit from setting up heaps.
    {
      struct PpcPatch {
        uint32_t addr;
        uint32_t old_insn;
        uint32_t new_insn;
        const char* desc;
      };
      const PpcPatch patches[] = {
          {0x834D6400, 0x909EFFF8, 0x909EFFF0,
           "PreInitSystem: stw gSystemConfig (fix offset -8 -> -16)"},
          {0x834D6444, 0x809EFFF8, 0x809EFFF0,
           "PreInitSystem: lwz gSystemConfig (fix offset -8 -> -16)"},
          // BinStream::Read PPC patch removed — now fully host-overridden
      };
      for (const auto& p : patches) {
        auto* heap = memory->LookupHeap(p.addr);
        auto* mem = memory->TranslateVirtual<uint8_t*>(p.addr);
        if (heap && mem) {
          uint32_t cur = xe::load_and_swap<uint32_t>(mem);
          if (cur == p.old_insn) {
            heap->Protect(p.addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(mem, p.new_insn);
            XELOGI("DC3: Patched {} at {:08X} ({:08X} -> {:08X})",
                   p.desc, p.addr, p.old_insn, p.new_insn);
            result.applied++;
          } else {
            XELOGW("DC3: PPC patch at {:08X} — expected {:08X} but found {:08X}",
                    p.addr, p.old_insn, cur);
          }
        }
      }
    }

    // Set gUsingCD=1 early: CD mode, files from ARK archive.
    // GetFileAll crash is bypassed by stubbing it to return null.
    // Must write to BOTH the decomp address and the original binary address
    // because original binary code (CDRead, etc.) reads gUsingCD from the
    // original hardcoded address, not the decomp's relocated address.
    {
      auto* ucd = memory->TranslateVirtual<uint8_t*>(kAddr.g_using_cd);
      if (ucd) {
        xe::store_and_swap<uint32_t>(ucd, 1);
        XELOGI("DC3: Set gUsingCD=1 at {:08X} (decomp address)",
               kAddr.g_using_cd);
        result.applied++;
      }
      auto* ucd_orig = memory->TranslateVirtual<uint8_t*>(kAddr.g_using_cd_orig);
      if (ucd_orig) {
        xe::store_and_swap<uint32_t>(ucd_orig, 1);
        XELOGI("DC3: Set gUsingCD=1 at {:08X} (original binary address — "
               "synced for original CDRead/UsingCD)",
               kAddr.g_using_cd_orig);
        result.applied++;
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
  ApplyXdkOverrides(ctx, stopgap_result);
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

void Dc3PopulateAddressesFromCatalog(
    const std::unordered_map<std::string, uint32_t>& catalog,
    const std::unordered_map<std::string, uint32_t>& crt_sentinels) {
  int updated = 0;
  auto get = [&](const char* key, uint32_t& field) {
    auto it = catalog.find(key);
    if (it != catalog.end() && it->second != field) {
      field = it->second;
      ++updated;
    }
  };
  auto get_crt = [&](const char* key, uint32_t& field) {
    auto it = crt_sentinels.find(key);
    if (it != crt_sentinels.end() && it->second != field) {
      field = it->second;
      ++updated;
    }
  };

  // CRT sentinels (from crt_sentinels section)
  get_crt("__xc_a", kAddr.xc_a);
  get_crt("__xc_z", kAddr.xc_z);
  get_crt("__xi_a", kAddr.xi_a);
  get_crt("__xi_z", kAddr.xi_z);

  // Decomp .CRT$XCU section
  get("crt_xcu_start", kAddr.crt_xcu_start);
  get("crt_xcu_end", kAddr.crt_xcu_end);

  // CRT functions
  get("ioinit", kAddr.ioinit);
  get("cinit", kAddr.cinit);
  get("errno_fn", kAddr.errno_fn);
  get("invalid_parameter_noinfo", kAddr.invalid_parameter_noinfo);
  get("call_reportfault", kAddr.call_reportfault);
  get("amsg_exit", kAddr.amsg_exit);
  get("report_gsfailure", kAddr.report_gsfailure);

  // CRT formatter
  get("output_l", kAddr.output_l);
  get("woutput_l", kAddr.woutput_l);

  // Debug subsystem
  get("debug_print", kAddr.debug_print);
  get("debug_fail", kAddr.debug_fail);
  get("debug_do_crucible", kAddr.debug_do_crucible);
  get("datanode_print", kAddr.datanode_print);

  // Import/thunk
  get("xapi_call_thread_notify", kAddr.xapi_call_thread_notify);
  get("mtinit", kAddr.mtinit);
  get("xregister_thread_notify", kAddr.xregister_thread_notify);
  get("text_start", kAddr.text_start);
  get("text_size", kAddr.text_size);
  get("idata_start", kAddr.idata_start);
  get("idata_end", kAddr.idata_end);
  get("thunk_area_start", kAddr.thunk_area_start);
  get("thunk_area_end", kAddr.thunk_area_end);

  // Locale
  get("get_system_language", kAddr.get_system_language);
  get("get_system_locale", kAddr.get_system_locale);
  get("xget_locale", kAddr.xget_locale);
  get("xtl_get_language", kAddr.xtl_get_language);
  get("debug_break", kAddr.debug_break);

  // ReadCacheStream probes
  get("rcs_read_cache_stream", kAddr.rcs_read_cache_stream);
  get("rcs_bufstream_read_impl", kAddr.rcs_bufstream_read_impl);
  get("rcs_bufstream_seek_impl", kAddr.rcs_bufstream_seek_impl);

  // SystemConfig / FindArray
  get("system_config_2", kAddr.system_config_2);
  get("find_array", kAddr.find_array);

  // Object / factory globals
  get("object_factories_map", kAddr.object_factories_map);
  get("register_factory", kAddr.register_factory);
  get("new_object", kAddr.new_object);
  get("object_set_name", kAddr.object_set_name);
  get("load_meta_materials", kAddr.load_meta_materials);
  get("rndmat_static_name_sym", kAddr.rndmat_static_name_sym);
  get("metamaterial_static_name_sym", kAddr.metamaterial_static_name_sym);
  get("g_system_config", kAddr.g_system_config);
  get("read_system_config", kAddr.read_system_config);
  get("g_string_table_global", kAddr.g_string_table_global);
  get("g_hash_table", kAddr.g_hash_table);

  // CriticalSection (IAT thunk bypass)
  get("critsec_ctor", kAddr.critsec_ctor);
  get("critsec_enter", kAddr.critsec_enter);
  get("critsec_exit", kAddr.critsec_exit);

  // Memory / allocator
  get("mem_or_pool_alloc", kAddr.mem_or_pool_alloc);
  get("mem_alloc", kAddr.mem_alloc);
  get("pool_alloc", kAddr.pool_alloc);
  get("mem_free", kAddr.mem_free);
  get("pool_free", kAddr.pool_free);
  get("mem_or_pool_free", kAddr.mem_or_pool_free);
  get("operator_new", kAddr.operator_new);
  get("operator_delete", kAddr.operator_delete);
  get("g_num_heaps", kAddr.g_num_heaps);
  get("string_reserve", kAddr.string_reserve);
  get("g_chunk_alloc", kAddr.g_chunk_alloc);

  // DataArray / DataNode / Symbol
  get("string_table_add", kAddr.string_table_add);
  get("symbol_preinit", kAddr.symbol_preinit);

  // TextStream / String ops
  get("textstream_op_const_char", kAddr.textstream_op_const_char);
  get("string_op_plus_eq", kAddr.string_op_plus_eq);

  // XMP
  get("xmp_override_bg_music", kAddr.xmp_override_bg_music);
  get("xmp_restore_bg_music", kAddr.xmp_restore_bg_music);

  // Write bridges
  get("write_nolock", kAddr.write_nolock);
  get("write_fn", kAddr.write_fn);

  // FileIsLocal
  get("file_is_local", kAddr.file_is_local);

  // File system globals
  get("g_using_cd", kAddr.g_using_cd);
  get("check_for_archive", kAddr.check_for_archive);
  get("file_init", kAddr.file_init);
  get("archive_init", kAddr.archive_init);
  get("the_archive", kAddr.the_archive);
  get("arkfile_read", kAddr.arkfile_read);
  get("system_pre_init_1", kAddr.system_pre_init_1);
  get("system_pre_init_2", kAddr.system_pre_init_2);

  // Holmes
  get("protocol_debug_string", kAddr.protocol_debug_string);

  // Wind
  get("set_wind", kAddr.set_wind);

  // RndTransformable
  get("set_dirty_force", kAddr.set_dirty_force);

  // Memory_Xbox
  get("alloc_type", kAddr.alloc_type);

  // Rnd
  get("rnd_create_defaults", kAddr.rnd_create_defaults);

  // MetaMaterial
  get("create_and_set_meta_mat", kAddr.create_and_set_meta_mat);
  get("s_meta_materials", kAddr.s_meta_materials);

  // Post-processing / GPU init
  get("ng_postproc_rebuild_tex", kAddr.ng_postproc_rebuild_tex);
  get("ng_dofproc_init", kAddr.ng_dofproc_init);
  get("rnd_shadowmap_init", kAddr.rnd_shadowmap_init);
  get("dxrnd_suspend", kAddr.dxrnd_suspend);
  get("occlusion_query_mgr_ctor", kAddr.occlusion_query_mgr_ctor);
  get("d3d_device_suspend", kAddr.d3d_device_suspend);
  get("d3d_device_resume", kAddr.d3d_device_resume);
  get("dxrnd_init_buffers", kAddr.dxrnd_init_buffers);
  get("dxrnd_create_post_textures", kAddr.dxrnd_create_post_textures);

  // Audio / Synth
  get("synth360_preinit", kAddr.synth360_preinit);
  get("synth_init", kAddr.synth_init);

  // Bink video
  get("bink_start_async_thread", kAddr.bink_start_async_thread);
  get("bink_platform_init", kAddr.bink_platform_init);

  // LoadMgr
  get("poll_front_loader", kAddr.poll_front_loader);
  get("poll_until_loaded", kAddr.poll_until_loaded);

  // CRT RTTI
  get("rt_dynamic_cast", kAddr.rt_dynamic_cast);

  // String constants
  get("g_null_str", kAddr.g_null_str);

  // SkeletonIdentifier (Kinect)
  get("skeleton_identifier_init", kAddr.skeleton_identifier_init);
  get("skeleton_identifier_poll", kAddr.skeleton_identifier_poll);

  // OSCMessenger (Holmes debug)
  get("osc_messenger_poll", kAddr.osc_messenger_poll);

  // BinStream / Rand2
  get("binstream_read", kAddr.binstream_read);
  get("binstream_read_endian", kAddr.binstream_read_endian);
  get("rand2_ctor", kAddr.rand2_ctor);
  get("rand2_int", kAddr.rand2_int);
  get("bs_op_dataarray", kAddr.bs_op_dataarray);

  // STL container globals (BSS, sentinel init from host)
  get("the_load_mgr", kAddr.the_load_mgr);
  get("auto_timer_stmrs", kAddr.auto_timer_stmrs);
  get("rnd_overlay_soverlays", kAddr.rnd_overlay_soverlays);
  get("synth_pollable_spollables", kAddr.synth_pollable_spollables);
  get("midi_parser_sparsers", kAddr.midi_parser_sparsers);
  get("rnd_multi_mesh_sproxy", kAddr.rnd_multi_mesh_sproxy);
  get("g_caches", kAddr.g_caches);
  get("g_decompression_queue", kAddr.g_decompression_queue);

  // gConditional / gDataArrayConditional ctor functions
  // (BSS variable addresses extracted at runtime from PPC code)
  get("g_conditional_ctor", kAddr.g_conditional_ctor);
  get("g_data_array_conditional_ctor", kAddr.g_data_array_conditional_ctor);

  XELOGI("DC3: Populated {} address catalog entries from manifest", updated);
  XELOGI("DC3: Key addresses: system_config_2={:08X} g_system_config={:08X} "
         "the_load_mgr={:08X} object_factories_map={:08X}",
         kAddr.system_config_2, kAddr.g_system_config,
         kAddr.the_load_mgr, kAddr.object_factories_map);
}

}  // namespace xe
