/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 */

#include "xenia/dc3_nui_patch_resolver.h"

#include <array>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#include "xenia/base/memory.h"
#include "xenia/memory.h"

namespace xe::dc3::test {
namespace {

struct GuestMemoryHarness {
  xe::Memory memory;
  uint32_t base = 0;
  uint32_t size = 0;

  GuestMemoryHarness(uint32_t alloc_size = 0x1000) : size(alloc_size) {
    REQUIRE(memory.Initialize());
    base = memory.SystemHeapAlloc(size, 0x1000, xe::kSystemHeapVirtual);
    REQUIRE(base != 0);
    memory.Zero(base, size);
  }

  ~GuestMemoryHarness() {
    if (base) {
      memory.SystemHeapFree(base);
    }
  }
};

Dc3TextSectionInfo MakeTextInfo(uint32_t base, uint32_t size) {
  Dc3TextSectionInfo info;
  info.have_range = true;
  info.start = base;
  info.end = base + size;
  return info;
}

void WriteWords(GuestMemoryHarness& harness, uint32_t address,
                const std::vector<uint32_t>& words) {
  auto* mem = harness.memory.TranslateVirtual<uint8_t*>(address);
  REQUIRE(mem != nullptr);
  for (size_t i = 0; i < words.size(); ++i) {
    xe::store_and_swap<uint32_t>(mem + i * 4, words[i]);
  }
}

struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const std::string& stem) {
    const auto now = std::chrono::high_resolution_clock::now()
                         .time_since_epoch()
                         .count();
    path = std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(now) + ".txt");
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

TEST_CASE("DC3 resolver hex parser parses and rejects invalid input",
          "[dc3_nui_patch_resolver]") {
  uint64_t value = 0;
  REQUIRE(Dc3TryParseHexU64("0x1234ABCD", &value));
  REQUIRE(value == 0x1234ABCDull);
  REQUIRE(Dc3TryParseHexU64("deadBEEF", &value));
  REQUIRE(value == 0xDEADBEEFull);
  REQUIRE_FALSE(Dc3TryParseHexU64("", &value));
  REQUIRE_FALSE(Dc3TryParseHexU64("0xZZ", &value));
  REQUIRE_FALSE(Dc3TryParseHexU64("0x1234 ", &value));
}

TEST_CASE("DC3 resolver fingerprint cache parser handles comments and values",
          "[dc3_nui_patch_resolver]") {
  TempFile file("dc3_fp_cache");
  {
    std::ofstream out(file.path);
    out << "# comment\n";
    out << "original = 0x1122334455667788\n";
    out << "decomp=0xAABBCCDDEEFF0011 # trailing\n";
  }
  auto parsed = Dc3LoadFingerprintCacheFile(file.path);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->original.has_value());
  REQUIRE(parsed->decomp.has_value());
  REQUIRE(*parsed->original == 0x1122334455667788ull);
  REQUIRE(*parsed->decomp == 0xAABBCCDDEEFF0011ull);
}

TEST_CASE("DC3 resolver manifest parser reads runtime fingerprint and targets",
          "[dc3_nui_patch_resolver]") {
  TempFile file("dc3_manifest");
  {
    std::ofstream out(file.path);
    out << R"JSON({
  "build_label": "decomp",
  "pe": {
    "text": {
      "fnv1a64": "0xA85033886A8BED5F",
      "xenia_runtime_fnv1a64": "0x28F813763596C6C6"
    }
  },
  "targets": {
    "NuiInitialize": { "address": "0x835D8B0C" }
  },
  "crt_sentinels": {
    "__xc_a": { "address": 2200000000 }
  }
})JSON";
  }
  auto parsed = Dc3LoadNuiPatchManifest(file.path);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->build_label == "decomp");
  REQUIRE(parsed->text_fingerprint.has_value());
  REQUIRE(*parsed->text_fingerprint == 0xA85033886A8BED5Full);
  REQUIRE(parsed->runtime_text_fingerprint.has_value());
  REQUIRE(*parsed->runtime_text_fingerprint == 0x28F813763596C6C6ull);
  REQUIRE(parsed->targets.count("NuiInitialize") == 1);
  REQUIRE(parsed->targets["NuiInitialize"] == 0x835D8B0Cu);
  REQUIRE(parsed->crt_sentinels.count("__xc_a") == 1);
}

TEST_CASE("DC3 resolver strict signature path resolves supported decomp target",
          "[dc3_nui_patch_resolver]") {
  GuestMemoryHarness harness;
  const uint32_t hint = harness.base + 0x100;
  // Decomp patch-site signature for NuiInitialize (runtime bytes).
  WriteWords(harness, hint,
             {0x7D8802A6, 0x4B5599F9, 0x9421FF50, 0x546B05AC, 0x7C7F1B78,
              0x7C972378});
  Dc3NuiPatchSpec spec{hint, 0, 0, "NuiInitialize"};
  auto result = Dc3ResolveNuiPatchTarget(spec, MakeTextInfo(harness.base, harness.size),
                                         nullptr, nullptr, "strict",
                                         &harness.memory, true);
  REQUIRE(result.resolved);
  REQUIRE(result.resolve_method == Dc3PatchResolveMethod::kSignatureStub);
  REQUIRE(result.resolved_address == hint);
}

