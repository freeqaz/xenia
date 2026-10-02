/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 decomp-layout launch pieces (NOT upstream).
 *
 * See dc3_decomp_launch.h.
 ******************************************************************************
 */

#include "xenia/titles/dc3/decomp/dc3_decomp_launch.h"

#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/titles/dc3/dc3_hack_pack.h"
#include "xenia/titles/dc3/dc3_runtime_telemetry.h"

DECLARE_bool(dc3_ik_telemetry);

namespace xe {

namespace {
// PPC instructions (big-endian), as in the NUI block of dc3_title.cc.
const uint32_t kLiR3_0 = 0x38600000;    // li r3, 0  (return S_OK / 0)
const uint32_t kLiR3_Neg1 = 0x3860FFFF; // li r3, -1 (return E_UNEXPECTED)
const uint32_t kBlr = 0x4E800020;       // blr

}  // namespace

// Decomp XEX patch table: NUI functions at decomp MAP public symbol
// addresses. Used when the original-address patches don't match the
// loaded XEX layout. Addresses sourced from manifest generator
// (generate_xenia_dc3_patch_manifest.py) which reads the MAP file.
// When the manifest JSON is available, the resolver prefers manifest
// addresses over these catalog entries.
const dc3::Dc3NuiPatchSpec kDc3DecompNuiPatches[] = {
    // Core lifecycle (nuiruntime.obj)
    {0x835D8B0C, kLiR3_0, kBlr, "NuiInitialize"},
    {0x835D66D0, kLiR3_0, kBlr, "NuiShutdown"},
    {0x8373F0E0, kLiR3_0, kBlr, "NuiMetaCpuEvent"},

    // Skeleton tracking (nuiskeleton.obj)
    {0x835CA2D8, kLiR3_0, kBlr, "NuiSkeletonTrackingEnable"},
    {0x835C9B28, kLiR3_0, kBlr, "NuiSkeletonTrackingDisable"},
    {0x835C9C98, kLiR3_0, kBlr, "NuiSkeletonSetTrackedSkeletons"},
    {0x835CA478, kLiR3_Neg1, kBlr, "NuiSkeletonGetNextFrame"},

    // Image streams (nuiimagecamera.obj)
    {0x835D0F7C, kLiR3_0, kBlr, "NuiImageStreamOpen"},
    {0x835D0354, kLiR3_Neg1, kBlr, "NuiImageStreamGetNextFrame"},
    {0x835D067C, kLiR3_0, kBlr, "NuiImageStreamReleaseFrame"},
    {0x835D0E18, kLiR3_0, kBlr,
     "NuiImageGetColorPixelCoordinatesFromDepthPixel"},

    // Audio (nuiaudio.obj)
    {0x8360B778, kLiR3_Neg1, kBlr, "NuiAudioCreate"},
    {0x8360B198, kLiR3_Neg1, kBlr, "NuiAudioCreatePrivate"},
    {0x8360B07C, kLiR3_0, kBlr, "NuiAudioRegisterCallbacks"},
    {0x8360B0F4, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacks"},
    {0x83609818, kLiR3_0, kBlr, "NuiAudioRegisterCallbacksPrivate"},
    {0x83609880, kLiR3_0, kBlr, "NuiAudioUnregisterCallbacksPrivate"},
    {0x8360AB98, kLiR3_0, kBlr, "NuiAudioRelease"},

    // Camera properties (nuidetroit.obj, nuiimagecameraproperties.obj)
    {0x835CFBC4, kLiR3_0, kBlr, "NuiCameraSetProperty"},
    {0x835CBBB4, kLiR3_0, kBlr, "NuiCameraElevationGetAngle"},
    {0x835CBC94, kLiR3_0, kBlr, "NuiCameraElevationSetAngle"},
    {0x835CC5E0, kLiR3_0, kBlr, "NuiCameraAdjustTilt"},
    {0x835CCAD0, kLiR3_0, kBlr, "NuiCameraGetNormalToGravity"},
    {0x835CFC1C, kLiR3_0, kBlr, "NuiCameraSetExposureRegionOfInterest"},
    {0x835CE4DC, kLiR3_0, kBlr, "NuiCameraGetExposureRegionOfInterest"},
    {0x835CECE0, kLiR3_0, kBlr, "NuiCameraGetProperty"},
    {0x835CECF0, kLiR3_0, kBlr, "NuiCameraGetPropertyF"},

    // Identity (identityapi.obj)
    {0x835CB384, kLiR3_0, kBlr, "NuiIdentityEnroll"},
    {0x835CB540, kLiR3_0, kBlr, "NuiIdentityIdentify"},
    {0x835CB668, kLiR3_0, kBlr, "NuiIdentityGetEnrollmentInformation"},
    {0x835CB87C, kLiR3_0, kBlr, "NuiIdentityAbort"},

    // Fitness (nuifitnessapi.obj, nuifitnessxam.obj)
    {0x835D9460, kLiR3_Neg1, kBlr, "NuiFitnessStartTracking"},
    {0x835D9728, kLiR3_Neg1, kBlr, "NuiFitnessPauseTracking"},
    {0x835D97F8, kLiR3_Neg1, kBlr, "NuiFitnessResumeTracking"},
    {0x835D98C8, kLiR3_Neg1, kBlr, "NuiFitnessStopTracking"},
    {0x83901464, kLiR3_Neg1, kBlr, "NuiFitnessGetCurrentFitnessData"},

    // Wave gesture (nuiwave.obj)
    {0x835D905C, kLiR3_Neg1, kBlr, "NuiWaveSetEnabled"},
    {0x835D8F6C, kLiR3_Neg1, kBlr, "NuiWaveGetGestureOwnerProgress"},

    // Head tracking (nuiheadposition.obj, nuiheadorientation.obj)
    {0x83293DE0, kLiR3_0, kBlr, "NuiHeadPositionDisable"},
    {0x835E18FC, kLiR3_0, kBlr, "NuiHeadOrientationDisable"},

    // Speech (xspeechapi.obj)
    {0x832C6B88, kLiR3_0, kBlr, "NuiSpeechEnable"},
    {0x832C5B90, kLiR3_0, kBlr, "NuiSpeechDisable"},
    {0x832C5BCC, kLiR3_0, kBlr, "NuiSpeechCreateGrammar"},
    {0x832C5B9C, kLiR3_0, kBlr, "NuiSpeechLoadGrammar"},
    {0x832C5BBC, kLiR3_0, kBlr, "NuiSpeechUnloadGrammar"},
    {0x832C4A7C, kLiR3_0, kBlr, "NuiSpeechCommitGrammar"},
    {0x832C30E8, kLiR3_0, kBlr, "NuiSpeechStartRecognition"},
    {0x832C49B8, kLiR3_0, kBlr, "NuiSpeechStopRecognition"},
    {0x832C3110, kLiR3_0, kBlr, "NuiSpeechSetEventInterest"},
    {0x832C30F8, kLiR3_0, kBlr, "NuiSpeechSetGrammarState"},
    {0x832C49D8, kLiR3_0, kBlr, "NuiSpeechSetRuleState"},
    {0x832C49F4, kLiR3_0, kBlr, "NuiSpeechCreateRule"},
    {0x832C4A18, kLiR3_0, kBlr, "NuiSpeechCreateState"},
    {0x832C4A34, kLiR3_0, kBlr, "NuiSpeechAddWordTransition"},
    {0x832C3120, kLiR3_Neg1, kBlr, "NuiSpeechGetEvents"},
    {0x832C49C8, kLiR3_0, kBlr, "NuiSpeechDestroyEvent"},
    {0x832C6AB4, kLiR3_0, kBlr, "NuiSpeechEmulateRecognition"},

    // SmartGlass (XBC) - (xbcimpl.obj)
    {0x8352C7A4, kLiR3_0, kBlr, "CXbcImpl::Initialize"},
    {0x8352C0AC, kLiR3_0, kBlr, "CXbcImpl::DoWork"},
    {0x8352C530, kLiR3_0, kBlr, "CXbcImpl::SendJSON"},

    // D3D NUI (nui.obj)
    {0x837948A8, kLiR3_0, kBlr, "D3DDevice_NuiInitialize"},
    {0x8378DC78, kLiR3_0, kBlr, "D3DDevice_NuiMetaData"},
    {0x83794920, kLiR3_0, kBlr, "D3DDevice_NuiStart"},
    {0x83794964, kLiR3_0, kBlr, "D3DDevice_NuiStop"},

    // Internal NUI (Nuip*) from manifest
    {0x835E2FE4, kLiR3_0, kBlr, "NuipBuildXamNuiFrameData"},
    {0x835CDF3C, kLiR3_0, kBlr, "NuipCameraGetExposureRegionOfInterest"},
    {0x835CE548, kLiR3_0, kBlr, "NuipCameraGetProperty"},
    {0x835CE850, kLiR3_0, kBlr, "NuipCameraGetPropertyF"},
    {0x8363A7B4, kLiR3_0, kBlr, "NuipCreateInstance"},
    {0x835D92EC, kLiR3_0, kBlr, "NuipFitnessInitialize"},
    {0x835D9B98, kLiR3_0, kBlr, "NuipFitnessNewSkeletalFrame"},
    {0x835D93C8, kLiR3_0, kBlr, "NuipFitnessShutdown"},
    {0x835D8120, kLiR3_0, kBlr, "NuipInitialize"},
    {0x83641870, kLiR3_0, kBlr, "NuipLoadRegistry"},
    {0x8363A844, kLiR3_0, kBlr, "NuipModuleInit"},
    {0x8363A9C0, kLiR3_0, kBlr, "NuipModuleTerm"},
    {0x83641650, kLiR3_0, kBlr, "NuipRegCreateKeyExW"},
    {0x83640328, kLiR3_0, kBlr, "NuipRegEnumKeyExW"},
    {0x83640420, kLiR3_0, kBlr, "NuipRegEnumValueW"},
    {0x836413E0, kLiR3_0, kBlr, "NuipRegOpenKeyExW"},
    {0x83640A58, kLiR3_0, kBlr, "NuipRegQueryValueExW"},
    {0x83641020, kLiR3_0, kBlr, "NuipRegSetValueExW"},
    {0x83640964, kLiR3_0, kBlr, "NuipUnloadRegistry"},
    {0x835D8DF8, kLiR3_0, kBlr, "NuipWaveInit"},
    {0x835D8E60, kLiR3_0, kBlr, "NuipWaveUpdate"},
};
const int kDc3DecompNuiPatchCount =
    static_cast<int>(sizeof(kDc3DecompNuiPatches) /
                     sizeof(kDc3DecompNuiPatches[0]));

void ApplyDc3DecompHackPack(
    Memory* memory, cpu::Processor* processor, kernel::UserModule* module,
    bool headless, bool dc3_is_decomp_layout,
    const std::optional<dc3::Dc3NuiPatchManifest>& dc3_patch_manifest) {
  Dc3HackContext dc3_hack_ctx;
  dc3_hack_ctx.memory = memory;
  dc3_hack_ctx.processor = processor;
  dc3_hack_ctx.module = module;
  dc3_hack_ctx.is_decomp_layout = dc3_is_decomp_layout;
  if (dc3_patch_manifest.has_value()) {
    if (!dc3_patch_manifest->hack_pack_stubs.empty()) {
      dc3_hack_ctx.hack_pack_stubs = &dc3_patch_manifest->hack_pack_stubs;
      XELOGI("DC3: Passing {} hack-pack stub addresses from manifest",
             dc3_patch_manifest->hack_pack_stubs.size());
    }
    if (!dc3_patch_manifest->crt_sentinels.empty()) {
      dc3_hack_ctx.crt_sentinels = &dc3_patch_manifest->crt_sentinels;
      XELOGI("DC3: Passing {} CRT sentinel addresses from manifest",
             dc3_patch_manifest->crt_sentinels.size());
    }
    if (!dc3_patch_manifest->xdk_overrides.empty()) {
      dc3_hack_ctx.xdk_overrides = &dc3_patch_manifest->xdk_overrides;
      XELOGI("DC3: Passing {} XDK override addresses from manifest",
             dc3_patch_manifest->xdk_overrides.size());
    }
    if (!dc3_patch_manifest->xdk_code_ranges.empty()) {
      // Reinterpret-cast is safe: both CodeRange structs have identical
      // layout {uint32_t start; uint32_t end;}.
      dc3_hack_ctx.xdk_code_ranges = reinterpret_cast<
          const std::vector<Dc3HackContext::CodeRange>*>(
          &dc3_patch_manifest->xdk_code_ranges);
      XELOGI("DC3: Passing {} XDK code ranges for prologue scanning",
             dc3_patch_manifest->xdk_code_ranges.size());
    }
  }
  // Populate kAddr from manifest address catalog (before hack pack applies).
  if (dc3_patch_manifest.has_value() &&
      !dc3_patch_manifest->address_catalog.empty()) {
    Dc3PopulateAddressesFromCatalog(dc3_patch_manifest->address_catalog,
                                    dc3_patch_manifest->crt_sentinels);
  }
  dc3_hack_ctx.is_headless = headless;
  auto dc3_hack_summary = ApplyDc3HackPack(dc3_hack_ctx);
  for (const auto& result : dc3_hack_summary.results) {
    XELOGD("DC3: hack-pack category={} applied={} skipped={} failed={}",
           Dc3HackCategoryName(result.category), result.applied,
           result.skipped, result.failed);
  }
  if (cvars::dc3_ik_telemetry) {
    auto ik_result = ApplyDc3IKTelemetry(dc3_hack_ctx);
    XELOGI("DC3: IK telemetry: applied={} skipped={} failed={}",
           ik_result.applied, ik_result.skipped, ik_result.failed);
  }
  Dc3RuntimeTelemetryRecordBootMilestone("dc3_hack_pack_apply_complete");
}

}  // namespace xe
