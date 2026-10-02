/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 guest address table shared by the hack packs and IK telemetry (NOT upstream).
 *
 * Moved verbatim out of decomp/dc3_hack_pack.cc, where it was an
 * anonymous-namespace global. Defined in dc3_hack_pack_common.cc.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_ADDRESSES_H_
#define XENIA_TITLES_DC3_DC3_ADDRESSES_H_

#include <cstdint>

namespace xe {

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
  // HamIKEffector (IK telemetry instrumentation)
  uint32_t ham_ik_poll = 0x824C21E8;                // size 0x4FC
  uint32_t ham_ik_apply_constraints = 0x824BF5D8;   // size 0x244, 100% match
  uint32_t ham_ik_get_ground_height = 0x824BF820;   // size 0x78
  uint32_t ham_ik_get_type = 0x824C0820;             // size 0x25C, 100% match
  uint32_t ham_ik_apply_pos_constraints = 0x824BF430; // size 0x1A8, 100% match
  uint32_t ham_ik_elbow = 0x824C16D8;                // IKElbow, size 0xF0, 100% match
  uint32_t ham_ik_do_fancy_elbow = 0x824C17C8;       // DoFancyElbow, size 0x3FC
  // In-function clamp point inside HamIKEffector::Poll (idx 191, `fmr f1, f29`)
  // just before Interp(neutralQ.v, effQ.v, clampFactor, q.v).  At this PC
  // neutralQ.v.z = 0xC8(r1), effQ.v.z = 0x78(r1), clampFactor = f29.
  uint32_t ham_ik_poll_ankle_clamp = 0x824C24E4;
  // In-function point inside HamIKEffector::Poll just before the FINAL
  // SetWorldXfm `bl` (`addi r4, r1, 0xE0` loading &finalXfm).  At this PC
  // finalXfm is the stack Transform @ 0xE0(r1); finalXfm.v.x/.y/.z are at
  // 0x104/0x108/0x10C(r1).  Capturing here gives the ankle IK's FINAL OUTPUT
  // (post Interp + post finger->effector back-transform).
  uint32_t ham_ik_poll_final_xfm_out = 0x824C26C8;
  uint32_t holmes_client_poll = 0x82631C58;          // per-frame, already noop-stubbed
};
extern Dc3Addresses kAddr;

}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_ADDRESSES_H_