TEST_CASE("DC3 resolver strict signature coverage matrix resolves supported targets",
          "[dc3_nui_patch_resolver]") {
  struct SigCase {
    const char* name;
    std::vector<uint32_t> words;
  };
  const std::array<SigCase, 59> original_cases = {{
      {"NuiInitialize",
       {0x7D8802A6, 0x4BFCC709, 0x9421FF50, 0x546B05AC, 0x7C7F1B78,
        0x7C972378}},
      {"NuiShutdown",
       {0x7D8802A6, 0x4BFCEB79, 0x9421FF50, 0x3D608312, 0x3B600000,
        0x3BEBC8A0}},
      {"CXbcImpl::Initialize",
       {0x7D8802A6, 0x483978A9, 0x9421FF80, 0x3D6082F1, 0x7C7D1B78, 0x3B6B2BEC,
        0x7C9C2378, 0x7F63DB78}},
      {"CXbcImpl::DoWork",
       {0x48004EA8, 0x00000000, 0x3D40830A, 0x39600000, 0x390AB060,
        0x39200000}},
      {"CXbcImpl::SendJSON",
       {0x7D8802A6, 0x48397B21, 0x9421FB60, 0x7C7F1B78, 0x7C9C2378, 0x7CBB2B78,
        0x39600400, 0x3BA00000}},
      {"NuiMetaCpuEvent",
       {0x7DEF7B78, 0x3C607DEF, 0x60637B78, 0x4E800020, 0x7D8802A6, 0x4BE463A9,
        0x9421FF70, 0x3FE08316}},
      {"NuiImageStreamOpen",
       {0x7D8802A6, 0x4BFD45D9, 0x9421FF40, 0x7C7D1B78, 0x7C9C2378, 0x7CBA2B78,
        0x7CDB3378, 0x7CF73B78}},
      {"NuiImageStreamGetNextFrame",
       {0x7D8802A6, 0x4BFD5229, 0x9421FF60, 0x7C9E2378, 0x7CBC2B78, 0x2B030000,
        0x409A0014, 0x3C608007}},
      {"NuiImageStreamReleaseFrame",
       {0x7D8802A6, 0x4BFD4F11, 0x9421FF80, 0x7C9F2378, 0x2B030000, 0x409A0014,
        0x3C608007, 0x60630057}},
      {"NuiAudioCreate",
       {0x39200000, 0x39000000, 0x4BFFFA18, 0x00000000, 0x7D8802A6, 0x9181FFF8,
        0xFBE1FFF0, 0x9421FFA0}},
      {"NuiAudioCreatePrivate",
       {0x7D8802A6, 0x4BF8FEB1, 0x9421FF20, 0x90860000, 0x3D6082F2, 0x7C751B78,
        0x3B8BFEB0, 0x7C9E2378}},
      {"NuiAudioRegisterCallbacks",
       {0x7D8802A6, 0x4BF90001, 0x9421FF90, 0x3D6082F2, 0x7C9E2378, 0x3BEBFFBC,
        0x7CBD2B78, 0x387FFFE4}},
      {"NuiAudioUnregisterCallbacks",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6082F2,
        0x7C9E2378, 0x3BEBFFBC}},
      {"NuiAudioRegisterCallbacksPrivate",
       {0x7D8802A6, 0x4BF91889, 0x9421FF90, 0x3D6082F2, 0x7C9F2378, 0x3BCBFFA0,
        0x7CBD2B78, 0x7FC3F378}},
      {"NuiAudioUnregisterCallbacksPrivate",
       {0x7D8802A6, 0x4BF91821, 0x9421FF90, 0x3D6082F2, 0x7C9D2378, 0x3BCBFFA0,
        0x7FC3F378, 0x484D9761}},
      {"NuiAudioRelease",
       {0x7D8802A6, 0x4BF904E1, 0x9421FF70, 0x3D6082F2, 0x83C30000, 0x7C7D1B78,
        0x3B8BFEA8, 0x387C00F8}},
      {"NuiSkeletonTrackingEnable",
       {0x7D8802A6, 0x4BFDB325, 0x9421FF60, 0x3D608312, 0x3B200000, 0x3BEBC8A0,
        0x7C7D1B78, 0x7C9E2378}},
      {"NuiSkeletonTrackingDisable",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6082F2,
        0x3BCBF650, 0x7FC3F378}},
      {"NuiSkeletonSetTrackedSkeletons",
       {0x7D8802A6, 0x4BFDB999, 0x9421FF90, 0x7C7E1B78, 0x2B030000, 0x409A0014,
        0x3C608007, 0x60630057}},
      {"NuiSkeletonGetNextFrame",
       {0x7D8802A6, 0x4BFDB18D, 0x9421FF60, 0x7C9A2378, 0x2B040000, 0x409A0014,
        0x3C608007, 0x60630057}},
      {"NuiImageGetColorPixelCoordinatesFromDepthPixel",
       {0x7D8802A6, 0x4BFD4751, 0x9421FF70, 0x7C9E2378, 0x7CBC2B78, 0x7CDB3378,
        0x7CFA3B78, 0x7D1F4378}},
      {"NuiCameraSetProperty",
       {0x7D8802A6, 0x4BFD59E1, 0x9421FF90, 0x7C7F1B78, 0x7C9E2378, 0x7CBD2B78,
        0x2F030000, 0x409A0020}},
      {"NuiCameraElevationGetAngle",
       {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x2B030000, 0x409A001C, 0x3C608007,
        0x60630057, 0x38210060}},
      {"NuiCameraElevationSetAngle",
       {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0, 0x7C7F1B78, 0x38600025,
        0x4852175D, 0x2B030000}},
      {"NuiCameraAdjustTilt",
       {0x7D8802A6, 0x4BFD8FE5, 0xDBA1FFC0, 0xDBC1FFC8, 0xDBE1FFD0, 0x9421FF70,
        0x7C7E1B78, 0xFFE00890}},
      {"NuiCameraGetNormalToGravity",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D608312,
        0x3D20821C, 0x3D008200}},
      {"NuiCameraSetExposureRegionOfInterest",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x7C7F1B78,
        0x7C9E2378, 0x2F030000}},
      {"NuiCameraGetExposureRegionOfInterest",
       {0x7C852378, 0x38800000, 0x4BFFFA48, 0x00000000, 0x7D8802A6, 0x9181FFF8,
        0xDBE1FFF0, 0x9421FF90}},
      {"NuiCameraGetProperty",
       {0x7CA62B78, 0x7C852378, 0x38800000, 0x4BFFF874, 0x7D8802A6, 0x9181FFF8,
        0xFBC1FFE8, 0xFBE1FFF0}},
      {"NuiCameraGetPropertyF",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D608312,
        0x7CBE2B78, 0x816BC89C}},
      {"NuiIdentityEnroll",
       {0x7D8802A6, 0x4BFDA269, 0x9421FF50, 0x54AB003C, 0x7C7C1B78, 0x7C9D2378,
        0x7CBE2B78, 0x7CDA3378}},
      {"NuiIdentityIdentify",
       {0x7D8802A6, 0x4BFDA0B1, 0x9421FF60, 0x3D608312, 0x7C7E1B78, 0x396BC8A0,
        0x7C9D2378, 0x7CBC2B78}},
      {"NuiIdentityGetEnrollmentInformation",
       {0x7D8802A6, 0x4BFD9F91, 0x9421FF80, 0x7C7D1B78, 0x7C9F2378, 0x2B030008,
        0x41980010, 0x3C608007}},
      {"NuiIdentityAbort",
       {0x38600000, 0x4BFFFEE4, 0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x81630008,
        0x81430004, 0x7D6907B4}},

  // BEGIN AUTOGEN TEST CASES: speech_fitness_wave_head_nuip original
      {"NuiFitnessStartTracking",
       {0x7D8802A6, 0x4BFCBDA5, 0xDBA1FF90, 0xDBC1FF98, 0xDBE1FFA0, 0x9421FF20, 0x3D608312, 0x3BA00000}},
      {"NuiFitnessPauseTracking",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D608313, 0x7C7E1B78, 0x3BEBCDA0}},
      {"NuiFitnessResumeTracking",
       {0x7D8802A6, 0x4BFCBA29, 0x9421FF80, 0x3D608313, 0x7C7E1B78, 0x3BEBCDA0, 0x7C9D2378, 0x387F0008}},
      {"NuiFitnessStopTracking",
       {0x7D8802A6, 0x4BFCB941, 0xDBC1FFA0, 0xDBE1FFA8, 0x9421FF30, 0x3D608313, 0x7C7E1B78, 0x3BEBCDA0}},
      {"NuiFitnessGetCurrentFitnessData",
       {0x7D8802A6, 0x4BB3C299, 0x9421FF80, 0x3D608313, 0x7C7F1B78, 0x3BABCDA0, 0x7C9E2378, 0x387D0008}},
      {"NuiWaveSetEnabled",
       {0x7D8802A6, 0x4BFCC1CD, 0x9421FF80, 0x3D608312, 0x7C7C1B78, 0x396BC8A0, 0x814B0874, 0x2F0A0000}},
      {"NuiWaveGetGestureOwnerProgress",
       {0x7D8802A6, 0x4BFCC2C1, 0x9421FF80, 0x7C7E1B78, 0x7C9F2378, 0x2B030000, 0x409A0010, 0x3C608007}},
      {"NuiHeadOrientationDisable",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6082F2, 0x3BCBFBF4, 0x7FC3F378}},
      {"NuiHeadPositionDisable",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6082F2, 0x3BCBF8FC, 0x7FC3F378}},
      {"NuiSpeechEnable",
       {0x7C862378, 0x2B030000, 0x409A0010, 0x3C608007, 0x60630057, 0x4E800020, 0x3D608316, 0x80A30004}},
      {"NuiSpeechDisable",
       {0x3D608316, 0x386BAA30, 0x4BFFEEE0, 0x00000000, 0x7C6B1B78, 0x7CC73378, 0x3D408316, 0x7CA62B78}},
      {"NuiSpeechCreateGrammar",
       {0x7C6B1B78, 0x3D408316, 0x7C852378, 0x386AAA30, 0x7D645B78, 0x4BFFF044, 0x7D8802A6, 0x9181FFF8}},
      {"NuiSpeechLoadGrammar",
       {0x7C6B1B78, 0x7CC73378, 0x3D408316, 0x7CA62B78, 0x7C852378, 0x386AAA30, 0x7D645B78, 0x4BFFF17C}},
      {"NuiSpeechUnloadGrammar",
       {0x3D608316, 0x7C641B78, 0x386BAA30, 0x4BFFF2CC, 0x7C6B1B78, 0x3D408316, 0x7C852378, 0x386AAA30}},
      {"NuiSpeechCommitGrammar",
       {0x3D608316, 0x7C641B78, 0x386BAA30, 0x4BFFF73C, 0x7D8802A6, 0x4BF7AECD, 0x9421FF70, 0x3BC00000}},
      {"NuiSpeechStartRecognition",
       {0x3D608316, 0x38800000, 0x386BAA30, 0x4BFFF70C, 0x7C6B1B78, 0x3D408316, 0x7C852378, 0x386AAA30}},
      {"NuiSpeechStopRecognition",
       {0x3D608316, 0x38800000, 0x386BAA30, 0x4BFFE84C, 0x3D608316, 0x7C641B78, 0x386BAA30, 0x4BFFF39C}},
      {"NuiSpeechSetEventInterest",
       {0x3D608316, 0x7C641B78, 0x386BAA30, 0x4BFFFB3C, 0x7C6B1B78, 0x3D408316, 0x7CA62B78, 0x7C852378}},
      {"NuiSpeechSetGrammarState",
       {0x7C6B1B78, 0x3D408316, 0x7C852378, 0x386AAA30, 0x7D645B78, 0x4BFFFABC, 0x3D608316, 0x7C641B78}},
      {"NuiSpeechSetRuleState",
       {0x7C6B1B78, 0x3D408316, 0x7CA62B78, 0x7C852378, 0x386AAA30, 0x7D645B78, 0x4BFFF3F8, 0x00000000}},
      {"NuiSpeechCreateRule",
       {0x7CE83B78, 0x7C6B1B78, 0x7CC73378, 0x3D408316, 0x7CA62B78, 0x7C852378, 0x386AAA30, 0x7D645B78}},
      {"NuiSpeechCreateState",
       {0x7C6B1B78, 0x3D408316, 0x7CA62B78, 0x7C852378, 0x386AAA30, 0x7D645B78, 0x4BFFF518, 0x00000000}},
      {"NuiSpeechAddWordTransition",
       {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x7D094378, 0x91410054, 0x7CE83B78, 0x7C6B1B78, 0x7CC73378}},
      {"NuiSpeechGetEvents",
       {0x7C6B1B78, 0x3D408316, 0x7CA62B78, 0x7C852378, 0x386AAA30, 0x7D645B78, 0x4BFFFC48, 0x00000000}},
      {"NuiSpeechDestroyEvent",
       {0x3D608316, 0x7C641B78, 0x386BAA30, 0x4BFFF39C, 0x7C6B1B78, 0x3D408316, 0x7CA62B78, 0x7C852378}},
  // END AUTOGEN TEST CASES: speech_fitness_wave_head_nuip original
  }};
  const std::array<SigCase, 88> decomp_cases = {{
      {"NuiInitialize",
       {0x7D8802A6, 0x4B5599F9, 0x9421FF50, 0x546B05AC, 0x7C7F1B78,
        0x7C972378}},
      {"NuiShutdown",
       {0x7D8802A6, 0x4B55BE45, 0x9421FF50, 0x3D6083C5, 0x3B600000,
        0x3BEB3EB8}},
      {"CXbcImpl::Initialize",
       {0x7D8802A6, 0x4B605D41, 0x9421FF80, 0x3D6083A3, 0x7C7D1B78, 0x3B6B7E28,
        0x7C9C2378, 0x7F63DB78}},
      {"CXbcImpl::DoWork",
       {0x48003DC8, 0x3D4083C2, 0x39600000, 0x390A8C10, 0x39200000,
        0x7D0A4378}},
      {"CXbcImpl::SendJSON",
       {0x7D8802A6, 0x4B605FAD, 0x9421FB60, 0x7C7F1B78, 0x7C9C2378, 0x7CBB2B78,
        0x39600400, 0x3BA00000}},
      {"NuiMetaCpuEvent",
       {0x7DEF7B78, 0x3C607DEF, 0x60637B78, 0x4E800020, 0x7D8802A6, 0x4B3F3351,
        0x9421FF70, 0x3FE083C9}},
      {"NuiMetaCpuEvent",
       {0x38210090, 0x4B3F3380, 0x3D6083C9, 0x7C641B78, 0x806BD3E8, 0x480073DC,
        0x3D6083C9, 0x806BD3E8, 0x4800740C, 0x4B8EBFEC, 0x7D8802A6, 0x4B3F3311}},
      {"D3DDevice_NuiInitialize",
       {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0, 0x7C7F1B78, 0x7DAD6B78,
        0x3D60822E, 0x816B8808}},
      {"D3DDevice_NuiInitialize",
       {0xEBC1FFE8, 0xEBE1FFF0, 0x4E800020, 0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0,
        0x9421FFA0, 0x7C7F1B78}},
      {"D3DDevice_NuiStart",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x7C7F1B78,
        0x7C9E2378, 0x7DAD6B78}},
      {"D3DDevice_NuiStop",
       {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0, 0x7C7F1B78, 0x7DAD6B78,
        0x39600000, 0x917F3670}},
      {"D3DDevice_NuiStop",
       {0x57E9043E, 0x7D6A5A2E, 0x7D6B4838, 0x556A043E, 0x5569ECFE, 0x556BA73E,
        0x7D295378, 0x5529E8FE, 0x7D295378, 0x5529E8FE, 0x7D2A5378, 0x554A073C}},
      {"NuiImageStreamOpen",
       {0x7D8802A6, 0x4B561589, 0x9421FF40, 0x7C7D1B78, 0x7C9C2378, 0x7CBA2B78,
        0x7CDB3378, 0x7CF73B78}},
      {"NuiImageStreamGetNextFrame",
       {0x7D8802A6, 0x4B5621C1, 0x9421FF60, 0x7C9E2378, 0x7CBC2B78, 0x2B030000,
        0x409A0014, 0x3C608007}},
      {"NuiImageStreamReleaseFrame",
       {0x7D8802A6, 0x4B561EA9, 0x9421FF80, 0x7C9F2378, 0x2B030000, 0x409A0014,
        0x3C608007, 0x60630057}},
      {"NuiAudioCreate",
       {0x39200000, 0x39000000, 0x4BFFFA18, 0x4E800020, 0x3D6083A4, 0x386BC608,
        0x4BFFF300, 0x3D6083A4}},
      {"NuiAudioCreatePrivate",
       {0x7D8802A6, 0x4B52735D, 0x9421FF20, 0x90860000, 0x3D6083A4, 0x7C751B78,
        0x3B8BC4E0, 0x7C9E2378}},
      {"NuiAudioRegisterCallbacks",
       {0x7D8802A6, 0x4B5274A9, 0x9421FF90, 0x3D6083A4, 0x7C9E2378, 0x3BEBC5EC,
        0x7CBD2B78, 0x387FFFE4}},
      {"NuiAudioUnregisterCallbacks",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083A4,
        0x7C9E2378, 0x3BEBC5EC}},
      {"NuiAudioRegisterCallbacksPrivate",
       {0x7D8802A6, 0x4B528D0D, 0x9421FF90, 0x3D6083A4, 0x7C9F2378, 0x3BCBC5D0,
        0x7CBD2B78, 0x7FC3F378}},
      {"NuiAudioUnregisterCallbacksPrivate",
       {0x7D8802A6, 0x4B528CA5, 0x9421FF90, 0x3D6083A4, 0x7C9D2378, 0x3BCBC5D0,
        0x7FC3F378, 0x48352EE1}},
      {"NuiAudioRelease",
       {0x7D8802A6, 0x4B527985, 0x9421FF70, 0x3D6083A4, 0x83C30000, 0x7C7D1B78,
        0x3B8BC4D8, 0x387C00F8}},
      {"NuiSkeletonTrackingEnable",
       {0x7D8802A6, 0x4B5681E5, 0x9421FF60, 0x3D6083C5, 0x3B200000, 0x3BEB3EB8,
        0x7C7D1B78, 0x7C9E2378}},
      {"NuiSkeletonTrackingDisable",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083A4,
        0x3BCBBC98, 0x7FC3F378}},
      {"NuiSkeletonSetTrackedSkeletons",
       {0x7D8802A6, 0x4B568839, 0x9421FF90, 0x7C7E1B78, 0x2B030000, 0x409A0014,
        0x3C608007, 0x60630057}},
      {"NuiSkeletonGetNextFrame",
       {0x7D8802A6, 0x4B56804D, 0x9421FF60, 0x7C9A2378, 0x2B040000, 0x409A0014,
        0x3C608007, 0x60630057}},
      {"NuiImageGetColorPixelCoordinatesFromDepthPixel",
       {0x7D8802A6, 0x4B5616A9, 0x9421FF70, 0x7C9E2378, 0x7CBC2B78, 0x7CDB3378,
        0x7CFA3B78, 0x7D1F4378}},
      {"NuiCameraSetProperty",
       {0x7D8802A6, 0x4B56290D, 0x9421FF90, 0x7C7F1B78, 0x7C9E2378, 0x7CBD2B78,
        0x2F030000, 0x409A0020}},
      {"NuiCameraElevationGetAngle",
       {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x2B030000, 0x409A001C, 0x3C608007,
        0x60630057, 0x38210060}},
      {"NuiCameraElevationSetAngle",
       {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0, 0x7C7F1B78, 0x38600025,
        0x4839098D, 0x2B030000}},
      {"NuiCameraAdjustTilt",
       {0x7D8802A6, 0x4B565EED, 0xDBA1FFC0, 0xDBC1FFC8, 0xDBE1FFD0, 0x9421FF70,
        0x7C7E1B78, 0xFFE00890}},
      {"NuiCameraGetNormalToGravity",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083C5,
        0x3D208204, 0x3D008200}},
      {"NuiCameraSetExposureRegionOfInterest",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x7C7F1B78,
        0x7C9E2378, 0x2F030000}},
      {"NuiCameraGetExposureRegionOfInterest",
       {0x7C852378, 0x38800000, 0x4BFFFA58, 0x7D8802A6, 0x9181FFF8, 0xDBE1FFF0,
        0x9421FF90, 0x7C6B07B4}},
      {"NuiCameraGetProperty",
       {0x7CA62B78, 0x7C852378, 0x38800000, 0x4BFFF85C, 0x7D8802A6, 0x9181FFF8,
        0xFBC1FFE8, 0xFBE1FFF0}},
      {"NuiCameraGetPropertyF",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083C5,
        0x7CBE2B78, 0x816B3EB4}},
      {"NuiIdentityEnroll",
       {0x7D8802A6, 0x4B56713D, 0x9421FF50, 0x54AB003C, 0x7C7C1B78, 0x7C9D2378,
        0x7CBE2B78, 0x7CDA3378}},
      {"NuiIdentityIdentify",
       {0x7D8802A6, 0x4B566F89, 0x9421FF60, 0x3D6083C5, 0x7C7E1B78, 0x396B3EB8,
        0x7C9D2378, 0x7CBC2B78}},
      {"NuiIdentityGetEnrollmentInformation",
       {0x7D8802A6, 0x4B566E69, 0x9421FF80, 0x7C7D1B78, 0x7C9F2378, 0x2B030008,
        0x41980010, 0x3C608007}},
      {"NuiIdentityAbort",
       {0x38600000, 0x4BFFFEE8, 0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x81630008,
        0x81430004, 0x7D6907B4}},

  // BEGIN AUTOGEN TEST CASES: speech_fitness_wave_head_nuip decomp
      {"NuiFitnessStartTracking",
       {0x7D8802A6, 0x4B559055, 0xDBA1FF90, 0xDBC1FF98, 0xDBE1FFA0, 0x9421FF20, 0x3D6083C5, 0x3BA00000}},
      {"NuiFitnessPauseTracking",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083C6, 0x7C7E1B78, 0x3BEB43B8}},
      {"NuiFitnessResumeTracking",
       {0x7D8802A6, 0x4B558CD9, 0x9421FF80, 0x3D6083C6, 0x7C7E1B78, 0x3BEB43B8, 0x7C9D2378, 0x387F0008}},
      {"NuiFitnessStopTracking",
       {0x7D8802A6, 0x4B558BF1, 0xDBC1FFA0, 0xDBE1FFA8, 0x9421FF30, 0x3D6083C6, 0x7C7E1B78, 0x3BEB43B8}},
      {"NuiFitnessGetCurrentFitnessData",
       {0x7D8802A6, 0x4B23106D, 0x9421FF80, 0x3D6083C6, 0x7C7F1B78, 0x3BAB43B8, 0x7C9E2378, 0x387D0008}},
      {"NuiWaveSetEnabled",
       {0x7D8802A6, 0x4B559471, 0x9421FF80, 0x3D6083C5, 0x7C7C1B78, 0x396B3EB8, 0x814B0874, 0x2F0A0000}},
      {"NuiWaveGetGestureOwnerProgress",
       {0x7D8802A6, 0x4B559565, 0x9421FF80, 0x7C7E1B78, 0x7C9F2378, 0x2B030000, 0x409A0010, 0x3C608007}},
      {"NuiHeadOrientationDisable",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083A4, 0x3BCBC224, 0x7FC3F378}},
      {"NuiHeadPositionDisable",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083A3, 0x3BCB8A48, 0x7FC3F378}},
      {"NuiSpeechEnable",
       {0x7C862378, 0x2B030000, 0x409A0010, 0x3C608007, 0x60630057, 0x4E800020, 0x3D6083C0, 0x80A30004}},
      {"NuiSpeechDisable",
       {0x3D6083C0, 0x386B9108, 0x4BFFEEF4, 0x7C6B1B78, 0x7CC73378, 0x3D4083C0, 0x7CA62B78, 0x7C852378}},
      {"NuiSpeechCreateGrammar",
       {0x7C6B1B78, 0x3D4083C0, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFF058, 0x7D8802A6, 0x9181FFF8}},
      {"NuiSpeechLoadGrammar",
       {0x7C6B1B78, 0x7CC73378, 0x3D4083C0, 0x7CA62B78, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFF190}},
      {"NuiSpeechUnloadGrammar",
       {0x3D6083C0, 0x7C641B78, 0x386B9108, 0x4BFFF2E0, 0x7C6B1B78, 0x3D4083C0, 0x7C852378, 0x386A9108}},
      {"NuiSpeechCommitGrammar",
       {0x3D6083C0, 0x7C641B78, 0x386B9108, 0x4BFFF768, 0x7D8802A6, 0x4B86DA41, 0x9421FF70, 0x3BC00000}},
      {"NuiSpeechStartRecognition",
       {0x3D6083C0, 0x38800000, 0x386B9108, 0x4BFFF728, 0x7C6B1B78, 0x3D4083C0, 0x7C852378, 0x386A9108}},
      {"NuiSpeechStopRecognition",
       {0x3D6083C0, 0x38800000, 0x386B9108, 0x4BFFE884, 0x3D6083C0, 0x7C641B78, 0x386B9108, 0x4BFFF3C4}},
      {"NuiSpeechSetEventInterest",
       {0x3D6083C0, 0x7C641B78, 0x386B9108, 0x4BFFFB54, 0x7C6B1B78, 0x3D4083C0, 0x7CA62B78, 0x7C852378}},
      {"NuiSpeechSetGrammarState",
       {0x7C6B1B78, 0x3D4083C0, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFFAD4, 0x3D6083C0, 0x7C641B78}},
      {"NuiSpeechSetRuleState",
       {0x7C6B1B78, 0x3D4083C0, 0x7CA62B78, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFF420, 0x7CE83B78}},
      {"NuiSpeechCreateRule",
       {0x7CE83B78, 0x7C6B1B78, 0x7CC73378, 0x3D4083C0, 0x7CA62B78, 0x7C852378, 0x386A9108, 0x7D645B78}},
      {"NuiSpeechCreateState",
       {0x7C6B1B78, 0x3D4083C0, 0x7CA62B78, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFF544, 0x7D8802A6}},
      {"NuiSpeechAddWordTransition",
       {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x7D094378, 0x91410054, 0x7CE83B78, 0x7C6B1B78, 0x7CC73378}},
      {"NuiSpeechGetEvents",
       {0x7C6B1B78, 0x3D4083C0, 0x7CA62B78, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFFC5C, 0x7D8802A6}},
      {"NuiSpeechDestroyEvent",
       {0x3D6083C0, 0x7C641B78, 0x386B9108, 0x4BFFF3C4, 0x7C6B1B78, 0x3D4083C0, 0x7CA62B78, 0x7C852378}},
      {"NuiSpeechEmulateRecognition",
       {0x7C6B1B78, 0x3D4083C0, 0x7C852378, 0x386A9108, 0x7D645B78, 0x4BFFF510, 0x7D8802A6, 0x4B86BA05}},
      {"D3DDevice_NuiMetaData",
       {0x7D8802A6, 0x4B3A484D, 0x9421FF70, 0x7C7F1B78, 0x7C9A2378, 0x7CBB2B78, 0x7CDD3378, 0x7DAD6B78}},
      {"NuipBuildXamNuiFrameData",
       {0x7D8802A6, 0x4B54F4E9, 0x9421FF80, 0x7C7E1B78, 0x7C9D2378, 0x7CBC2B78, 0x38A00AE0, 0x38800000}},
      {"NuipCameraGetExposureRegionOfInterest",
       {0x2F040002, 0x41980010, 0x3C608007, 0x60630057, 0x4E800020, 0x2F030000, 0x409AFFF0, 0x3D6083C5}},
      {"NuipCameraGetProperty",
       {0x7D8802A6, 0x4B563F89, 0x9421FF80, 0x7C9F2378, 0x7CDE3378, 0x3BA00000, 0x2F040002, 0x41980014}},
      {"NuipCameraGetPropertyF",
       {0x7D8802A6, 0x4B563C81, 0x9421FF80, 0x7C6A1B78, 0x7C832378, 0x7CDD3378, 0x2F040002, 0x40980248}},
      {"NuipCreateInstance",
       {0x7D8802A6, 0x4B4F7D19, 0x9421FF80, 0x39400000, 0x7C6B1B78, 0x91410050, 0x3D20822B, 0x3D4083C9}},
      {"NuipFitnessInitialize",
       {0x7D8802A6, 0x4B5591E5, 0x9421FF80, 0x3D6083C6, 0x3BEB43B8, 0x387F0008, 0x48383455, 0x39600000}},
      {"NuipFitnessNewSkeletalFrame",
       {0x7D8802A6, 0x4B558935, 0x9421FF80, 0x3D6083C6, 0x7C7E1B78, 0x3BAB43B8, 0x387D0008, 0x48382BA5}},
      {"NuipFitnessShutdown",
       {0x7D8802A6, 0x4B559101, 0x9421FF80, 0x3D6083C6, 0x3BEB43B8, 0x387F0008, 0x48383379, 0x3B600000}},
      {"NuipInitialize",
       {0x7D8802A6, 0x4B55A375, 0x9421FCC0, 0x3D8033FF, 0x90A10364, 0x3B600000, 0x618C0314, 0x7C771B78}},
      {"NuipLoadRegistry",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x3D6083A4, 0x396BCE80, 0x814B000C}},
      {"NuipModuleInit",
       {0x7D8802A6, 0x4B4F7C75, 0x9421FF40, 0x3BE00000, 0x7C7D1B78, 0x7C9E2378, 0x7CB72B78, 0x2B030000}},
      {"NuipModuleTerm",
       {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x48005F91, 0x3D6083C9, 0x386BCAE8}},
      {"NuipRegCreateKeyExW",
       {0x7D8802A6, 0x4B4F0E6D, 0x9421FB50, 0x7CFB3B78, 0x7D475378, 0x7D064378, 0x7F65DB78, 0x7C7F1B78}},
      {"NuipRegEnumKeyExW",
       {0x7D8802A6, 0x4B4F21A5, 0x9421FF80, 0x7C681B78, 0x7CBE2B78, 0x7CDD3378, 0x3B800000, 0x2B050000}},
      {"NuipRegEnumValueW",
       {0x7D8802A6, 0x4B4F209D, 0x9421FF60, 0x3D6083A4, 0x7C7C1B78, 0x396BCE80, 0x7CBE2B78, 0x7CDD3378}},
      {"NuipRegOpenKeyExW",
       {0x7D8802A6, 0x4B4F10E9, 0x9421FD70, 0x3D6083A4, 0x3940FFFF, 0x396BCE80, 0x3BC00000, 0x91470000}},
      {"NuipRegQueryValueExW",
       {0x7D8802A6, 0x4B4F1A71, 0x9421FF70, 0x7CDF3378, 0x7CFD3B78, 0x7D1E4378, 0x2B080000, 0x409A0014}},
      {"NuipRegSetValueExW",
       {0x7D8802A6, 0x4B4F1499, 0x9421F700, 0x3D6083A4, 0x7C7C1B78, 0x396BCE80, 0x7C9F2378, 0x7CD73378}},
      {"NuipUnloadRegistry",
       {0x7D8802A6, 0x4B4F1B5D, 0x9421FF70, 0x3D6083A4, 0x39400003, 0x396BCE80, 0x3B200000, 0x396BFFFC}},
      {"NuipWaveInit",
       {0x7D8802A6, 0x4B5596D5, 0x9421FF80, 0x7C7C1B78, 0x38600000, 0x3BA00000, 0x3BC00000, 0x2F030000}},
      {"NuipWaveUpdate",
       {0x7D8802A6, 0x4B559659, 0xDBE1FFA8, 0x9421FF50, 0x7C7C1B78, 0x2B030000, 0x409A0010, 0x3C608007}},
  // END AUTOGEN TEST CASES: speech_fitness_wave_head_nuip decomp
  }};

  auto run_cases = [](const auto& cases) {
    GuestMemoryHarness harness(0x20000);
    auto text = MakeTextInfo(harness.base, harness.size);
    for (size_t i = 0; i < cases.size(); ++i) {
      const uint32_t addr = harness.base + static_cast<uint32_t>(0x100 + i * 0x200);
      WriteWords(harness, addr, cases[i].words);
      Dc3NuiPatchSpec spec{addr, 0, 0, cases[i].name};
      auto result = Dc3ResolveNuiPatchTarget(spec, text, nullptr, nullptr, "strict",
                                             &harness.memory, true);
      INFO(cases[i].name);
      REQUIRE(result.resolved);
      REQUIRE(result.resolve_method == Dc3PatchResolveMethod::kSignatureStub);
      REQUIRE(result.resolved_address == addr);
    }
  };

  SECTION("original") { run_cases(original_cases); }
  SECTION("decomp") { run_cases(decomp_cases); }
}

