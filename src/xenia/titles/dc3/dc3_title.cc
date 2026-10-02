/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 (title 373307D9) title module (NOT upstream).
 *
 * The DC3 launch block, NUI return externs and lifetime hooks,
 * moved verbatim out of emulator.cc (Phase 1, FORK_CLEANUP_PLAN.md).
 ******************************************************************************
 */

#include "xenia/titles/dc3/dc3_title.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if XE_PLATFORM_LINUX
#include <sys/mman.h>
#include <cerrno>
#endif

#include "xenia/config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/apu/apu_flags.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_input_driver.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/titles/dc3/dc3_dta_channel.h"
#include "xenia/titles/dc3/dc3_flags.h"
#include "xenia/titles/dc3/dc3_hack_pack.h"
#include "xenia/titles/dc3/dc3_nui_patch_resolver.h"
#include "xenia/titles/dc3/dc3_nui_sequencer.h"
#include "xenia/titles/dc3/dc3_runtime_telemetry.h"
#include "xenia/titles/dc3/decomp/dc3_decomp_launch.h"
#include "xenia/titles/title_hooks.h"

#if XE_PLATFORM_LINUX
DECLARE_string(dc3_dta_channel);
#endif  // XE_PLATFORM_LINUX

namespace xe {

using namespace xe::literals;

namespace {
using namespace xe::dc3;

void Dc3NuiReturnOkExtern(cpu::ppc::PPCContext* ppc_context,
                          kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 0;
}

void Dc3NuiReturnNeg1Extern(cpu::ppc::PPCContext* ppc_context,
                            kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = UINT64_C(0xFFFFFFFFFFFFFFFF);
}

void Dc3NuiReturn1Extern(cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
  (void)kernel_state;
  if (ppc_context && ppc_context->scratch) {
    Dc3RuntimeTelemetryRecordNuiOverrideHit(
        static_cast<uint32_t>(ppc_context->scratch));
  }
  ppc_context->r[3] = 1;
}

}  // namespace

void ApplyDc3LaunchHooks(const titles::TitleLaunchContext& ctx) {
  using namespace xe::dc3;
  // The names Emulator::CompleteLaunch used, so the block below is verbatim.
  Memory* memory = ctx.memory;
  cpu::Processor* processor = ctx.processor;
  kernel::UserModule* module = ctx.module;
  const std::optional<uint32_t>& title_id = ctx.title_id;
  const std::filesystem::path& content_root = ctx.content_root;
  const bool headless = ctx.headless;

  // DC3 title-specific guest code patches.
  // DC3 Title ID: 0x373307D9 (Dance Central 3)
  bool dc3_is_decomp_layout = false;
  std::optional<Dc3NuiPatchManifest> dc3_patch_manifest;
  // Title-gated only: dc3_is_decomp_layout is not known yet (layout detection
  // runs ~500 lines below), so gating on it here made this whole block dead
  // code -- the manifest never loaded and Dc3PopulateAddressesFromCatalog
  // never ran, leaving kAddr on compiled-in defaults for every boot.  Loading
  // the manifest for an original-layout image is harmless: every consumer
  // (hack pack, kAddr populate) is separately gated on the detected layout.
  if (title_id.has_value() && title_id.value() == 0x373307D9) {
    Dc3MaybeCleanStaleContentCache(content_root);

    // Opt this title in to the MMIO write soft-fault (64KB-vs-4KB protect
    // granularity conflict in the XEX image data region). These two constants
    // track the DC3 debug build's data layout and used to be hardcoded inside
    // the shared fault handler, where they applied to every game; they are the
    // exact values that were in mmio_handler.cc, kept verbatim so DC3 boot
    // behavior is unchanged. Log the module's .data bounds next to them so
    // drift is visible after a relink (the range deliberately spans more than
    // .data alone, so it is not derived from the section table).
    cpu::MMIOHandler::SetSoftFaultWritableRange(0x83320000, 0x836C0000);
    if (auto* xex = module->xex_module()) {
      if (auto* data = xex->GetPESection(".data")) {
        XELOGI("DC3: module .data is [{:08X}, {:08X}) (soft-fault writable "
               "range is [83320000, 836C0000))",
               data->address, data->address + data->size);
      }
    }

    // Load the patch manifest early so it's available for both NUI patching
    // and the hack pack (which runs independently of --stub_nui_functions).
    std::filesystem::path early_manifest_path;
    if (!cvars::dc3_nui_patch_manifest_path.empty()) {
      early_manifest_path = cvars::dc3_nui_patch_manifest_path;
    } else if (auto auto_path = Dc3AutoProbePatchManifestPath()) {
      early_manifest_path = *auto_path;
    }
    if (!early_manifest_path.empty()) {
      dc3_patch_manifest = Dc3LoadNuiPatchManifest(early_manifest_path);
      if (dc3_patch_manifest) {
        XELOGI("DC3: Loaded patch manifest '{}' (build_label={} targets={} "
               "crt={} hack_pack_stubs={})",
               xe::path_to_utf8(early_manifest_path),
               dc3_patch_manifest->build_label,
               dc3_patch_manifest->targets.size(),
               dc3_patch_manifest->crt_sentinels.size(),
               dc3_patch_manifest->hack_pack_stubs.size());
      } else {
        XELOGW("DC3: Failed to load patch manifest '{}'",
               xe::path_to_utf8(early_manifest_path));
      }
    }
  }

  //
  // The DC3 debug build statically links the Xbox 360 Kinect SDK (NUI).
  // NuiInitialize and related functions are PPC code embedded in the XEX,
  // NOT kernel imports. They fail because Kinect hardware doesn't exist
  // in xenia. The debug build's MILO_ASSERT halts on these failures.
  //
  // We patch the NUI functions in guest memory with PPC stubs before the
  // JIT compiles them. Each stub is 2 instructions (8 bytes):
  //   li r3, 0    (0x38600000) - return S_OK
  //   blr         (0x4E800020) - return
  //
  // This is the standard emulator approach for HLE of statically-linked
  // SDK functions (similar to Dolphin's OS HLE patches).
  if (title_id.has_value() && title_id.value() == 0x373307D9 &&
      cvars::stub_nui_functions) {
    XELOGI("DC3: Stubbing NUI (Kinect SDK) functions in guest memory");

    // PPC instructions (big-endian)
    const uint32_t kLiR3_0 = 0x38600000;    // li r3, 0  (return S_OK / 0)
    const uint32_t kLiR3_1 = 0x38600001;    // li r3, 1
    const uint32_t kLiR3_Neg1 = 0x3860FFFF; // li r3, -1 (return E_UNEXPECTED)
    const uint32_t kBlr = 0x4E800020;       // blr

    // NUI function addresses from the DC3 debug XEX (ham_xbox_r.exe).
    // These are guest virtual addresses for the statically-linked NUI SDK.
    // The functions are PPC code from the Xbox 360 Kinect SDK libraries.
    //
    // Addresses sourced from: dc3-decomp/config/373307D9/symbols.txt
    //
    // TODO: These addresses are specific to the DC3 debug build. A future
    // enhancement could detect the build variant (debug vs retail) by
    // checking function prologues before patching, or use a title-specific
    // patch config file.
    //
    // TODO: Consider implementing proper NUI emulation in xenia's kernel
    // layer instead of guest memory patches. This would involve:
    //   1. Adding NUI kernel module exports (NuiInitialize, NuiShutdown, etc.)
    //      similar to xam_nui.cc's XamNui* exports
    //   2. Registering them in the export resolver so import thunks work
    //   3. But core NUI functions are statically linked, NOT imported, so
    //      this approach would require either:
    //      a) Modifying x64_emitter.cc's Call() to check kExtern behavior
    //         (currently only CallExtern for sc2 checks it), or
    //      b) Adding a guest function override mechanism to Processor that
    //         intercepts calls by address (using ExternHandler + patching
    //         the indirection table entry)
    //   4. A proper implementation could provide skeleton data, depth maps,
    //      etc. for automated testing of gesture/dance gameplay
    // Functions that MUST return S_OK (game asserts on failure):
    //   - NuiInitialize: LiveCameraInput ctor line 129 MILO_ASSERT_FMT
    //   - NuiSkeletonTrackingEnable: LiveCameraInput ctor line 141
    //   - NuiImageStreamOpen: LiveCameraInput ctor lines 146, 160
    //     Returns S_OK but does NOT write output handle -> handle stays NULL
    //     -> NuiImageStreamGetNextFrame guarded by if(handle) -> never called
    //
    // Functions that can return failure (game handles gracefully):
    //   - NuiAudioCreate: wrapped in if(SUCCEEDED(...)), no assert
    //   - NuiImageStreamGetNextFrame: no assert, just skips frame processing
    //
    // Functions called from destructor (NuiShutdown always, audio only if
    // NuiAudioCreate succeeded):
    //   - NuiShutdown: called unconditionally
    //   - NuiAudioRelease: only if unk11d4 set (NuiAudioCreate success)
    //
    // The NUI SDK has ~50 functions statically linked. We stub all known
    // entry points to return S_OK (0). Functions that produce frame data
    // (GetNextFrame) return E_UNEXPECTED (-1) to signal "no data available".
    //
    // TODO: For more complete Kinect emulation, these stubs could be
    // replaced with C++ handlers that provide:
    //   - Synthetic skeleton data for automated gameplay testing
    //   - Pre-recorded depth/color streams for regression testing
    //   - Scripted gesture sequences for CI/CD integration
    //   This would require the Processor-level ExternHandler approach
    //   described above, since bl-called functions don't go through
    //   the CallExtern codepath.

    Dc3NuiPatchSpec patches[] = {
        // Core lifecycle
        {0x829D1200, kLiR3_0, kBlr, "NuiInitialize"},
        {0x829CEDA0, kLiR3_0, kBlr, "NuiShutdown"},

        // Skeleton tracking
        {0x829C25F0, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
        {0x829C1E18, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
        {0x829C1F90, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
        {0x829C2790, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

        // Image streams - return S_OK but don't write output handle.
        // The handle pointer (r8) stays NULL -> GetNextFrame never called
        // because LiveCameraInput::PollTracking guards with if(curBuf.unk0).
        {0x829C9330, kLiR3_0, kBlr, "NuiImageStreamOpen"},
        {0x829C86F0, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
        {0x829C8A18, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
        {0x829C91C8, kLiR3_0, kBlr, "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

        // Audio - NuiAudioCreate returning failure (E_UNEXPECTED) prevents
        // the game from calling NuiAudioRegisterCallbacks (no assert).
        // LiveCameraInput sets unk11d4=0, destructor skips audio cleanup.
        //
        // TODO: NuiAudioCreate internally accesses global NUI state via
        // RtlEnterCriticalSection on a NUI mutex, allocates buffers with
        // XMemAlloc, calls XamVoiceGetMicArrayAudioEx for Kinect mic array,
        // creates 2 audio threads. Full audio emulation would require
        // implementing the NUIAUDIO subsystem with:
        //   - MEC (Microphone Echo Cancellation) stub
        //   - XamVoiceGetMicArrayAudioEx returning dummy audio streams
        //   - Audio processing thread stubs
        {0x82A0E028, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
        {0x82A0DA48, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
        {0x82A0D928, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
        {0x82A0D9A0, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
        {0x82A0C0A0, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
        {0x82A0C108, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
        {0x82A0D440, kLiR3_0, kBlr, "NuiAudioRelease"},

        // Camera properties - called at end of LiveCameraInput ctor
        // (SetColorCameraProperty) and in diagnostic/debug draw code.
        // Must not crash; accessing uninitialized NUI global state would
        // segfault without this stub.
        //
        // TODO: Camera property stubs could track set values in a map
        // and return them from Get calls, enabling camera config testing
        // without Kinect hardware.
        {0x829C7F48, kLiR3_0, kBlr, "NuiCameraSetProperty"},
        {0x829C7058, kLiR3_0, kBlr, "NuiCameraGetProperty"},
        {0x829C7068, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},
        {0x829C7FA0, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
        {0x829C6868, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
        {0x829C3FE0, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
        {0x829C3EF8, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
        {0x829C4940, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
        {0x829C4E38, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},

        // Identity (used by Skeleton.cpp for player identification)
        //
        // TODO: NuiIdentityIdentify takes a tracking ID, flags, callback,
        // and user data. A proper stub could invoke the callback with a
        // "no match" result to simulate identity processing completing.
        {0x829C36B0, kLiR3_0, kBlr, "NuiIdentityEnroll"},
        {0x829C3870, kLiR3_0, kBlr, "NuiIdentityIdentify"},
        {0x829C3998, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
        {0x829C3BB0, kLiR3_0, kBlr, "NuiIdentityAbort"},

        // Fitness tracking (FitnessFilter.cpp)
        // Only called during fitness gameplay mode. All use MILO_NOTIFY
        // on failure (not MILO_ASSERT), so failure is safe.
        {0x829D1B68, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
        {0x829D1E30, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
        {0x829D1F00, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
        {0x829D1FD0, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
        {0x82E61690, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

        // Wave gesture (WaveToTurnOnLight.cpp)
        {0x829D1758, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
        {0x829D1668, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

        // Head tracking
        {0x829DA0C8, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},
        {0x829DA598, kLiR3_0, kBlr, "NuiHeadPositionDisable"},

        // Speech recognition (SpeechMgr.cpp) - many have MILO_ASSERT_FMT.
        // SpeechMgr is only created if kinect.speech.enabled=1 in config.
        // If speech IS enabled, these must return S_OK to avoid asserts
        // in NuiSpeechCreateGrammar, NuiSpeechCommitGrammar, etc.
        //
        // TODO: Speech emulation could accept pre-scripted voice commands
        // for automated testing of menu navigation and gameplay triggers.
        // Would need to implement:
        //   - Grammar state management (rule tree in host memory)
        //   - Event queue with synthetic recognition events
        //   - NuiSpeechGetEvents returning scripted results
        {0x82A24B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
        {0x82A23B70, kLiR3_0, kBlr, "NuiSpeechDisable"},
        {0x82A23BB0, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
        {0x82A23B80, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
        {0x82A23BA0, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
        {0x82A22A48, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
        {0x82A21068, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
        {0x82A22978, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
        {0x82A21090, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
        {0x82A21078, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
        {0x82A22998, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
        {0x82A229B8, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
        {0x82A229E0, kLiR3_0, kBlr, "NuiSpeechCreateState"},
        {0x82A22A00, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
        {0x82A210A0, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
        {0x82A22988, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
        // NOTE: 0x82A24A98 was previously stubbed as "NuiSpeech__E_init"
        // but MAP file reveals it's actually Object::sFactories static
        // initializer (Object.obj) — a critical game engine function.
        // Stubbing it broke the object factory system and caused hangs
        // in downstream initializers (gPropPaths at 0x82A24B28).
        //
        // The original JIT boundary issue (blr in EmulateRecognition stub
        // at 0x82A24AB0 getting scanned into the initializer) is avoided
        // by also not stubbing NuiSpeechEmulateRecognition — the original
        // function prologue doesn't have an early blr, so the JIT
        // boundary detection works correctly with the original code.
        //
        // NuiSpeechEmulateRecognition (0x82A24AB0) is never called
        // because all speech API entry points are already stubbed above.

        // Misc
        {0x82B57560, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

        // Xbox SmartGlass (XBC) SDK - statically linked from XBC.lib
        // SmartGlassInit() calls XbcInitialize(), which calls
        // CXbcImpl::Initialize(). If Initialize returns failure, the game
        // prints "Failed to initialize Xbox SmartGlass library." and crashes.
        // The thunk functions (XbcInitialize, XbcDoWork, XbcSendJSON) are
        // only 4 bytes each (branch instructions), so we stub the real
        // CXbcImpl implementations instead.
        //
        // TODO: SmartGlass could be used for controller input automation
        // (e.g., sending menu selections from a test harness). Would need:
        //   - JSON message parsing/generation
        //   - Client connection state management
        //   - XLRC (Xbox Live Real-time Communication) stub layer
        {0x82606078, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
        {0x82605960, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
        {0x82605DF8, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},
    };

    // Log a stable .text fingerprint to support future resolver matching.
    Dc3TextSectionInfo text_info;
    if (auto* xex = module->xex_module()) {
      if (auto* text = xex->GetPESection(".text")) {
        auto* text_mem = memory->TranslateVirtual<uint8_t*>(text->address);
        text_info.start = text->address;
        text_info.end = text->address + text->size;
        text_info.have_range = text->size != 0;
        if (text_mem && text->size) {
          uint64_t hash = UINT64_C(1469598103934665603);
          for (uint32_t i = 0; i < text->size; ++i) {
            hash ^= text_mem[i];
            hash *= UINT64_C(1099511628211);
          }
          text_info.fingerprint = hash;
          text_info.have_fingerprint = true;
          XELOGI("DC3: .text fingerprint addr={:08X} size=0x{:X} fnv1a64={:016X}",
                 text->address, text->size, hash);
        }
      }
    }

    // Two-pass approach: first check how many patch targets have zero-padding.
    // If many do, we're running a decomp/rebuilt XEX with different function
    // layout, and ALL patches use wrong addresses. In the original retail XEX,
    // no NUI function address would be zero-filled.
    int total_patches = static_cast<int>(sizeof(patches) / sizeof(patches[0]));
    int zero_count = 0;
    for (const auto& patch : patches) {
      auto* mem = memory->TranslateVirtual<uint8_t*>(patch.address);
      if (mem && xe::load_and_swap<uint32_t>(mem) == 0x00000000) {
        zero_count++;
      }
    }

    bool is_decomp_layout = (zero_count > total_patches / 4);
    std::string_view layout_reason = "zero-padding heuristic";
    // Reuse manifest loaded earlier (before NUI block).
    auto& patch_manifest = dc3_patch_manifest;
    const bool explicit_patch_manifest_path =
        !cvars::dc3_nui_patch_manifest_path.empty();
    std::optional<Dc3FingerprintCache> fingerprint_cache;
    std::filesystem::path fingerprint_cache_path;
    if (!cvars::dc3_nui_layout_fingerprint_cache_path.empty()) {
      fingerprint_cache_path = cvars::dc3_nui_layout_fingerprint_cache_path;
    } else if (auto auto_cache_path = Dc3AutoProbeFingerprintCachePath()) {
      fingerprint_cache_path = *auto_cache_path;
    }
    if (!fingerprint_cache_path.empty()) {
      fingerprint_cache = Dc3LoadFingerprintCacheFile(fingerprint_cache_path);
      if (!fingerprint_cache) {
        XELOGW("DC3: Failed to load fingerprint cache file '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      } else {
        XELOGI("DC3: Loaded fingerprint cache '{}'",
               xe::path_to_utf8(fingerprint_cache_path));
      }
    }
    if (cvars::dc3_nui_patch_layout == "original") {
      is_decomp_layout = false;
      layout_reason = "forced by --dc3_nui_patch_layout=original";
    } else if (cvars::dc3_nui_patch_layout == "decomp") {
      is_decomp_layout = true;
      layout_reason = "forced by --dc3_nui_patch_layout=decomp";
    } else if (cvars::dc3_nui_patch_layout == "auto") {
      bool matched_manifest_layout = false;
      if (patch_manifest &&
          (patch_manifest->build_label == "decomp" ||
           patch_manifest->build_label == "original")) {
        const std::optional<uint64_t> manifest_runtime_fp =
            patch_manifest->runtime_text_fingerprint;
        const std::optional<uint64_t> manifest_compare_fp =
            manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                            : patch_manifest->text_fingerprint;
        if (text_info.have_fingerprint && manifest_compare_fp.has_value()) {
          if (*manifest_compare_fp == text_info.fingerprint) {
            if (patch_manifest->build_label == "decomp") {
              is_decomp_layout = true;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=decomp";
            } else {
              is_decomp_layout = false;
              layout_reason =
                  "matched patch manifest fingerprint/build_label=original";
            }
            matched_manifest_layout = true;
          } else if (explicit_patch_manifest_path) {
            XELOGW(
                "DC3: Patch manifest fingerprint {:016X} != runtime .text "
                "fingerprint {:016X}; trusting explicit manifest build_label={}",
                *manifest_compare_fp, text_info.fingerprint,
                patch_manifest->build_label);
            is_decomp_layout = patch_manifest->build_label == "decomp";
            layout_reason =
                "trusted explicit patch manifest build_label (fingerprint mismatch)";
            matched_manifest_layout = true;
          }
        } else if (explicit_patch_manifest_path) {
          XELOGW(
              "DC3: Patch manifest missing comparable fingerprint; trusting "
              "explicit manifest build_label={}",
              patch_manifest->build_label);
          is_decomp_layout = patch_manifest->build_label == "decomp";
          layout_reason =
              "trusted explicit patch manifest build_label (no fingerprint)";
          matched_manifest_layout = true;
        }
      }
      uint64_t fp_original = 0;
      uint64_t fp_decomp = 0;
      bool have_fp_original = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_original, &fp_original);
      bool have_fp_decomp = Dc3TryParseHexU64(
          cvars::dc3_nui_layout_fingerprint_decomp, &fp_decomp);
      if (!have_fp_original && fingerprint_cache &&
          fingerprint_cache->original.has_value()) {
        fp_original = *fingerprint_cache->original;
        have_fp_original = true;
      }
      if (!have_fp_decomp && fingerprint_cache &&
          fingerprint_cache->decomp.has_value()) {
        fp_decomp = *fingerprint_cache->decomp;
        have_fp_decomp = true;
      }
      if (!matched_manifest_layout && text_info.have_fingerprint && have_fp_original &&
          text_info.fingerprint == fp_original) {
        is_decomp_layout = false;
        layout_reason = "matched --dc3_nui_layout_fingerprint_original";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 have_fp_decomp &&
                 text_info.fingerprint == fp_decomp) {
        is_decomp_layout = true;
        layout_reason = "matched --dc3_nui_layout_fingerprint_decomp";
      } else if (!matched_manifest_layout && text_info.have_fingerprint &&
                 (have_fp_original || have_fp_decomp)) {
        const std::string fp_original_str = have_fp_original
                                                ? fmt::format("{:016X}", fp_original)
                                                : std::string("<unset>");
        const std::string fp_decomp_str = have_fp_decomp
                                              ? fmt::format("{:016X}", fp_decomp)
                                              : std::string("<unset>");
        XELOGI(
            "DC3: .text fingerprint {:016X} did not match configured layout "
            "fingerprints (original={} decomp={})",
            text_info.fingerprint, fp_original_str, fp_decomp_str);
      }
    } else {
      XELOGW(
          "DC3: Invalid --dc3_nui_patch_layout='{}' (expected auto|original|decomp); "
          "falling back to auto heuristic",
          cvars::dc3_nui_patch_layout);
    }
    XELOGI(
        "DC3: NUI patch layout={} reason={} (zero-padding {}/{})",
        is_decomp_layout ? "decomp" : "original", layout_reason, zero_count,
        total_patches);
    dc3_is_decomp_layout = is_decomp_layout;

    // Select the appropriate patch table based on XEX layout.
    const Dc3NuiPatchSpec* active_patches =
        is_decomp_layout ? kDc3DecompNuiPatches : patches;
    int active_count = is_decomp_layout ? kDc3DecompNuiPatchCount
                                        : total_patches;

    std::optional<Dc3NuiSymbolManifest> symbol_manifest;
    std::filesystem::path symbol_manifest_path;
    if (!cvars::dc3_nui_symbol_map_path.empty()) {
      symbol_manifest_path = cvars::dc3_nui_symbol_map_path;
    } else if (auto auto_path = Dc3AutoProbeNuiSymbolMapPath()) {
      symbol_manifest_path = *auto_path;
    }
    if (!symbol_manifest_path.empty()) {
      symbol_manifest = Dc3LoadNuiSymbolManifest(symbol_manifest_path);
      if (symbol_manifest) {
        XELOGI("DC3: Loaded NUI symbol manifest '{}' ({} .text symbols)",
               xe::path_to_utf8(symbol_manifest_path),
               symbol_manifest->text_symbols.size());
      } else {
        XELOGW("DC3: Failed to load NUI symbol manifest '{}'",
               xe::path_to_utf8(symbol_manifest_path));
      }
    }

    bool use_patch_manifest_targets = patch_manifest.has_value();
    if (patch_manifest && text_info.have_fingerprint) {
      const std::optional<uint64_t> manifest_runtime_fp =
          patch_manifest->runtime_text_fingerprint;
      const std::optional<uint64_t> manifest_compare_fp =
          manifest_runtime_fp.has_value() ? manifest_runtime_fp
                                          : patch_manifest->text_fingerprint;
      if (manifest_compare_fp.has_value() &&
          *manifest_compare_fp != text_info.fingerprint) {
        XELOGW(
            "DC3: Disabling patch manifest target resolution due fingerprint "
            "mismatch (manifest {:016X} != runtime {:016X}); "
            "falling back to symbol/signature/catalog",
            *manifest_compare_fp, text_info.fingerprint);
        use_patch_manifest_targets = false;
      }
    }

    std::string resolver_mode = cvars::dc3_nui_patch_resolver_mode;
    if (resolver_mode == "legacy") {
      XELOGW("DC3: --dc3_nui_patch_resolver_mode=legacy has been removed; "
             "using hybrid");
      resolver_mode = "hybrid";
    } else if (resolver_mode != "hybrid" && resolver_mode != "strict") {
      XELOGW("DC3: Unknown --dc3_nui_patch_resolver_mode='{}'; using hybrid",
             resolver_mode);
      resolver_mode = "hybrid";
    }
    Dc3RuntimeTelemetryConfig telemetry_config;
    telemetry_config.title_id = "373307D9";
    telemetry_config.build_kind = is_decomp_layout ? "decomp" : "original";
    telemetry_config.resolver_mode = resolver_mode;
    telemetry_config.signature_resolver = cvars::dc3_nui_enable_signature_resolver;
    telemetry_config.guest_overrides = true;
    Dc3RuntimeTelemetryBeginSession(telemetry_config);
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_block_begin");

    std::vector<Dc3ResolvedNuiPatch> resolved_patches;
    resolved_patches.reserve(active_count);
    int resolved_by_manifest = 0;
    int resolved_by_symbol = 0;
    int resolved_by_signature = 0;
    int resolved_by_catalog = 0;
    int resolver_strict_rejects = 0;
    for (int i = 0; i < active_count; ++i) {
      auto resolved = Dc3ResolveNuiPatchTarget(active_patches[i], text_info,
                                               use_patch_manifest_targets
                                                   ? &*patch_manifest
                                                   : nullptr,
                                               symbol_manifest ? &*symbol_manifest
                                                               : nullptr,
                                               resolver_mode,
                                               memory,
                                               cvars::dc3_nui_enable_signature_resolver);
      if (!resolved.resolved && resolved.strict_rejected) {
        resolver_strict_rejects++;
      } else if (resolved.resolved) {
        if (resolved.resolve_method == Dc3PatchResolveMethod::kPatchManifest) {
          resolved_by_manifest++;
        } else if (resolved.resolve_method == Dc3PatchResolveMethod::kSymbolMap) {
          resolved_by_symbol++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kSignatureStub) {
          resolved_by_signature++;
        } else if (resolved.resolve_method ==
                   Dc3PatchResolveMethod::kCatalogAddress) {
          resolved_by_catalog++;
        }
      }
      resolved_patches.push_back(resolved);
    }
    XELOGI(
        "DC3: NUI resolver summary mode={} manifest_hits={} symbol_hits={} "
        "signature_hits={} catalog_hits={} strict_rejects={} total={}",
        resolver_mode, resolved_by_manifest,
        resolved_by_symbol, resolved_by_signature,
        resolved_by_catalog, resolver_strict_rejects, active_count);
    Dc3RuntimeTelemetryRecordNuiResolverSummary(
        resolver_mode, resolved_by_manifest, resolved_by_symbol,
        resolved_by_signature, resolved_by_catalog, resolver_strict_rejects,
        active_count);
    if (cvars::dc3_nui_signature_trace) {
      auto should_trace_signature_target = [](std::string_view name) {
        return !name.empty();
      };
      auto log_words = [&](const char* label, uint32_t address) {
        if (!Dc3PatchTargetInText(text_info, address, 4)) {
          XELOGI("DC3: SignatureTrace {} {:08X} (outside .text)", label, address);
          return;
        }
        auto* mem = memory->TranslateVirtual<uint8_t*>(address);
        if (!mem) {
          XELOGI("DC3: SignatureTrace {} {:08X} (unmapped)", label, address);
          return;
        }
        std::string words;
        for (int j = 0; j < 12; ++j) {
          if (!Dc3PatchTargetInText(text_info, address + j * 4, 4)) {
            break;
          }
          const uint32_t w = xe::load_and_swap<uint32_t>(mem + j * 4);
          if (!words.empty()) {
            words.push_back(' ');
          }
          words += fmt::format("{:08X}", w);
        }
        XELOGI("DC3: SignatureTrace {} {:08X}: {}", label, address, words);
      };
      for (const auto& resolved_patch : resolved_patches) {
        const auto& patch = resolved_patch.spec;
        if (!should_trace_signature_target(patch.name)) {
          continue;
        }
        XELOGI("DC3: SignatureTrace target={} resolver={} resolved={} "
               "catalog={:08X} resolved_addr={:08X}",
               patch.name, Dc3PatchResolveMethodName(resolved_patch.resolve_method),
               resolved_patch.resolved ? 1 : 0, patch.address,
               resolved_patch.resolved ? resolved_patch.resolved_address : 0);
        log_words("catalog", patch.address);
        if (resolved_patch.resolved && resolved_patch.resolved_address != patch.address) {
          log_words("resolved", resolved_patch.resolved_address);
        }
      }
    }

    const bool requested_guest_overrides = cvars::dc3_guest_overrides;
    const bool enable_guest_overrides = true;
    if (!requested_guest_overrides) {
      XELOGW(
          "DC3: DC3 NUI/XBC legacy byte-patch path has been removed; "
          "forcing guest overrides on (rollback by reverting commit)");
    }
    XELOGI("DC3: NUI/XBC apply path guest_overrides={} resolver_mode={} "
           "signature_resolver={}",
           enable_guest_overrides ? 1 : 0, resolver_mode,
           cvars::dc3_nui_enable_signature_resolver ? 1 : 0);

    processor->ClearGuestFunctionOverrides();
    auto guest_extern_handler_for_patch =
        [&](const Dc3NuiPatchSpec& patch) -> cpu::GuestFunction::ExternHandler {
      if (!enable_guest_overrides) {
        return nullptr;
      }
      // Preserve the original-layout fake skeleton path when enabled.
      if (cvars::fake_kinect_data && !is_decomp_layout &&
          std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        return Dc3NuiSequencerExtern;
      }
      if (patch.insn1 != kBlr) {
        return nullptr;
      }
      if (patch.insn0 == kLiR3_0) {
        return Dc3NuiReturnOkExtern;
      }
      if (patch.insn0 == kLiR3_Neg1) {
        return Dc3NuiReturnNeg1Extern;
      }
      if (patch.insn0 == kLiR3_1) {
        return Dc3NuiReturn1Extern;
      }
      return nullptr;
    };
    auto patch_target_in_text = [&](uint32_t address) -> bool {
      return Dc3PatchTargetInText(text_info, address);
    };
    int override_registered = 0;
    int override_unsupported = 0;
    int override_register_failed = 0;
    int override_register_non_text = 0;
    int override_register_unresolved = 0;
    for (int i = 0; i < active_count; i++) {
      const auto& resolved_patch = resolved_patches[i];
      const auto& patch = resolved_patch.spec;
      if (!resolved_patch.resolved) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unresolved target; resolver mode={})",
            patch.address, patch.name, resolver_mode);
        override_register_unresolved++;
        override_register_failed++;
        continue;
      }
      const uint32_t patch_addr = resolved_patch.resolved_address;
            if (std::string_view(patch.name) == "NuiSkeletonGetNextFrame") {
        auto* h = guest_extern_handler_for_patch(patch);
        if (h) {
          processor->RegisterGuestFunctionOverride(patch_addr, h, patch.name);
          XELOGI("DC3: ULTRA FORCED registration of NUI sequencer at {:08X}", patch_addr);
          override_registered++;
          continue;
        }
      }
      auto handler = guest_extern_handler_for_patch(patch);
      if (!handler) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(unsupported patch shape for override; legacy byte patch path "
            "removed)",
            patch_addr, patch.name);
        override_unsupported++;
        override_register_failed++;
        continue;
      }
      if (!patch_target_in_text(patch_addr)) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(outside .text range {:08X}-{:08X})",
            patch_addr, patch.name, text_info.start, text_info.end);
        override_register_non_text++;
        override_register_failed++;
        continue;
      }
      auto* heap = memory->LookupHeap(patch_addr);
      auto* mem = memory->TranslateVirtual<uint8_t*>(patch_addr);
      if (!heap || !mem) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(invalid guest address)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      uint32_t existing0 = xe::load_and_swap<uint32_t>(mem + 0);
      if (existing0 == 0x00000000) {
        XELOGW(
            "DC3: Guest override registration skipped {:08X}: {} "
            "(zero-filled target)",
            patch_addr, patch.name);
        override_register_failed++;
        continue;
      }
      processor->RegisterGuestFunctionOverride(patch_addr, handler,
                                                std::string(patch.name));
      XELOGI("DC3: Registered guest extern override {:08X}: {} (resolver={})",
             patch_addr, patch.name,
             Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      Dc3RuntimeTelemetryRecordNuiOverrideRegistered(
          patch.name, patch_addr,
          Dc3PatchResolveMethodName(resolved_patch.resolve_method));
      override_registered++;
    }
    XELOGI(
        "DC3: Registered {} guest extern overrides from NUI patch table "
        "({} entries not overridden, {} registration failures, "
        "{} outside .text, {} unresolved)",
        override_registered, active_count - override_registered,
        override_register_failed, override_register_non_text,
        override_register_unresolved);

    const int patched = 0;
    const int overridden = override_registered;
    const int skipped = active_count - overridden;
    XELOGI(
        "DC3: NUI patch/override summary: patched={} overridden={} skipped={} "
        "total={} layout={} unsupported_override_entries={} "
        "override_registration_failures={} "
        "override_registration_non_text={} skipped_unresolved={} "
        "legacy_byte_patching_removed=1",
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original", override_unsupported,
        override_register_failed, override_register_non_text,
        override_register_unresolved);
    Dc3RuntimeTelemetryRecordNuiPatchSummary(
        patched, overridden, skipped, active_count,
        is_decomp_layout ? "decomp" : "original");
    Dc3RuntimeTelemetryRecordBootMilestone("dc3_nui_patch_apply_complete");

    // Fake Kinect skeleton data injection / SkeletonUpdate patches
    // are extracted into dc3_hack_pack for the same reason as the non-NUI
    // workarounds: keep emulator.cc orchestration-focused and preserve an
    // explicit retirement path.
    {
      Dc3HackContext dc3_skeleton_ctx;
      dc3_skeleton_ctx.memory = memory;
      dc3_skeleton_ctx.processor = processor;
      dc3_skeleton_ctx.module = module;
      dc3_skeleton_ctx.is_decomp_layout = is_decomp_layout;
      dc3_skeleton_ctx.is_headless = headless;
      auto skel_result = ApplyDc3SkeletonHackPack(dc3_skeleton_ctx);
      XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
             Dc3HackCategoryName(skel_result.category), skel_result.applied,
             skel_result.skipped, skel_result.failed);
    }
  }

  //
  // Fix CRT XapiCallThreadNotifyRoutines hang for the DC3 decomp XEX.
  //
  // The decomp's CRT has an uninitialized LIST_ENTRY at 0x83B14C3C
  // (XapiThreadNotifyRoutineList). On a real Xbox 360, this would be
  // statically initialized to point to itself (empty circular list). In
  // the decomp build, it contains garbage, causing
  // XapiCallThreadNotifyRoutines (0x82F51108) to iterate a corrupt list
  // and spin forever trying to call null callback pointers.
  //
  // Non-NUI DC3 workarounds (CRT/imports/debug/decomp runtime stopgaps) are
  // extracted into the DC3 hack pack module to keep emulator.cc orchestration-
  // only and make retirement tracking manageable.
  if (title_id.has_value() && title_id.value() == 0x373307D9 &&
      dc3_is_decomp_layout) {
    ApplyDc3DecompHackPack(memory, processor, module, headless,
                           dc3_is_decomp_layout, dc3_patch_manifest);
  } else if (title_id.has_value() && title_id.value() == 0x373307D9) {
    XELOGI(
        "DC3: Skipping hack pack for original XEX (NUI overrides already applied)");
    XELOGI("DC3: Original-XEX boot patch staging begins "
           "(fake_kinect_data={} decomp_layout={})",
           cvars::fake_kinect_data, dc3_is_decomp_layout);
    auto with_patch_target =
        [&](const char* label, uint32_t addr, size_t size, auto&& apply) {
          auto* ptr = memory->TranslateVirtual<uint8_t*>(addr);
          if (!ptr) {
            XELOGW("DC3: Patch lookup failed: {} at {:08X} "
                   "(TranslateVirtual returned null)",
                   label, addr);
            return false;
          }
          auto* heap = memory->LookupHeap(addr);
          if (!heap) {
            XELOGW("DC3: Patch lookup failed: {} at {:08X} "
                   "(LookupHeap returned null)",
                   label, addr);
            return false;
          }
          heap->Protect(addr, size, kMemoryProtectRead | kMemoryProtectWrite);
          apply(ptr);
          return true;
        };

    constexpr uint32_t kSaveLoadManagerActivate = 0x82894A10;
    with_patch_target("SaveLoadManager::Activate", kSaveLoadManagerActivate, 4,
                      [&](uint8_t* sla_ptr) {
                        xe::store_and_swap<uint32_t>(sla_ptr, 0x4E800020);
                        XELOGI(
                            "DC3: Stubbed SaveLoadManager::Activate at {:08X} to blr",
                            kSaveLoadManagerActivate);
                      });

    constexpr uint32_t kHamPanelFocusComponent = 0x828EFE90;
    constexpr uint32_t kUIPanelFocusComponent = 0x827A6310;
    with_patch_target("HamPanel::FocusComponent", kHamPanelFocusComponent, 4,
                      [&](uint8_t* ptr) {
                        constexpr uint32_t kBranchMask = 0x03FFFFFC;
                        uint32_t branch =
                            0x48000000 |
                            ((kUIPanelFocusComponent - kHamPanelFocusComponent) &
                             kBranchMask);
                        xe::store_and_swap<uint32_t>(ptr, branch);
                        XELOGI("DC3: UI fix: redirected HamPanel::FocusComponent "
                               "at {:08X} to UIPanel::FocusComponent {:08X}",
                               kHamPanelFocusComponent, kUIPanelFocusComponent);
                      });

    constexpr uint32_t kHamScreenIsEventDialogOnTop = 0x829626D8;
    with_patch_target("HamScreen::IsEventDialogOnTop",
                      kHamScreenIsEventDialogOnTop, 8, [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38600000);
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x4E800020);
                        XELOGI("DC3: UI fix: stubbed HamScreen::IsEventDialogOnTop "
                               "at {:08X} to return false",
                               kHamScreenIsEventDialogOnTop);
                      });

    constexpr uint32_t kCDReadDone = 0x826026E0;
    with_patch_target("CDReadDone", kCDReadDone, 8, [&](uint8_t* cdr_ptr) {
      xe::store_and_swap<uint32_t>(cdr_ptr + 0, 0x38600001);
      xe::store_and_swap<uint32_t>(cdr_ptr + 4, 0x4E800020);
      XELOGI("DC3: Stubbed CDReadDone at {:08X} to return true",
             kCDReadDone);
    });

    constexpr uint32_t kContentMgrRefreshDone = 0x825FEB48;
    with_patch_target("ContentMgr::RefreshDone", kContentMgrRefreshDone, 8,
                      [&](uint8_t* crd_ptr) {
                        xe::store_and_swap<uint32_t>(crd_ptr + 0, 0x38600001);
                        xe::store_and_swap<uint32_t>(crd_ptr + 4, 0x4E800020);
                        XELOGI("DC3: Stubbed ContentMgr::RefreshDone at {:08X} "
                               "to return true",
                               kContentMgrRefreshDone);
                      });

    constexpr uint32_t kSplashPrepareNext = 0x82554388;
    with_patch_target("Splash::PrepareNext", kSplashPrepareNext, 8,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38600000);
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::PrepareNext "
                               "at {:08X} to return false",
                               kSplashPrepareNext);
                      });

    constexpr uint32_t kSplashBeginSplasher = 0x825554C8;
    with_patch_target("Splash::BeginSplasher", kSplashBeginSplasher, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::BeginSplasher "
                               "at {:08X} to blr",
                               kSplashBeginSplasher);
                      });

    constexpr uint32_t kSplashSuspend = 0x82553BE0;
    with_patch_target("Splash::Suspend", kSplashSuspend, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::Suspend "
                               "at {:08X} to blr",
                               kSplashSuspend);
                      });

    constexpr uint32_t kSplashResume = 0x82553D68;
    with_patch_target("Splash::Resume", kSplashResume, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Splash bypass: stubbed Splash::Resume "
                               "at {:08X} to blr",
                               kSplashResume);
                      });

    constexpr uint32_t kSpeechGrammarUnload = 0x82439F38;
    with_patch_target("SpeechMgr::Grammar::Unload", kSpeechGrammarUnload, 4,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr, 0x4E800020);
                        XELOGI("DC3: Speech fix: stubbed "
                               "SpeechMgr::Grammar::Unload at {:08X} to blr",
                               kSpeechGrammarUnload);
                      });

    constexpr uint32_t kMovieInit = 0x82555678;
    // Intentionally NOT stubbed. Movie::Init() is just `{ TheMovieSys.Init(); }`,
    // the boot's entry into movie-system init. The old `blr` stub here was the
    // root of the regression: it skipped TheMovieSys.Init() entirely, so
    // BinkMovieSys::Init never ran, isInitalized stayed false, and the guest's
    // MILO_ASSERT(TheMovieSys.IsInitialized()) in Movie::BeginFromFile (line 220)
    // fired fatally on the attract movie. Letting Movie::Init run now dispatches
    // to BinkMovieSys::Init (patched below to set isInitalized=1 and return
    // before the hanging BinkStartAsyncThread), which is the whole point.
    XELOGI("DC3: Movie bypass: leaving Movie::Init at {:08X} intact so it "
           "calls TheMovieSys.Init() (BinkMovieSys::Init patched to set flag)",
           kMovieInit);

    constexpr uint32_t kBinkMovieSysInit = 0x82E214A8;
    // Set TheMovieSys.isInitalized = true instead of a bare blr. The old blr
    // stub skipped MovieSys::Init() entirely, so isInitalized stayed false and
    // the guest's MILO_ASSERT(TheMovieSys.IsInitialized()) (Movie.cpp:220)
    // fired fatally at boot, freezing the whole game on the debug error screen.
    // MovieSys layout: vptr @0, isInitalized(bool) @4; r3 == this (BinkMovieSys
    // base coincides with the MovieSys base). We still skip BinkStartAsyncThread
    // (the part that hangs headless and the reason Init was stubbed at all).
    with_patch_target("BinkMovieSys::Init", kBinkMovieSysInit, 12,
                      [&](uint8_t* ptr) {
                        xe::store_and_swap<uint32_t>(ptr + 0, 0x38000001);  // li  r0, 1
                        xe::store_and_swap<uint32_t>(ptr + 4, 0x98030004);  // stb r0, 4(r3)
                        xe::store_and_swap<uint32_t>(ptr + 8, 0x4E800020);  // blr
                        XELOGI("DC3: Movie bypass: BinkMovieSys::Init at {:08X} "
                               "now sets isInitalized=1 (was blr)",
                               kBinkMovieSysInit);
                      });

    if (cvars::fake_kinect_data) {
      XELOGI("DC3: Entering original-XEX fake Kinect patch block");

      constexpr uint32_t kSetPlayerPresentGuard = 0x8290834C;
      constexpr uint32_t kExpectedInsn = 0x4800001D;
      auto* guard_ptr =
          memory->TranslateVirtual<uint8_t*>(kSetPlayerPresentGuard);
      if (guard_ptr) {
        uint32_t actual = xe::load_and_swap<uint32_t>(guard_ptr);
        if (actual == kExpectedInsn) {
          auto* heap = memory->LookupHeap(kSetPlayerPresentGuard);
          if (heap) {
            heap->Protect(kSetPlayerPresentGuard, 4,
                          kMemoryProtectRead | kMemoryProtectWrite);
            xe::store_and_swap<uint32_t>(guard_ptr, 0x60000000);
            XELOGI("DC3: Calibration bypass: NOP'd IsTrackingAllSkeletons "
                   "guard in SetPlayerPresent at {:08X}",
                   kSetPlayerPresentGuard);
          }
        } else {
          XELOGW("DC3: Calibration bypass: unexpected insn at {:08X}: "
                 "{:08X} (expected {:08X})",
                 kSetPlayerPresentGuard, actual, kExpectedInsn);
        }
      }

      constexpr uint32_t kChoosePlayerSides = 0x82909968;
      with_patch_target("ChoosePlayerSides", kChoosePlayerSides, 4,
                        [&](uint8_t* cps_ptr) {
                          xe::store_and_swap<uint32_t>(cps_ptr, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "ChoosePlayerSides at {:08X} to blr",
                                 kChoosePlayerSides);
                        });

      constexpr uint32_t kSetPlayerSkeletonWarningData = 0x82907880;
      with_patch_target("SetPlayerSkeletonWarningData",
                        kSetPlayerSkeletonWarningData, 4,
                        [&](uint8_t* spw_ptr) {
                          xe::store_and_swap<uint32_t>(spw_ptr, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "SetPlayerSkeletonWarningData at {:08X} to blr",
                                 kSetPlayerSkeletonWarningData);
                        });

      constexpr uint32_t kSetPlayerSkeletonNavData = 0x82909340;
      constexpr uint32_t kSetPlayerPresent = 0x82908320;
      auto* nav_ptr =
          memory->TranslateVirtual<uint8_t*>(kSetPlayerSkeletonNavData);
      if (nav_ptr) {
        auto* heap = memory->LookupHeap(kSetPlayerSkeletonNavData);
        if (heap) {
          heap->Protect(kSetPlayerSkeletonNavData, 64,
                        kMemoryProtectRead | kMemoryProtectWrite);
          auto w = [nav_ptr](int idx, uint32_t insn) {
            xe::store_and_swap<uint32_t>(nav_ptr + idx * 4, insn);
          };
          int i = 0;
          w(i++, 0x7C0802A6);
          w(i++, 0x90010004);
          w(i++, 0x9421FFC0);
          w(i++, 0x38600000);
          w(i++, 0x38800001);
          w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909350));
          w(i++, 0x38600001);
          w(i++, 0x38800001);
          w(i++, 0x48000001 | (kSetPlayerPresent - 0x82909358));
          w(i++, 0x38210040);
          w(i++, 0x80010004);
          w(i++, 0x7C0803A6);
          w(i++, 0x4E800020);
          XELOGI("DC3: Calibration bypass: replaced SetPlayerSkeletonNavData "
                 "at {:08X} with SetPlayerPresent stub ({} instructions)",
                 kSetPlayerSkeletonNavData, i);
        }
      }

      constexpr uint32_t kShouldWaitForRecovery = 0x82904CD0;
      with_patch_target("ShouldWaitForRecovery", kShouldWaitForRecovery, 8,
                        [&](uint8_t* swr_ptr) {
                          xe::store_and_swap<uint32_t>(swr_ptr + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(swr_ptr + 4, 0x4E800020);
                          XELOGI("DC3: Calibration bypass: stubbed "
                                 "ShouldWaitForRecovery at {:08X} to return false",
                                 kShouldWaitForRecovery);
                        });

      constexpr uint32_t kExitControllerMode = 0x82902748;
      with_patch_target("ExitControllerMode", kExitControllerMode, 4,
                        [&](uint8_t* ecm_ptr) {
                          xe::store_and_swap<uint32_t>(ecm_ptr, 0x4E800020);
                          XELOGI("DC3: Controller bypass: stubbed "
                                 "ExitControllerMode at {:08X} to blr",
                                 kExitControllerMode);
                        });

      // Blocker A (early auto-pause during gameplay): with --fake_kinect_data,
      // the synthetic skeleton is never registered as a "playing" player, so
      // Game::CheckForSkeletonLoss() sees numPlaying(0) < threshold(1) every
      // SkeletonUpdate and calls Game::PauseForSkeletonLoss() ~2s into the song
      // -> Handle(pause_game) -> perform_pause_screen, killing playback.
      // Confirmed root cause: PAUSE-ONSET DIAG showed the UIEventMgr dialog
      // queue EMPTY at the pause onset (qsize=0), ruling out GamePanel::Poll's
      // HasActiveDialogEvent() branch and pinning it on the skeleton-loss path.
      // Real Kinect would mark the player present and this never fires; under
      // fake input it's a false positive. Stub the void Game::PauseForSkeletonLoss
      // (private, non-virtual; 0x82866D50) to a bare blr so the song keeps
      // playing. Scoped to the fake-Kinect block since that's the only case that
      // produces the false skeleton loss. The player-count computation in
      // CheckForSkeletonLoss is left intact; only the pause action is removed.
      constexpr uint32_t kPauseForSkeletonLoss = 0x82866D50;
      with_patch_target("Game::PauseForSkeletonLoss", kPauseForSkeletonLoss, 4,
                        [&](uint8_t* pfsl_ptr) {
                          xe::store_and_swap<uint32_t>(pfsl_ptr, 0x4E800020);
                          XELOGI("DC3: Gameplay fix: stubbed "
                                 "Game::PauseForSkeletonLoss at {:08X} to blr "
                                 "(suppress fake-Kinect false skeleton-loss "
                                 "auto-pause)",
                                 kPauseForSkeletonLoss);
                        });

      constexpr uint32_t kMoviePoll = 0x82555CB8;
      with_patch_target("Movie::Poll", kMoviePoll, 8,
                        [&](uint8_t* mp_ptr) {
                          xe::store_and_swap<uint32_t>(mp_ptr + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(mp_ptr + 4, 0x4E800020);
                          XELOGI("DC3: Movie bypass: stubbed Movie::Poll at "
                                 "{:08X} to return false",
                                 kMoviePoll);
                        });

      auto patch4 = [&](uint32_t addr, uint32_t val, const char* desc) {
        auto* p = memory->TranslateVirtual<uint8_t*>(addr);
        if (!p) {
          return;
        }
        auto* h = memory->LookupHeap(addr);
        if (!h) {
          return;
        }
        h->Protect(addr, 4, kMemoryProtectRead | kMemoryProtectWrite);
        xe::store_and_swap<uint32_t>(p, val);
        XELOGI("DC3: Audio fix: {} at {:08X}", desc, addr);
      };

      constexpr uint32_t kXMAHALAlloc = 0x82E77250;
      with_patch_target("XMAHALAllocateContexts", kXMAHALAlloc, 8,
                        [&](uint8_t* p) {
                          xe::store_and_swap<uint32_t>(p + 0, 0x38600000);
                          xe::store_and_swap<uint32_t>(p + 4, 0x4E800020);
                          XELOGI("DC3: Audio fix: stubbed XMAHALAllocateContexts "
                                 "at {:08X} to return S_OK",
                                 kXMAHALAlloc);
                        });

      // The stub above leaves the XMA HAL with no contexts, which is only
      // survivable while the guest's audio render callback never runs. The
      // paced nop audio driver (77d85acaa) made it run: ~15 s into boot the
      // callback faults in XMAHALWriteAndUnlockContexts (0x82E77C64) holding
      // a lock that D3DDevice_Resume then waits on forever, so the main
      // thread stops (no System::Poll, no DTA channel, menus only advance
      // through the forced-transition fallbacks, game_screen never loads).
      // Keep this title on the dummy driver unless explicitly asked for
      // --nop_audio_driver=paced.
      if (cvars::nop_audio_driver == "auto") {
        cvars::nop_audio_driver = "dummy";
        XELOGI("DC3: Audio fix: --nop_audio_driver auto -> dummy (XMA HAL "
               "contexts are stubbed; the render callback must not run)");
      }

      patch4(0x82867288 + 0x90, 0x48000024,
             "HandleWait+0x90: bne 40820024 -> b 48000024");
      patch4(0x8252B9E0 + 0x70, 0x38600001,
             "HamAudio::IsReady+0x70: bctrl -> li r3,1");

      constexpr uint32_t kHamDirectorSongAnim = 0x82475578;
      with_patch_target("HamDirector::SongAnim", kHamDirectorSongAnim, 8,
                        [&](uint8_t* p) {
                          // SongAnim(playerIndex): force the pre-authored EXPERT
                          // song.anim (which has baked clip keyframes) instead of
                          // the routine-builder anim (empty headless — the
                          // remixer never runs). Mirrors the #ifdef HX_NATIVE
                          // fallback compiled out of debug.xex.
                          //   li r4,2 (kDifficultyExpert)
                          //   b  0x82473E58  (HamDirector::SongAnimByDifficulty)
                          // NOTE: branch MUST target the function ENTRY 0x82473E58
                          // (0x4BFFE8DC), NOT 0x82473E5C/+4 (0x4BFFE8E0). The +4
                          // target landed on the SongAnimByDifficulty survival
                          // patch's `blr`, skipping `li r3,0`, so SongAnim returned
                          // r3 unchanged == TheHamDirector -> ClipPlayer::Init then
                          // called GetKeys with this==HamDirector -> infinite hang.
                          // (Survival patch now removed; SongAnimByDifficulty runs
                          // its real `return mSongAnims[diff]` on the healthy map.)
                          xe::store_and_swap<uint32_t>(p + 0, 0x38800002);
                          xe::store_and_swap<uint32_t>(p + 4, 0x4BFFE8DC);
                          XELOGI("DC3: Anim fix: patched HamDirector::SongAnim "
                                 "at {:08X} to tail-call SongAnimByDifficulty"
                                 "(expert)",
                                 kHamDirectorSongAnim);
                        });
    }

    // Apply IK telemetry instrumentation to the original XEX if requested.
    if (cvars::dc3_ik_telemetry) {
      Dc3HackContext ik_ctx;
      ik_ctx.memory = memory;
      ik_ctx.processor = processor;
      ik_ctx.module = module;
      ik_ctx.is_decomp_layout = false;
      auto ik_result = ApplyDc3IKTelemetry(ik_ctx);
      XELOGI("DC3: IK telemetry (original XEX): applied={} skipped={} failed={}",
             ik_result.applied, ik_result.skipped, ik_result.failed);
    }

#if XE_PLATFORM_LINUX
    // dc3-oracle: DTA evaluation channel (default off => no override, no
    // behaviour change). See src/xenia/titles/dc3/dc3_dta_channel.h.
    if (!cvars::dc3_dta_channel.empty()) {
      Dc3DtaChannelInstall(processor, memory,
                           cvars::dc3_dta_channel);
    }
#endif  // XE_PLATFORM_LINUX
  }
}

namespace {

void Dc3OnLaunchPath() { Dc3RuntimeTelemetryEndSession("launch_path_reset"); }

void Dc3OnTerminateTitle() {
  Dc3RuntimeTelemetryEndSession("terminate_title");
}

}  // namespace

void RegisterDc3TitleHooks() {
  titles::TitleHooks hooks;
  hooks.name = "dc3";
  hooks.apply_launch_hooks = ApplyDc3LaunchHooks;
  hooks.on_launch_path = Dc3OnLaunchPath;
  hooks.on_terminate_title = Dc3OnTerminateTitle;
  titles::RegisterTitleHooks(hooks);
}

}  // namespace xe