TEST_CASE("DC3 resolver strict signature uses local hint to adjust manifest target",
          "[dc3_nui_patch_resolver]") {
  GuestMemoryHarness harness;
  const uint32_t patch_site = harness.base + 0x180;
  const uint32_t manifest_target = patch_site + 0x24;
  WriteWords(harness, patch_site,
             {0x7D8802A6, 0x4B605FAD, 0x9421FB60, 0x7C7F1B78, 0x7C9C2378,
              0x7CBB2B78, 0x39600400, 0x3BA00000});
  Dc3NuiPatchManifest manifest;
  manifest.targets["CXbcImpl::SendJSON"] = manifest_target;
  Dc3NuiPatchSpec spec{patch_site, 0, 0, "CXbcImpl::SendJSON"};
  auto result = Dc3ResolveNuiPatchTarget(spec, MakeTextInfo(harness.base, harness.size),
                                         &manifest, nullptr, "hybrid",
                                         &harness.memory, true);
  REQUIRE(result.resolved);
  REQUIRE(result.resolved_address == patch_site);
  REQUIRE(result.resolve_method == Dc3PatchResolveMethod::kSignatureStub);
}

TEST_CASE("DC3 resolver strict signature fails closed on local hint miss",
          "[dc3_nui_patch_resolver]") {
  GuestMemoryHarness harness;
  const uint32_t hint = harness.base + 0x200;
  // Non-matching bytes in the local hint window.
  WriteWords(harness, hint, {0x60000000, 0x60000000, 0x60000000, 0x4E800020});
  Dc3NuiPatchSpec spec{hint, 0, 0, "CXbcImpl::Initialize"};
  auto result = Dc3ResolveNuiPatchTarget(spec, MakeTextInfo(harness.base, harness.size),
                                         nullptr, nullptr, "strict",
                                         &harness.memory, true);
  REQUIRE_FALSE(result.resolved);
  REQUIRE(result.strict_rejected);
}

TEST_CASE("DC3 resolver strict signature fails closed on local miss with global clone",
          "[dc3_nui_patch_resolver]") {
  GuestMemoryHarness harness(0x4000);
  const uint32_t hint = harness.base + 0x200;
  const uint32_t clone = harness.base + 0xA00;
  // Non-matching bytes in the local hint window near the catalog/spec hint.
  WriteWords(harness, hint, {0x60000000, 0x60000000, 0x60000000, 0x4E800020});
  // Valid signature exists elsewhere in .text, but strict mode should refuse the
  // global scan result once the local hint window misses.
  WriteWords(harness, clone,
             {0x7D8802A6, 0x4B5599F9, 0x9421FF50, 0x546B05AC, 0x7C7F1B78,
              0x7C972378});
  Dc3NuiPatchSpec spec{hint, 0, 0, "NuiInitialize"};
  auto result = Dc3ResolveNuiPatchTarget(spec, MakeTextInfo(harness.base, harness.size),
                                         nullptr, nullptr, "strict",
                                         &harness.memory, true);
  REQUIRE_FALSE(result.resolved);
  REQUIRE(result.strict_rejected);
}

TEST_CASE("DC3 resolver strict signature rejects ambiguous global matches",
          "[dc3_nui_patch_resolver]") {
  GuestMemoryHarness harness(0x4000);
  const uint32_t match_a = harness.base + 0x400;
  const uint32_t match_b = harness.base + 0xC00;
  WriteWords(harness, match_a,
             {0x7D8802A6, 0x4B5599F9, 0x9421FF50, 0x546B05AC, 0x7C7F1B78,
              0x7C972378});
  WriteWords(harness, match_b,
             {0x7D8802A6, 0x4BFCC709, 0x9421FF50, 0x546B05AC, 0x7C7F1B78,
              0x7C972378});
  // No catalog/spec hint address -> resolver performs global scan and must reject
  // the ambiguous result set.
  Dc3NuiPatchSpec spec{0, 0, 0, "NuiInitialize"};
  auto result = Dc3ResolveNuiPatchTarget(spec, MakeTextInfo(harness.base, harness.size),
                                         nullptr, nullptr, "strict",
                                         &harness.memory, true);
  REQUIRE_FALSE(result.resolved);
  REQUIRE(result.strict_rejected);
}

TEST_CASE("DC3 resolver strict signature rejects ambiguous speech-family matches",
          "[dc3_nui_patch_resolver]") {
  GuestMemoryHarness harness(0x4000);
  const uint32_t match_a = harness.base + 0x500;
  const uint32_t match_b = harness.base + 0xD00;
  // Two valid NuiSpeechEnable signatures (orig + decomp variant) should be
  // treated as ambiguous without a local hint address.
  WriteWords(harness, match_a,
             {0x7C862378, 0x2B030000, 0x409A0010, 0x3C608007, 0x60630057,
              0x4E800020, 0x3D608316, 0x80A30004});
  WriteWords(harness, match_b,
             {0x7C862378, 0x2B030000, 0x409A0010, 0x3C608007, 0x60630057,
              0x4E800020, 0x3D6083C0, 0x80A30004});
  Dc3NuiPatchSpec spec{0, 0, 0, "NuiSpeechEnable"};
  auto result = Dc3ResolveNuiPatchTarget(spec, MakeTextInfo(harness.base, harness.size),
                                         nullptr, nullptr, "strict",
                                         &harness.memory, true);
  REQUIRE_FALSE(result.resolved);
  REQUIRE(result.strict_rejected);
}

}  // namespace
}  // namespace xe::dc3::test
