/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 */

#include "xenia/dc3_nui_patch_resolver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "xenia/base/logging.h"
#include "xenia/base/filesystem.h"
#include "xenia/memory.h"

namespace xe {
namespace dc3 {
namespace {

struct Dc3SignatureWord {
  uint32_t value;
  uint32_t mask;
};

struct Dc3SignatureVariant {
  const Dc3SignatureWord* words;
  size_t word_count;
};

struct Dc3TargetSignatureSet {
  const char* name;
  const Dc3SignatureVariant* variants;
  size_t variant_count;
};

constexpr uint32_t kFullMask = 0xFFFFFFFFu;
constexpr uint32_t kBranchMask = 0xFC000003u;

constexpr Dc3SignatureWord kNuiInitializeOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCC709, kBranchMask},
    {0x9421FF50, kFullMask},
    {0x546B05AC, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C972378, kFullMask},
};
constexpr Dc3SignatureWord kNuiInitializeDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B5599F9, kBranchMask},
    {0x9421FF50, kFullMask},
    {0x546B05AC, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C972378, kFullMask},
};

constexpr Dc3SignatureWord kNuiShutdownOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCEB79, kBranchMask},
    {0x9421FF50, kFullMask},
    {0x3D608312, kFullMask},
    {0x3B600000, kFullMask},
    {0x3BEBC8A0, kFullMask},
};
constexpr Dc3SignatureWord kNuiShutdownDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B55BE45, kBranchMask},
    {0x9421FF50, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x3B600000, kFullMask},
    {0x3BEB3EB8, kFullMask},
};

constexpr Dc3SignatureWord kXbcInitOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x483978A9, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6082F1, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x3B6B2BEC, kFullMask},
    {0x7C9C2378, kFullMask},
    {0x7F63DB78, kFullMask},
};
constexpr Dc3SignatureWord kXbcInitDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B605D41, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083A3, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x3B6B7E28, kFullMask},
    {0x7C9C2378, kFullMask},
    {0x7F63DB78, kFullMask},
};

constexpr Dc3SignatureWord kXbcDoWorkOrigWords[] = {
    {0x48004EA8, kBranchMask},
    {0x00000000, kFullMask},
    {0x3D40830A, kFullMask},
    {0x39600000, kFullMask},
    {0x390AB060, kFullMask},
    {0x39200000, kFullMask},
};
constexpr Dc3SignatureWord kXbcDoWorkDecompWords[] = {
    {0x48003DC8, kBranchMask},
    {0x3D4083C2, kFullMask},
    {0x39600000, kFullMask},
    {0x390A8C10, kFullMask},
    {0x39200000, kFullMask},
    {0x7D0A4378, kFullMask},
};

// SendJSON needs a longer signature due common prologues / repeated local loops.
constexpr Dc3SignatureWord kXbcSendJsonOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x48397B21, kBranchMask},
    {0x9421FB60, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9C2378, kFullMask},
    {0x7CBB2B78, kFullMask},
    {0x39600400, kFullMask},
    {0x3BA00000, kFullMask},
};
constexpr Dc3SignatureWord kXbcSendJsonDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B605FAD, kBranchMask},
    {0x9421FB60, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9C2378, kFullMask},
    {0x7CBB2B78, kFullMask},
    {0x39600400, kFullMask},
    {0x3BA00000, kFullMask},
};

constexpr Dc3SignatureWord kNuiMetaCpuEventOrigWords[] = {
    {0x7DEF7B78, kFullMask},
    {0x3C607DEF, kFullMask},
    {0x60637B78, kFullMask},
    {0x4E800020, kFullMask},
    {0x7D8802A6, kFullMask},
    {0x4BE463A9, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3FE08316, kFullMask},
};
constexpr Dc3SignatureWord kNuiMetaCpuEventDecompWords[] = {
    {0x7DEF7B78, kFullMask},
    {0x3C607DEF, kFullMask},
    {0x60637B78, kFullMask},
    {0x4E800020, kFullMask},
    {0x7D8802A6, kFullMask},
    {0x4B3F3351, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3FE083C9, kFullMask},
};
constexpr Dc3SignatureWord kNuiMetaCpuEventDecompCatalogWords[] = {
    {0x38210090, kFullMask},
    {0x4B3F3380, kBranchMask},
    {0x3D6083C9, kFullMask},
    {0x7C641B78, kFullMask},
    {0x806BD3E8, kFullMask},
    {0x480073DC, kBranchMask},
    {0x3D6083C9, kFullMask},
    {0x806BD3E8, kFullMask},
    {0x4800740C, kBranchMask},
    {0x4B8EBFEC, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x4B3F3311, kBranchMask},
};

constexpr Dc3SignatureWord kD3DNuiInitializeDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7DAD6B78, kFullMask},
    {0x3D60822E, kFullMask},
    {0x816B8808, kFullMask},
};
constexpr Dc3SignatureWord kD3DNuiInitializeDecompCatalogWords[] = {
    {0xEBC1FFE8, kFullMask},
    {0xEBE1FFF0, kFullMask},
    {0x4E800020, kFullMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7C7F1B78, kFullMask},
};
constexpr Dc3SignatureWord kD3DNuiStartDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x7DAD6B78, kFullMask},
};
constexpr Dc3SignatureWord kD3DNuiStopDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7DAD6B78, kFullMask},
    {0x39600000, kFullMask},
    {0x917F3670, kFullMask},
};
constexpr Dc3SignatureWord kD3DNuiStopDecompCatalogWords[] = {
    {0x57E9043E, kFullMask},
    {0x7D6A5A2E, kFullMask},
    {0x7D6B4838, kFullMask},
    {0x556A043E, kFullMask},
    {0x5569ECFE, kFullMask},
    {0x556BA73E, kFullMask},
    {0x7D295378, kFullMask},
    {0x5529E8FE, kFullMask},
    {0x7D295378, kFullMask},
    {0x5529E8FE, kFullMask},
    {0x7D2A5378, kFullMask},
    {0x554A073C, kFullMask},
};

constexpr Dc3SignatureWord kNuiImageStreamOpenOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFD45D9, kBranchMask},
    {0x9421FF40, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9C2378, kFullMask},
    {0x7CBA2B78, kFullMask},
    {0x7CDB3378, kFullMask},
    {0x7CF73B78, kFullMask},
};
constexpr Dc3SignatureWord kNuiImageStreamOpenDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B561589, kBranchMask},
    {0x9421FF40, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9C2378, kFullMask},
    {0x7CBA2B78, kFullMask},
    {0x7CDB3378, kFullMask},
    {0x7CF73B78, kFullMask},
};

constexpr Dc3SignatureWord kNuiImageStreamGetNextFrameOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFD5229, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x7CBC2B78, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
};
constexpr Dc3SignatureWord kNuiImageStreamGetNextFrameDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B5621C1, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x7CBC2B78, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
};

constexpr Dc3SignatureWord kNuiImageStreamReleaseFrameOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFD4F11, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
};
constexpr Dc3SignatureWord kNuiImageStreamReleaseFrameDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B561EA9, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
};

constexpr Dc3SignatureWord kNuiAudioCreateOrigWords[] = {
    {0x39200000, kFullMask},
    {0x39000000, kFullMask},
    {0x4BFFFA18, kBranchMask},
    {0x00000000, kFullMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FFA0, kFullMask},
};
constexpr Dc3SignatureWord kNuiAudioCreateDecompWords[] = {
    {0x39200000, kFullMask},
    {0x39000000, kFullMask},
    {0x4BFFFA18, kBranchMask},
    {0x4E800020, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x386BC608, kFullMask},
    {0x4BFFF300, kBranchMask},
    {0x3D6083A4, kFullMask},
};

constexpr Dc3SignatureWord kNuiAudioCreatePrivateOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BF8FEB1, kBranchMask},
    {0x9421FF20, kFullMask},
    {0x90860000, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x7C751B78, kFullMask},
    {0x3B8BFEB0, kFullMask},
    {0x7C9E2378, kFullMask},
};
constexpr Dc3SignatureWord kNuiAudioCreatePrivateDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B52735D, kBranchMask},
    {0x9421FF20, kFullMask},
    {0x90860000, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C751B78, kFullMask},
    {0x3B8BC4E0, kFullMask},
    {0x7C9E2378, kFullMask},
};

constexpr Dc3SignatureWord kNuiAudioRegisterCallbacksOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BF90001, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x3BEBFFBC, kFullMask},
    {0x7CBD2B78, kFullMask},
    {0x387FFFE4, kFullMask},
};
constexpr Dc3SignatureWord kNuiAudioRegisterCallbacksDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B5274A9, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x3BEBC5EC, kFullMask},
    {0x7CBD2B78, kFullMask},
    {0x387FFFE4, kFullMask},
};

constexpr Dc3SignatureWord kNuiAudioUnregisterCallbacksOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x3BEBFFBC, kFullMask},
};
constexpr Dc3SignatureWord kNuiAudioUnregisterCallbacksDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x3BEBC5EC, kFullMask},
};

constexpr Dc3SignatureWord kNuiAudioRegisterCallbacksPrivateOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BF91889, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x3BCBFFA0, kFullMask},
    {0x7CBD2B78, kFullMask},
    {0x7FC3F378, kFullMask},
};
constexpr Dc3SignatureWord kNuiAudioRegisterCallbacksPrivateDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B528D0D, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x3BCBC5D0, kFullMask},
    {0x7CBD2B78, kFullMask},
    {0x7FC3F378, kFullMask},
};

constexpr Dc3SignatureWord kNuiAudioUnregisterCallbacksPrivateOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BF91821, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x3BCBFFA0, kFullMask},
    {0x7FC3F378, kFullMask},
    {0x484D9761, kBranchMask},
};
constexpr Dc3SignatureWord kNuiAudioUnregisterCallbacksPrivateDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B528CA5, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x3BCBC5D0, kFullMask},
    {0x7FC3F378, kFullMask},
    {0x48352EE1, kBranchMask},
};

constexpr Dc3SignatureWord kNuiAudioReleaseOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BF904E1, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x83C30000, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x3B8BFEA8, kFullMask},
    {0x387C00F8, kFullMask},
};
constexpr Dc3SignatureWord kNuiAudioReleaseDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B527985, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x83C30000, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x3B8BC4D8, kFullMask},
    {0x387C00F8, kFullMask},
};

constexpr Dc3SignatureVariant kNuiInitializeVariants[] = {
    {kNuiInitializeOrigWords, std::size(kNuiInitializeOrigWords)},
    {kNuiInitializeDecompWords, std::size(kNuiInitializeDecompWords)},
};
constexpr Dc3SignatureVariant kNuiShutdownVariants[] = {
    {kNuiShutdownOrigWords, std::size(kNuiShutdownOrigWords)},
    {kNuiShutdownDecompWords, std::size(kNuiShutdownDecompWords)},
};
constexpr Dc3SignatureVariant kXbcInitVariants[] = {
    {kXbcInitOrigWords, std::size(kXbcInitOrigWords)},
    {kXbcInitDecompWords, std::size(kXbcInitDecompWords)},
};
constexpr Dc3SignatureVariant kXbcDoWorkVariants[] = {
    {kXbcDoWorkOrigWords, std::size(kXbcDoWorkOrigWords)},
    {kXbcDoWorkDecompWords, std::size(kXbcDoWorkDecompWords)},
};
constexpr Dc3SignatureVariant kXbcSendJsonVariants[] = {
    {kXbcSendJsonOrigWords, std::size(kXbcSendJsonOrigWords)},
    {kXbcSendJsonDecompWords, std::size(kXbcSendJsonDecompWords)},
};
constexpr Dc3SignatureVariant kNuiMetaCpuEventVariants[] = {
    {kNuiMetaCpuEventOrigWords, std::size(kNuiMetaCpuEventOrigWords)},
    {kNuiMetaCpuEventDecompWords, std::size(kNuiMetaCpuEventDecompWords)},
    {kNuiMetaCpuEventDecompCatalogWords,
     std::size(kNuiMetaCpuEventDecompCatalogWords)},
};
constexpr Dc3SignatureVariant kD3DNuiInitializeVariants[] = {
    {kD3DNuiInitializeDecompWords, std::size(kD3DNuiInitializeDecompWords)},
    {kD3DNuiInitializeDecompCatalogWords,
     std::size(kD3DNuiInitializeDecompCatalogWords)},
};
constexpr Dc3SignatureVariant kD3DNuiStartVariants[] = {
    {kD3DNuiStartDecompWords, std::size(kD3DNuiStartDecompWords)},
};
constexpr Dc3SignatureVariant kD3DNuiStopVariants[] = {
    {kD3DNuiStopDecompWords, std::size(kD3DNuiStopDecompWords)},
    {kD3DNuiStopDecompCatalogWords, std::size(kD3DNuiStopDecompCatalogWords)},
};
constexpr Dc3SignatureVariant kNuiImageStreamOpenVariants[] = {
    {kNuiImageStreamOpenOrigWords, std::size(kNuiImageStreamOpenOrigWords)},
    {kNuiImageStreamOpenDecompWords, std::size(kNuiImageStreamOpenDecompWords)},
};
constexpr Dc3SignatureVariant kNuiImageStreamGetNextFrameVariants[] = {
    {kNuiImageStreamGetNextFrameOrigWords,
     std::size(kNuiImageStreamGetNextFrameOrigWords)},
    {kNuiImageStreamGetNextFrameDecompWords,
     std::size(kNuiImageStreamGetNextFrameDecompWords)},
};
constexpr Dc3SignatureVariant kNuiImageStreamReleaseFrameVariants[] = {
    {kNuiImageStreamReleaseFrameOrigWords,
     std::size(kNuiImageStreamReleaseFrameOrigWords)},
    {kNuiImageStreamReleaseFrameDecompWords,
     std::size(kNuiImageStreamReleaseFrameDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioCreateVariants[] = {
    {kNuiAudioCreateOrigWords, std::size(kNuiAudioCreateOrigWords)},
    {kNuiAudioCreateDecompWords, std::size(kNuiAudioCreateDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioCreatePrivateVariants[] = {
    {kNuiAudioCreatePrivateOrigWords, std::size(kNuiAudioCreatePrivateOrigWords)},
    {kNuiAudioCreatePrivateDecompWords,
     std::size(kNuiAudioCreatePrivateDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioRegisterCallbacksVariants[] = {
    {kNuiAudioRegisterCallbacksOrigWords,
     std::size(kNuiAudioRegisterCallbacksOrigWords)},
    {kNuiAudioRegisterCallbacksDecompWords,
     std::size(kNuiAudioRegisterCallbacksDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioUnregisterCallbacksVariants[] = {
    {kNuiAudioUnregisterCallbacksOrigWords,
     std::size(kNuiAudioUnregisterCallbacksOrigWords)},
    {kNuiAudioUnregisterCallbacksDecompWords,
     std::size(kNuiAudioUnregisterCallbacksDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioRegisterCallbacksPrivateVariants[] = {
    {kNuiAudioRegisterCallbacksPrivateOrigWords,
     std::size(kNuiAudioRegisterCallbacksPrivateOrigWords)},
    {kNuiAudioRegisterCallbacksPrivateDecompWords,
     std::size(kNuiAudioRegisterCallbacksPrivateDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioUnregisterCallbacksPrivateVariants[] = {
    {kNuiAudioUnregisterCallbacksPrivateOrigWords,
     std::size(kNuiAudioUnregisterCallbacksPrivateOrigWords)},
    {kNuiAudioUnregisterCallbacksPrivateDecompWords,
     std::size(kNuiAudioUnregisterCallbacksPrivateDecompWords)},
};
constexpr Dc3SignatureVariant kNuiAudioReleaseVariants[] = {
    {kNuiAudioReleaseOrigWords, std::size(kNuiAudioReleaseOrigWords)},
    {kNuiAudioReleaseDecompWords, std::size(kNuiAudioReleaseDecompWords)},
};
constexpr Dc3SignatureWord kNuiSkeletonTrackingEnableOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFDB325, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x3D608312, kFullMask},
    {0x3B200000, kFullMask},
    {0x3BEBC8A0, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9E2378, kFullMask},
};
constexpr Dc3SignatureWord kNuiSkeletonTrackingEnableDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B5681E5, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x3B200000, kFullMask},
    {0x3BEB3EB8, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9E2378, kFullMask},
};
constexpr Dc3SignatureVariant kNuiSkeletonTrackingEnableVariants[] = {
    {kNuiSkeletonTrackingEnableOrigWords,
     std::size(kNuiSkeletonTrackingEnableOrigWords)},
    {kNuiSkeletonTrackingEnableDecompWords,
     std::size(kNuiSkeletonTrackingEnableDecompWords)},
};

constexpr Dc3SignatureWord kNuiSkeletonTrackingDisableOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x3BCBF650, kFullMask},
    {0x7FC3F378, kFullMask},
};
constexpr Dc3SignatureWord kNuiSkeletonTrackingDisableDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x3BCBBC98, kFullMask},
    {0x7FC3F378, kFullMask},
};
constexpr Dc3SignatureVariant kNuiSkeletonTrackingDisableVariants[] = {
    {kNuiSkeletonTrackingDisableOrigWords,
     std::size(kNuiSkeletonTrackingDisableOrigWords)},
    {kNuiSkeletonTrackingDisableDecompWords,
     std::size(kNuiSkeletonTrackingDisableDecompWords)},
};

constexpr Dc3SignatureWord kNuiSkeletonSetTrackedSkeletonsOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFDB999, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
};
constexpr Dc3SignatureWord kNuiSkeletonSetTrackedSkeletonsDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B568839, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
};
constexpr Dc3SignatureVariant kNuiSkeletonSetTrackedSkeletonsVariants[] = {
    {kNuiSkeletonSetTrackedSkeletonsOrigWords,
     std::size(kNuiSkeletonSetTrackedSkeletonsOrigWords)},
    {kNuiSkeletonSetTrackedSkeletonsDecompWords,
     std::size(kNuiSkeletonSetTrackedSkeletonsDecompWords)},
};

constexpr Dc3SignatureWord kNuiSkeletonGetNextFrameOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFDB18D, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x7C9A2378, kFullMask},
    {0x2B040000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
};
constexpr Dc3SignatureWord kNuiSkeletonGetNextFrameDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B56804D, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x7C9A2378, kFullMask},
    {0x2B040000, kFullMask},
    {0x409A0014, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
};
constexpr Dc3SignatureVariant kNuiSkeletonGetNextFrameVariants[] = {
    {kNuiSkeletonGetNextFrameOrigWords, std::size(kNuiSkeletonGetNextFrameOrigWords)},
    {kNuiSkeletonGetNextFrameDecompWords,
     std::size(kNuiSkeletonGetNextFrameDecompWords)},
};

constexpr Dc3SignatureWord
    kNuiImageGetColorPixelCoordinatesFromDepthPixelOrigWords[] = {
        {0x7D8802A6, kFullMask},
        {0x4BFD4751, kBranchMask},
        {0x9421FF70, kFullMask},
        {0x7C9E2378, kFullMask},
        {0x7CBC2B78, kFullMask},
        {0x7CDB3378, kFullMask},
        {0x7CFA3B78, kFullMask},
        {0x7D1F4378, kFullMask},
};
constexpr Dc3SignatureWord
    kNuiImageGetColorPixelCoordinatesFromDepthPixelDecompWords[] = {
        {0x7D8802A6, kFullMask},
        {0x4B5616A9, kBranchMask},
        {0x9421FF70, kFullMask},
        {0x7C9E2378, kFullMask},
        {0x7CBC2B78, kFullMask},
        {0x7CDB3378, kFullMask},
        {0x7CFA3B78, kFullMask},
        {0x7D1F4378, kFullMask},
};
constexpr Dc3SignatureVariant
    kNuiImageGetColorPixelCoordinatesFromDepthPixelVariants[] = {
        {kNuiImageGetColorPixelCoordinatesFromDepthPixelOrigWords,
         std::size(kNuiImageGetColorPixelCoordinatesFromDepthPixelOrigWords)},
        {kNuiImageGetColorPixelCoordinatesFromDepthPixelDecompWords,
         std::size(kNuiImageGetColorPixelCoordinatesFromDepthPixelDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraSetPropertyOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFD59E1, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x7CBD2B78, kFullMask},
    {0x2F030000, kFullMask},
    {0x409A0020, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraSetPropertyDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B56290D, kBranchMask},
    {0x9421FF90, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x7CBD2B78, kFullMask},
    {0x2F030000, kFullMask},
    {0x409A0020, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraSetPropertyVariants[] = {
    {kNuiCameraSetPropertyOrigWords, std::size(kNuiCameraSetPropertyOrigWords)},
    {kNuiCameraSetPropertyDecompWords,
     std::size(kNuiCameraSetPropertyDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraElevationGetAngleOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A001C, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
    {0x38210060, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraElevationGetAngleDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A001C, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
    {0x38210060, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraElevationGetAngleVariants[] = {
    {kNuiCameraElevationGetAngleOrigWords,
     std::size(kNuiCameraElevationGetAngleOrigWords)},
    {kNuiCameraElevationGetAngleDecompWords,
     std::size(kNuiCameraElevationGetAngleDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraElevationSetAngleOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x38600025, kFullMask},
    {0x4852175D, kBranchMask},
    {0x2B030000, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraElevationSetAngleDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x38600025, kFullMask},
    {0x4839098D, kBranchMask},
    {0x2B030000, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraElevationSetAngleVariants[] = {
    {kNuiCameraElevationSetAngleOrigWords,
     std::size(kNuiCameraElevationSetAngleOrigWords)},
    {kNuiCameraElevationSetAngleDecompWords,
     std::size(kNuiCameraElevationSetAngleDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraAdjustTiltOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFD8FE5, kBranchMask},
    {0xDBA1FFC0, kFullMask},
    {0xDBC1FFC8, kFullMask},
    {0xDBE1FFD0, kFullMask},
    {0x9421FF70, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0xFFE00890, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraAdjustTiltDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B565EED, kBranchMask},
    {0xDBA1FFC0, kFullMask},
    {0xDBC1FFC8, kFullMask},
    {0xDBE1FFD0, kFullMask},
    {0x9421FF70, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0xFFE00890, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraAdjustTiltVariants[] = {
    {kNuiCameraAdjustTiltOrigWords, std::size(kNuiCameraAdjustTiltOrigWords)},
    {kNuiCameraAdjustTiltDecompWords,
     std::size(kNuiCameraAdjustTiltDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraGetNormalToGravityOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D608312, kFullMask},
    {0x3D20821C, kFullMask},
    {0x3D008200, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraGetNormalToGravityDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x3D208204, kFullMask},
    {0x3D008200, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraGetNormalToGravityVariants[] = {
    {kNuiCameraGetNormalToGravityOrigWords,
     std::size(kNuiCameraGetNormalToGravityOrigWords)},
    {kNuiCameraGetNormalToGravityDecompWords,
     std::size(kNuiCameraGetNormalToGravityDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraSetExposureRegionOfInterestOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x2F030000, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraSetExposureRegionOfInterestDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x2F030000, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraSetExposureRegionOfInterestVariants[] = {
    {kNuiCameraSetExposureRegionOfInterestOrigWords,
     std::size(kNuiCameraSetExposureRegionOfInterestOrigWords)},
    {kNuiCameraSetExposureRegionOfInterestDecompWords,
     std::size(kNuiCameraSetExposureRegionOfInterestDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraGetExposureRegionOfInterestOrigWords[] = {
    {0x7C852378, kFullMask},
    {0x38800000, kFullMask},
    {0x4BFFFA48, kBranchMask},
    {0x00000000, kFullMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xDBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraGetExposureRegionOfInterestDecompWords[] = {
    {0x7C852378, kFullMask},
    {0x38800000, kFullMask},
    {0x4BFFFA58, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xDBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x7C6B07B4, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraGetExposureRegionOfInterestVariants[] = {
    {kNuiCameraGetExposureRegionOfInterestOrigWords,
     std::size(kNuiCameraGetExposureRegionOfInterestOrigWords)},
    {kNuiCameraGetExposureRegionOfInterestDecompWords,
     std::size(kNuiCameraGetExposureRegionOfInterestDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraGetPropertyOrigWords[] = {
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x38800000, kFullMask},
    {0x4BFFF874, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraGetPropertyDecompWords[] = {
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x38800000, kFullMask},
    {0x4BFFF85C, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraGetPropertyVariants[] = {
    {kNuiCameraGetPropertyOrigWords, std::size(kNuiCameraGetPropertyOrigWords)},
    {kNuiCameraGetPropertyDecompWords,
     std::size(kNuiCameraGetPropertyDecompWords)},
};

constexpr Dc3SignatureWord kNuiCameraGetPropertyFOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D608312, kFullMask},
    {0x7CBE2B78, kFullMask},
    {0x816BC89C, kFullMask},
};
constexpr Dc3SignatureWord kNuiCameraGetPropertyFDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x7CBE2B78, kFullMask},
    {0x816B3EB4, kFullMask},
};
constexpr Dc3SignatureVariant kNuiCameraGetPropertyFVariants[] = {
    {kNuiCameraGetPropertyFOrigWords, std::size(kNuiCameraGetPropertyFOrigWords)},
    {kNuiCameraGetPropertyFDecompWords,
     std::size(kNuiCameraGetPropertyFDecompWords)},
};

constexpr Dc3SignatureWord kNuiIdentityEnrollOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFDA269, kBranchMask},
    {0x9421FF50, kFullMask},
    {0x54AB003C, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x7CBE2B78, kFullMask},
    {0x7CDA3378, kFullMask},
};
constexpr Dc3SignatureWord kNuiIdentityEnrollDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B56713D, kBranchMask},
    {0x9421FF50, kFullMask},
    {0x54AB003C, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x7CBE2B78, kFullMask},
    {0x7CDA3378, kFullMask},
};
constexpr Dc3SignatureVariant kNuiIdentityEnrollVariants[] = {
    {kNuiIdentityEnrollOrigWords, std::size(kNuiIdentityEnrollOrigWords)},
    {kNuiIdentityEnrollDecompWords, std::size(kNuiIdentityEnrollDecompWords)},
};

constexpr Dc3SignatureWord kNuiIdentityIdentifyOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFDA0B1, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x3D608312, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x396BC8A0, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x7CBC2B78, kFullMask},
};
constexpr Dc3SignatureWord kNuiIdentityIdentifyDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B566F89, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x396B3EB8, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x7CBC2B78, kFullMask},
};
constexpr Dc3SignatureVariant kNuiIdentityIdentifyVariants[] = {
    {kNuiIdentityIdentifyOrigWords, std::size(kNuiIdentityIdentifyOrigWords)},
    {kNuiIdentityIdentifyDecompWords,
     std::size(kNuiIdentityIdentifyDecompWords)},
};

constexpr Dc3SignatureWord kNuiIdentityGetEnrollmentInformationOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFD9F91, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x2B030008, kFullMask},
    {0x41980010, kFullMask},
    {0x3C608007, kFullMask},
};
constexpr Dc3SignatureWord kNuiIdentityGetEnrollmentInformationDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B566E69, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x2B030008, kFullMask},
    {0x41980010, kFullMask},
    {0x3C608007, kFullMask},
};
constexpr Dc3SignatureVariant kNuiIdentityGetEnrollmentInformationVariants[] = {
    {kNuiIdentityGetEnrollmentInformationOrigWords,
     std::size(kNuiIdentityGetEnrollmentInformationOrigWords)},
    {kNuiIdentityGetEnrollmentInformationDecompWords,
     std::size(kNuiIdentityGetEnrollmentInformationDecompWords)},
};

constexpr Dc3SignatureWord kNuiIdentityAbortOrigWords[] = {
    {0x38600000, kFullMask},
    {0x4BFFFEE4, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x81630008, kFullMask},
    {0x81430004, kFullMask},
    {0x7D6907B4, kFullMask},
};
constexpr Dc3SignatureWord kNuiIdentityAbortDecompWords[] = {
    {0x38600000, kFullMask},
    {0x4BFFFEE8, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x81630008, kFullMask},
    {0x81430004, kFullMask},
    {0x7D6907B4, kFullMask},
};
constexpr Dc3SignatureVariant kNuiIdentityAbortVariants[] = {
    {kNuiIdentityAbortOrigWords, std::size(kNuiIdentityAbortOrigWords)},
    {kNuiIdentityAbortDecompWords, std::size(kNuiIdentityAbortDecompWords)},
};

// BEGIN AUTOGEN SIG TRANCHE: speech_fitness_wave_head_nuip
constexpr Dc3SignatureWord kNuiFitnessStartTrackingOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCBDA5, kBranchMask},
    {0xDBA1FF90, kFullMask},
    {0xDBC1FF98, kFullMask},
    {0xDBE1FFA0, kFullMask},
    {0x9421FF20, kFullMask},
    {0x3D608312, kFullMask},
    {0x3BA00000, kFullMask},
};

constexpr Dc3SignatureWord kNuiFitnessStartTrackingDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B559055, kBranchMask},
    {0xDBA1FF90, kFullMask},
    {0xDBC1FF98, kFullMask},
    {0xDBE1FFA0, kFullMask},
    {0x9421FF20, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x3BA00000, kFullMask},
};

constexpr Dc3SignatureVariant kNuiFitnessStartTrackingVariants[] = {
    {kNuiFitnessStartTrackingOrigWords, std::size(kNuiFitnessStartTrackingOrigWords)},
    {kNuiFitnessStartTrackingDecompWords, std::size(kNuiFitnessStartTrackingDecompWords)},
};

constexpr Dc3SignatureWord kNuiFitnessPauseTrackingOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D608313, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BEBCDA0, kFullMask},
};

constexpr Dc3SignatureWord kNuiFitnessPauseTrackingDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BEB43B8, kFullMask},
};

constexpr Dc3SignatureVariant kNuiFitnessPauseTrackingVariants[] = {
    {kNuiFitnessPauseTrackingOrigWords, std::size(kNuiFitnessPauseTrackingOrigWords)},
    {kNuiFitnessPauseTrackingDecompWords, std::size(kNuiFitnessPauseTrackingDecompWords)},
};

constexpr Dc3SignatureWord kNuiFitnessResumeTrackingOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCBA29, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D608313, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BEBCDA0, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x387F0008, kFullMask},
};

constexpr Dc3SignatureWord kNuiFitnessResumeTrackingDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B558CD9, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BEB43B8, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x387F0008, kFullMask},
};

constexpr Dc3SignatureVariant kNuiFitnessResumeTrackingVariants[] = {
    {kNuiFitnessResumeTrackingOrigWords, std::size(kNuiFitnessResumeTrackingOrigWords)},
    {kNuiFitnessResumeTrackingDecompWords, std::size(kNuiFitnessResumeTrackingDecompWords)},
};

constexpr Dc3SignatureWord kNuiFitnessStopTrackingOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCB941, kBranchMask},
    {0xDBC1FFA0, kFullMask},
    {0xDBE1FFA8, kFullMask},
    {0x9421FF30, kFullMask},
    {0x3D608313, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BEBCDA0, kFullMask},
};

constexpr Dc3SignatureWord kNuiFitnessStopTrackingDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B558BF1, kBranchMask},
    {0xDBC1FFA0, kFullMask},
    {0xDBE1FFA8, kFullMask},
    {0x9421FF30, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BEB43B8, kFullMask},
};

constexpr Dc3SignatureVariant kNuiFitnessStopTrackingVariants[] = {
    {kNuiFitnessStopTrackingOrigWords, std::size(kNuiFitnessStopTrackingOrigWords)},
    {kNuiFitnessStopTrackingDecompWords, std::size(kNuiFitnessStopTrackingDecompWords)},
};

constexpr Dc3SignatureWord kNuiFitnessGetCurrentFitnessDataOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BB3C299, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D608313, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x3BABCDA0, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x387D0008, kFullMask},
};

constexpr Dc3SignatureWord kNuiFitnessGetCurrentFitnessDataDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B23106D, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x3BAB43B8, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x387D0008, kFullMask},
};

constexpr Dc3SignatureVariant kNuiFitnessGetCurrentFitnessDataVariants[] = {
    {kNuiFitnessGetCurrentFitnessDataOrigWords, std::size(kNuiFitnessGetCurrentFitnessDataOrigWords)},
    {kNuiFitnessGetCurrentFitnessDataDecompWords, std::size(kNuiFitnessGetCurrentFitnessDataDecompWords)},
};

constexpr Dc3SignatureWord kNuiWaveSetEnabledOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCC1CD, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D608312, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x396BC8A0, kFullMask},
    {0x814B0874, kFullMask},
    {0x2F0A0000, kFullMask},
};

constexpr Dc3SignatureWord kNuiWaveSetEnabledDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B559471, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083C5, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x396B3EB8, kFullMask},
    {0x814B0874, kFullMask},
    {0x2F0A0000, kFullMask},
};

constexpr Dc3SignatureVariant kNuiWaveSetEnabledVariants[] = {
    {kNuiWaveSetEnabledOrigWords, std::size(kNuiWaveSetEnabledOrigWords)},
    {kNuiWaveSetEnabledDecompWords, std::size(kNuiWaveSetEnabledDecompWords)},
};

constexpr Dc3SignatureWord kNuiWaveGetGestureOwnerProgressOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4BFCC2C1, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0010, kFullMask},
    {0x3C608007, kFullMask},
};

constexpr Dc3SignatureWord kNuiWaveGetGestureOwnerProgressDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B559565, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0010, kFullMask},
    {0x3C608007, kFullMask},
};

constexpr Dc3SignatureVariant kNuiWaveGetGestureOwnerProgressVariants[] = {
    {kNuiWaveGetGestureOwnerProgressOrigWords, std::size(kNuiWaveGetGestureOwnerProgressOrigWords)},
    {kNuiWaveGetGestureOwnerProgressDecompWords, std::size(kNuiWaveGetGestureOwnerProgressDecompWords)},
};

constexpr Dc3SignatureWord kNuiHeadOrientationDisableOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x3BCBFBF4, kFullMask},
    {0x7FC3F378, kFullMask},
};

constexpr Dc3SignatureWord kNuiHeadOrientationDisableDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x3BCBC224, kFullMask},
    {0x7FC3F378, kFullMask},
};

constexpr Dc3SignatureVariant kNuiHeadOrientationDisableVariants[] = {
    {kNuiHeadOrientationDisableOrigWords, std::size(kNuiHeadOrientationDisableOrigWords)},
    {kNuiHeadOrientationDisableDecompWords, std::size(kNuiHeadOrientationDisableDecompWords)},
};

constexpr Dc3SignatureWord kNuiHeadPositionDisableOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6082F2, kFullMask},
    {0x3BCBF8FC, kFullMask},
    {0x7FC3F378, kFullMask},
};

constexpr Dc3SignatureWord kNuiHeadPositionDisableDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A3, kFullMask},
    {0x3BCB8A48, kFullMask},
    {0x7FC3F378, kFullMask},
};

constexpr Dc3SignatureVariant kNuiHeadPositionDisableVariants[] = {
    {kNuiHeadPositionDisableOrigWords, std::size(kNuiHeadPositionDisableOrigWords)},
    {kNuiHeadPositionDisableDecompWords, std::size(kNuiHeadPositionDisableDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechEnableOrigWords[] = {
    {0x7C862378, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0010, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
    {0x4E800020, kFullMask},
    {0x3D608316, kFullMask},
    {0x80A30004, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechEnableDecompWords[] = {
    {0x7C862378, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0010, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
    {0x4E800020, kFullMask},
    {0x3D6083C0, kFullMask},
    {0x80A30004, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechEnableVariants[] = {
    {kNuiSpeechEnableOrigWords, std::size(kNuiSpeechEnableOrigWords)},
    {kNuiSpeechEnableDecompWords, std::size(kNuiSpeechEnableDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechDisableOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFEEE0, kBranchMask},
    {0x00000000, kFullMask},
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechDisableDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFEEF4, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechDisableVariants[] = {
    {kNuiSpeechDisableOrigWords, std::size(kNuiSpeechDisableOrigWords)},
    {kNuiSpeechDisableDecompWords, std::size(kNuiSpeechDisableDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechCreateGrammarOrigWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF044, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechCreateGrammarDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF058, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechCreateGrammarVariants[] = {
    {kNuiSpeechCreateGrammarOrigWords, std::size(kNuiSpeechCreateGrammarOrigWords)},
    {kNuiSpeechCreateGrammarDecompWords, std::size(kNuiSpeechCreateGrammarDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechLoadGrammarOrigWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF17C, kBranchMask},
};

constexpr Dc3SignatureWord kNuiSpeechLoadGrammarDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF190, kBranchMask},
};

constexpr Dc3SignatureVariant kNuiSpeechLoadGrammarVariants[] = {
    {kNuiSpeechLoadGrammarOrigWords, std::size(kNuiSpeechLoadGrammarOrigWords)},
    {kNuiSpeechLoadGrammarDecompWords, std::size(kNuiSpeechLoadGrammarDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechUnloadGrammarOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFF2CC, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechUnloadGrammarDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFF2E0, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechUnloadGrammarVariants[] = {
    {kNuiSpeechUnloadGrammarOrigWords, std::size(kNuiSpeechUnloadGrammarOrigWords)},
    {kNuiSpeechUnloadGrammarDecompWords, std::size(kNuiSpeechUnloadGrammarDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechCommitGrammarOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFF73C, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x4BF7AECD, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3BC00000, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechCommitGrammarDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFF768, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x4B86DA41, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3BC00000, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechCommitGrammarVariants[] = {
    {kNuiSpeechCommitGrammarOrigWords, std::size(kNuiSpeechCommitGrammarOrigWords)},
    {kNuiSpeechCommitGrammarDecompWords, std::size(kNuiSpeechCommitGrammarDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechStartRecognitionOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x38800000, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFF70C, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechStartRecognitionDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x38800000, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFF728, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechStartRecognitionVariants[] = {
    {kNuiSpeechStartRecognitionOrigWords, std::size(kNuiSpeechStartRecognitionOrigWords)},
    {kNuiSpeechStartRecognitionDecompWords, std::size(kNuiSpeechStartRecognitionDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechStopRecognitionOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x38800000, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFE84C, kBranchMask},
    {0x3D608316, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFF39C, kBranchMask},
};

constexpr Dc3SignatureWord kNuiSpeechStopRecognitionDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x38800000, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFE884, kBranchMask},
    {0x3D6083C0, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFF3C4, kBranchMask},
};

constexpr Dc3SignatureVariant kNuiSpeechStopRecognitionVariants[] = {
    {kNuiSpeechStopRecognitionOrigWords, std::size(kNuiSpeechStopRecognitionOrigWords)},
    {kNuiSpeechStopRecognitionDecompWords, std::size(kNuiSpeechStopRecognitionDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechSetEventInterestOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFFB3C, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechSetEventInterestDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFFB54, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechSetEventInterestVariants[] = {
    {kNuiSpeechSetEventInterestOrigWords, std::size(kNuiSpeechSetEventInterestOrigWords)},
    {kNuiSpeechSetEventInterestDecompWords, std::size(kNuiSpeechSetEventInterestDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechSetGrammarStateOrigWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFFABC, kBranchMask},
    {0x3D608316, kFullMask},
    {0x7C641B78, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechSetGrammarStateDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFFAD4, kBranchMask},
    {0x3D6083C0, kFullMask},
    {0x7C641B78, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechSetGrammarStateVariants[] = {
    {kNuiSpeechSetGrammarStateOrigWords, std::size(kNuiSpeechSetGrammarStateOrigWords)},
    {kNuiSpeechSetGrammarStateDecompWords, std::size(kNuiSpeechSetGrammarStateDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechSetRuleStateOrigWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF3F8, kBranchMask},
    {0x00000000, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechSetRuleStateDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF420, kBranchMask},
    {0x7CE83B78, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechSetRuleStateVariants[] = {
    {kNuiSpeechSetRuleStateOrigWords, std::size(kNuiSpeechSetRuleStateOrigWords)},
    {kNuiSpeechSetRuleStateDecompWords, std::size(kNuiSpeechSetRuleStateDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechCreateRuleOrigWords[] = {
    {0x7CE83B78, kFullMask},
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechCreateRuleDecompWords[] = {
    {0x7CE83B78, kFullMask},
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechCreateRuleVariants[] = {
    {kNuiSpeechCreateRuleOrigWords, std::size(kNuiSpeechCreateRuleOrigWords)},
    {kNuiSpeechCreateRuleDecompWords, std::size(kNuiSpeechCreateRuleDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechCreateStateOrigWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF518, kBranchMask},
    {0x00000000, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechCreateStateDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF544, kBranchMask},
    {0x7D8802A6, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechCreateStateVariants[] = {
    {kNuiSpeechCreateStateOrigWords, std::size(kNuiSpeechCreateStateOrigWords)},
    {kNuiSpeechCreateStateDecompWords, std::size(kNuiSpeechCreateStateDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechAddWordTransitionOrigWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7D094378, kFullMask},
    {0x91410054, kFullMask},
    {0x7CE83B78, kFullMask},
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechAddWordTransitionDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0x9421FFA0, kFullMask},
    {0x7D094378, kFullMask},
    {0x91410054, kFullMask},
    {0x7CE83B78, kFullMask},
    {0x7C6B1B78, kFullMask},
    {0x7CC73378, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechAddWordTransitionVariants[] = {
    {kNuiSpeechAddWordTransitionOrigWords, std::size(kNuiSpeechAddWordTransitionOrigWords)},
    {kNuiSpeechAddWordTransitionDecompWords, std::size(kNuiSpeechAddWordTransitionDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechGetEventsOrigWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386AAA30, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFFC48, kBranchMask},
    {0x00000000, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechGetEventsDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFFC5C, kBranchMask},
    {0x7D8802A6, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechGetEventsVariants[] = {
    {kNuiSpeechGetEventsOrigWords, std::size(kNuiSpeechGetEventsOrigWords)},
    {kNuiSpeechGetEventsDecompWords, std::size(kNuiSpeechGetEventsDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechDestroyEventOrigWords[] = {
    {0x3D608316, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386BAA30, kFullMask},
    {0x4BFFF39C, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D408316, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
};

constexpr Dc3SignatureWord kNuiSpeechDestroyEventDecompWords[] = {
    {0x3D6083C0, kFullMask},
    {0x7C641B78, kFullMask},
    {0x386B9108, kFullMask},
    {0x4BFFF3C4, kBranchMask},
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7CA62B78, kFullMask},
    {0x7C852378, kFullMask},
};

constexpr Dc3SignatureVariant kNuiSpeechDestroyEventVariants[] = {
    {kNuiSpeechDestroyEventOrigWords, std::size(kNuiSpeechDestroyEventOrigWords)},
    {kNuiSpeechDestroyEventDecompWords, std::size(kNuiSpeechDestroyEventDecompWords)},
};

constexpr Dc3SignatureWord kNuiSpeechEmulateRecognitionDecompWords[] = {
    {0x7C6B1B78, kFullMask},
    {0x3D4083C0, kFullMask},
    {0x7C852378, kFullMask},
    {0x386A9108, kFullMask},
    {0x7D645B78, kFullMask},
    {0x4BFFF510, kBranchMask},
    {0x7D8802A6, kFullMask},
    {0x4B86BA05, kBranchMask},
};

constexpr Dc3SignatureVariant kNuiSpeechEmulateRecognitionVariants[] = {
    {kNuiSpeechEmulateRecognitionDecompWords, std::size(kNuiSpeechEmulateRecognitionDecompWords)},
};

constexpr Dc3SignatureWord kD3DDeviceNuiMetaDataDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B3A484D, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x7C7F1B78, kFullMask},
    {0x7C9A2378, kFullMask},
    {0x7CBB2B78, kFullMask},
    {0x7CDD3378, kFullMask},
    {0x7DAD6B78, kFullMask},
};

constexpr Dc3SignatureVariant kD3DDeviceNuiMetaDataVariants[] = {
    {kD3DDeviceNuiMetaDataDecompWords, std::size(kD3DDeviceNuiMetaDataDecompWords)},
};

constexpr Dc3SignatureWord kNuipBuildXamNuiFrameDataDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B54F4E9, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x7C9D2378, kFullMask},
    {0x7CBC2B78, kFullMask},
    {0x38A00AE0, kFullMask},
    {0x38800000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipBuildXamNuiFrameDataVariants[] = {
    {kNuipBuildXamNuiFrameDataDecompWords, std::size(kNuipBuildXamNuiFrameDataDecompWords)},
};

constexpr Dc3SignatureWord kNuipCameraGetExposureRegionOfInterestDecompWords[] = {
    {0x2F040002, kFullMask},
    {0x41980010, kFullMask},
    {0x3C608007, kFullMask},
    {0x60630057, kFullMask},
    {0x4E800020, kFullMask},
    {0x2F030000, kFullMask},
    {0x409AFFF0, kFullMask},
    {0x3D6083C5, kFullMask},
};

constexpr Dc3SignatureVariant kNuipCameraGetExposureRegionOfInterestVariants[] = {
    {kNuipCameraGetExposureRegionOfInterestDecompWords, std::size(kNuipCameraGetExposureRegionOfInterestDecompWords)},
};

constexpr Dc3SignatureWord kNuipCameraGetPropertyDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B563F89, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x7CDE3378, kFullMask},
    {0x3BA00000, kFullMask},
    {0x2F040002, kFullMask},
    {0x41980014, kFullMask},
};

constexpr Dc3SignatureVariant kNuipCameraGetPropertyVariants[] = {
    {kNuipCameraGetPropertyDecompWords, std::size(kNuipCameraGetPropertyDecompWords)},
};

constexpr Dc3SignatureWord kNuipCameraGetPropertyFDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B563C81, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C6A1B78, kFullMask},
    {0x7C832378, kFullMask},
    {0x7CDD3378, kFullMask},
    {0x2F040002, kFullMask},
    {0x40980248, kFullMask},
};

constexpr Dc3SignatureVariant kNuipCameraGetPropertyFVariants[] = {
    {kNuipCameraGetPropertyFDecompWords, std::size(kNuipCameraGetPropertyFDecompWords)},
};

constexpr Dc3SignatureWord kNuipCreateInstanceDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F7D19, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x39400000, kFullMask},
    {0x7C6B1B78, kFullMask},
    {0x91410050, kFullMask},
    {0x3D20822B, kFullMask},
    {0x3D4083C9, kFullMask},
};

constexpr Dc3SignatureVariant kNuipCreateInstanceVariants[] = {
    {kNuipCreateInstanceDecompWords, std::size(kNuipCreateInstanceDecompWords)},
};

constexpr Dc3SignatureWord kNuipFitnessInitializeDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B5591E5, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x3BEB43B8, kFullMask},
    {0x387F0008, kFullMask},
    {0x48383455, kBranchMask},
    {0x39600000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipFitnessInitializeVariants[] = {
    {kNuipFitnessInitializeDecompWords, std::size(kNuipFitnessInitializeDecompWords)},
};

constexpr Dc3SignatureWord kNuipFitnessNewSkeletalFrameDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B558935, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x7C7E1B78, kFullMask},
    {0x3BAB43B8, kFullMask},
    {0x387D0008, kFullMask},
    {0x48382BA5, kBranchMask},
};

constexpr Dc3SignatureVariant kNuipFitnessNewSkeletalFrameVariants[] = {
    {kNuipFitnessNewSkeletalFrameDecompWords, std::size(kNuipFitnessNewSkeletalFrameDecompWords)},
};

constexpr Dc3SignatureWord kNuipFitnessShutdownDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B559101, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x3D6083C6, kFullMask},
    {0x3BEB43B8, kFullMask},
    {0x387F0008, kFullMask},
    {0x48383379, kBranchMask},
    {0x3B600000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipFitnessShutdownVariants[] = {
    {kNuipFitnessShutdownDecompWords, std::size(kNuipFitnessShutdownDecompWords)},
};

constexpr Dc3SignatureWord kNuipInitializeDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B55A375, kBranchMask},
    {0x9421FCC0, kFullMask},
    {0x3D8033FF, kFullMask},
    {0x90A10364, kFullMask},
    {0x3B600000, kFullMask},
    {0x618C0314, kFullMask},
    {0x7C771B78, kFullMask},
};

constexpr Dc3SignatureVariant kNuipInitializeVariants[] = {
    {kNuipInitializeDecompWords, std::size(kNuipInitializeDecompWords)},
};

constexpr Dc3SignatureWord kNuipLoadRegistryDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x396BCE80, kFullMask},
    {0x814B000C, kFullMask},
};

constexpr Dc3SignatureVariant kNuipLoadRegistryVariants[] = {
    {kNuipLoadRegistryDecompWords, std::size(kNuipLoadRegistryDecompWords)},
};

constexpr Dc3SignatureWord kNuipModuleInitDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F7C75, kBranchMask},
    {0x9421FF40, kFullMask},
    {0x3BE00000, kFullMask},
    {0x7C7D1B78, kFullMask},
    {0x7C9E2378, kFullMask},
    {0x7CB72B78, kFullMask},
    {0x2B030000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipModuleInitVariants[] = {
    {kNuipModuleInitDecompWords, std::size(kNuipModuleInitDecompWords)},
};

constexpr Dc3SignatureWord kNuipModuleTermDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x9181FFF8, kFullMask},
    {0xFBC1FFE8, kFullMask},
    {0xFBE1FFF0, kFullMask},
    {0x9421FF90, kFullMask},
    {0x48005F91, kBranchMask},
    {0x3D6083C9, kFullMask},
    {0x386BCAE8, kFullMask},
};

constexpr Dc3SignatureVariant kNuipModuleTermVariants[] = {
    {kNuipModuleTermDecompWords, std::size(kNuipModuleTermDecompWords)},
};

constexpr Dc3SignatureWord kNuipRegCreateKeyExWDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F0E6D, kBranchMask},
    {0x9421FB50, kFullMask},
    {0x7CFB3B78, kFullMask},
    {0x7D475378, kFullMask},
    {0x7D064378, kFullMask},
    {0x7F65DB78, kFullMask},
    {0x7C7F1B78, kFullMask},
};

constexpr Dc3SignatureVariant kNuipRegCreateKeyExWVariants[] = {
    {kNuipRegCreateKeyExWDecompWords, std::size(kNuipRegCreateKeyExWDecompWords)},
};

constexpr Dc3SignatureWord kNuipRegEnumKeyExWDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F21A5, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C681B78, kFullMask},
    {0x7CBE2B78, kFullMask},
    {0x7CDD3378, kFullMask},
    {0x3B800000, kFullMask},
    {0x2B050000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipRegEnumKeyExWVariants[] = {
    {kNuipRegEnumKeyExWDecompWords, std::size(kNuipRegEnumKeyExWDecompWords)},
};

constexpr Dc3SignatureWord kNuipRegEnumValueWDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F209D, kBranchMask},
    {0x9421FF60, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x396BCE80, kFullMask},
    {0x7CBE2B78, kFullMask},
    {0x7CDD3378, kFullMask},
};

constexpr Dc3SignatureVariant kNuipRegEnumValueWVariants[] = {
    {kNuipRegEnumValueWDecompWords, std::size(kNuipRegEnumValueWDecompWords)},
};

constexpr Dc3SignatureWord kNuipRegOpenKeyExWDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F10E9, kBranchMask},
    {0x9421FD70, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x3940FFFF, kFullMask},
    {0x396BCE80, kFullMask},
    {0x3BC00000, kFullMask},
    {0x91470000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipRegOpenKeyExWVariants[] = {
    {kNuipRegOpenKeyExWDecompWords, std::size(kNuipRegOpenKeyExWDecompWords)},
};

constexpr Dc3SignatureWord kNuipRegQueryValueExWDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F1A71, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x7CDF3378, kFullMask},
    {0x7CFD3B78, kFullMask},
    {0x7D1E4378, kFullMask},
    {0x2B080000, kFullMask},
    {0x409A0014, kFullMask},
};

constexpr Dc3SignatureVariant kNuipRegQueryValueExWVariants[] = {
    {kNuipRegQueryValueExWDecompWords, std::size(kNuipRegQueryValueExWDecompWords)},
};

constexpr Dc3SignatureWord kNuipRegSetValueExWDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F1499, kBranchMask},
    {0x9421F700, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x396BCE80, kFullMask},
    {0x7C9F2378, kFullMask},
    {0x7CD73378, kFullMask},
};

constexpr Dc3SignatureVariant kNuipRegSetValueExWVariants[] = {
    {kNuipRegSetValueExWDecompWords, std::size(kNuipRegSetValueExWDecompWords)},
};

constexpr Dc3SignatureWord kNuipUnloadRegistryDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B4F1B5D, kBranchMask},
    {0x9421FF70, kFullMask},
    {0x3D6083A4, kFullMask},
    {0x39400003, kFullMask},
    {0x396BCE80, kFullMask},
    {0x3B200000, kFullMask},
    {0x396BFFFC, kFullMask},
};

constexpr Dc3SignatureVariant kNuipUnloadRegistryVariants[] = {
    {kNuipUnloadRegistryDecompWords, std::size(kNuipUnloadRegistryDecompWords)},
};

constexpr Dc3SignatureWord kNuipWaveInitDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B5596D5, kBranchMask},
    {0x9421FF80, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x38600000, kFullMask},
    {0x3BA00000, kFullMask},
    {0x3BC00000, kFullMask},
    {0x2F030000, kFullMask},
};

constexpr Dc3SignatureVariant kNuipWaveInitVariants[] = {
    {kNuipWaveInitDecompWords, std::size(kNuipWaveInitDecompWords)},
};

constexpr Dc3SignatureWord kNuipWaveUpdateDecompWords[] = {
    {0x7D8802A6, kFullMask},
    {0x4B559659, kBranchMask},
    {0xDBE1FFA8, kFullMask},
    {0x9421FF50, kFullMask},
    {0x7C7C1B78, kFullMask},
    {0x2B030000, kFullMask},
    {0x409A0010, kFullMask},
    {0x3C608007, kFullMask},
};

constexpr Dc3SignatureVariant kNuipWaveUpdateVariants[] = {
    {kNuipWaveUpdateDecompWords, std::size(kNuipWaveUpdateDecompWords)},
};
// END AUTOGEN SIG TRANCHE: speech_fitness_wave_head_nuip
constexpr Dc3TargetSignatureSet kTargetSignatureSets[] = {
    {"NuiInitialize", kNuiInitializeVariants, std::size(kNuiInitializeVariants)},
    {"NuiShutdown", kNuiShutdownVariants, std::size(kNuiShutdownVariants)},
    {"NuiMetaCpuEvent", kNuiMetaCpuEventVariants, std::size(kNuiMetaCpuEventVariants)},
    {"NuiImageStreamOpen", kNuiImageStreamOpenVariants,
     std::size(kNuiImageStreamOpenVariants)},
    {"NuiImageStreamGetNextFrame", kNuiImageStreamGetNextFrameVariants,
     std::size(kNuiImageStreamGetNextFrameVariants)},
    {"NuiImageStreamReleaseFrame", kNuiImageStreamReleaseFrameVariants,
     std::size(kNuiImageStreamReleaseFrameVariants)},
    {"NuiAudioCreate", kNuiAudioCreateVariants, std::size(kNuiAudioCreateVariants)},
    {"NuiAudioCreatePrivate", kNuiAudioCreatePrivateVariants,
     std::size(kNuiAudioCreatePrivateVariants)},
    {"NuiAudioRegisterCallbacks", kNuiAudioRegisterCallbacksVariants,
     std::size(kNuiAudioRegisterCallbacksVariants)},
    {"NuiAudioUnregisterCallbacks", kNuiAudioUnregisterCallbacksVariants,
     std::size(kNuiAudioUnregisterCallbacksVariants)},
    {"NuiAudioRegisterCallbacksPrivate", kNuiAudioRegisterCallbacksPrivateVariants,
     std::size(kNuiAudioRegisterCallbacksPrivateVariants)},
    {"NuiAudioUnregisterCallbacksPrivate",
     kNuiAudioUnregisterCallbacksPrivateVariants,
     std::size(kNuiAudioUnregisterCallbacksPrivateVariants)},
    {"NuiAudioRelease", kNuiAudioReleaseVariants, std::size(kNuiAudioReleaseVariants)},
    {"NuiSkeletonTrackingEnable", kNuiSkeletonTrackingEnableVariants,
     std::size(kNuiSkeletonTrackingEnableVariants)},
    {"NuiSkeletonTrackingDisable", kNuiSkeletonTrackingDisableVariants,
     std::size(kNuiSkeletonTrackingDisableVariants)},
    {"NuiSkeletonSetTrackedSkeletons", kNuiSkeletonSetTrackedSkeletonsVariants,
     std::size(kNuiSkeletonSetTrackedSkeletonsVariants)},
    {"NuiSkeletonGetNextFrame", kNuiSkeletonGetNextFrameVariants,
     std::size(kNuiSkeletonGetNextFrameVariants)},
    {"NuiImageGetColorPixelCoordinatesFromDepthPixel",
     kNuiImageGetColorPixelCoordinatesFromDepthPixelVariants,
     std::size(kNuiImageGetColorPixelCoordinatesFromDepthPixelVariants)},
    {"NuiCameraSetProperty", kNuiCameraSetPropertyVariants,
     std::size(kNuiCameraSetPropertyVariants)},
    {"NuiCameraElevationGetAngle", kNuiCameraElevationGetAngleVariants,
     std::size(kNuiCameraElevationGetAngleVariants)},
    {"NuiCameraElevationSetAngle", kNuiCameraElevationSetAngleVariants,
     std::size(kNuiCameraElevationSetAngleVariants)},
    {"NuiCameraAdjustTilt", kNuiCameraAdjustTiltVariants,
     std::size(kNuiCameraAdjustTiltVariants)},
    {"NuiCameraGetNormalToGravity", kNuiCameraGetNormalToGravityVariants,
     std::size(kNuiCameraGetNormalToGravityVariants)},
    {"NuiCameraSetExposureRegionOfInterest",
     kNuiCameraSetExposureRegionOfInterestVariants,
     std::size(kNuiCameraSetExposureRegionOfInterestVariants)},
    {"NuiCameraGetExposureRegionOfInterest",
     kNuiCameraGetExposureRegionOfInterestVariants,
     std::size(kNuiCameraGetExposureRegionOfInterestVariants)},
    {"NuiCameraGetProperty", kNuiCameraGetPropertyVariants,
     std::size(kNuiCameraGetPropertyVariants)},
    {"NuiCameraGetPropertyF", kNuiCameraGetPropertyFVariants,
     std::size(kNuiCameraGetPropertyFVariants)},
    {"NuiIdentityEnroll", kNuiIdentityEnrollVariants,
     std::size(kNuiIdentityEnrollVariants)},
    {"NuiIdentityIdentify", kNuiIdentityIdentifyVariants,
     std::size(kNuiIdentityIdentifyVariants)},
    {"NuiIdentityGetEnrollmentInformation",
     kNuiIdentityGetEnrollmentInformationVariants,
     std::size(kNuiIdentityGetEnrollmentInformationVariants)},
    {"NuiIdentityAbort", kNuiIdentityAbortVariants,
     std::size(kNuiIdentityAbortVariants)},
    // BEGIN AUTOGEN SIG ENTRIES: speech_fitness_wave_head_nuip
    {"NuiFitnessStartTracking", kNuiFitnessStartTrackingVariants, std::size(kNuiFitnessStartTrackingVariants)},
    {"NuiFitnessPauseTracking", kNuiFitnessPauseTrackingVariants, std::size(kNuiFitnessPauseTrackingVariants)},
    {"NuiFitnessResumeTracking", kNuiFitnessResumeTrackingVariants, std::size(kNuiFitnessResumeTrackingVariants)},
    {"NuiFitnessStopTracking", kNuiFitnessStopTrackingVariants, std::size(kNuiFitnessStopTrackingVariants)},
    {"NuiFitnessGetCurrentFitnessData", kNuiFitnessGetCurrentFitnessDataVariants, std::size(kNuiFitnessGetCurrentFitnessDataVariants)},
    {"NuiWaveSetEnabled", kNuiWaveSetEnabledVariants, std::size(kNuiWaveSetEnabledVariants)},
    {"NuiWaveGetGestureOwnerProgress", kNuiWaveGetGestureOwnerProgressVariants, std::size(kNuiWaveGetGestureOwnerProgressVariants)},
    {"NuiHeadOrientationDisable", kNuiHeadOrientationDisableVariants, std::size(kNuiHeadOrientationDisableVariants)},
    {"NuiHeadPositionDisable", kNuiHeadPositionDisableVariants, std::size(kNuiHeadPositionDisableVariants)},
    {"NuiSpeechEnable", kNuiSpeechEnableVariants, std::size(kNuiSpeechEnableVariants)},
    {"NuiSpeechDisable", kNuiSpeechDisableVariants, std::size(kNuiSpeechDisableVariants)},
    {"NuiSpeechCreateGrammar", kNuiSpeechCreateGrammarVariants, std::size(kNuiSpeechCreateGrammarVariants)},
    {"NuiSpeechLoadGrammar", kNuiSpeechLoadGrammarVariants, std::size(kNuiSpeechLoadGrammarVariants)},
    {"NuiSpeechUnloadGrammar", kNuiSpeechUnloadGrammarVariants, std::size(kNuiSpeechUnloadGrammarVariants)},
    {"NuiSpeechCommitGrammar", kNuiSpeechCommitGrammarVariants, std::size(kNuiSpeechCommitGrammarVariants)},
    {"NuiSpeechStartRecognition", kNuiSpeechStartRecognitionVariants, std::size(kNuiSpeechStartRecognitionVariants)},
    {"NuiSpeechStopRecognition", kNuiSpeechStopRecognitionVariants, std::size(kNuiSpeechStopRecognitionVariants)},
    {"NuiSpeechSetEventInterest", kNuiSpeechSetEventInterestVariants, std::size(kNuiSpeechSetEventInterestVariants)},
    {"NuiSpeechSetGrammarState", kNuiSpeechSetGrammarStateVariants, std::size(kNuiSpeechSetGrammarStateVariants)},
    {"NuiSpeechSetRuleState", kNuiSpeechSetRuleStateVariants, std::size(kNuiSpeechSetRuleStateVariants)},
    {"NuiSpeechCreateRule", kNuiSpeechCreateRuleVariants, std::size(kNuiSpeechCreateRuleVariants)},
    {"NuiSpeechCreateState", kNuiSpeechCreateStateVariants, std::size(kNuiSpeechCreateStateVariants)},
    {"NuiSpeechAddWordTransition", kNuiSpeechAddWordTransitionVariants, std::size(kNuiSpeechAddWordTransitionVariants)},
    {"NuiSpeechGetEvents", kNuiSpeechGetEventsVariants, std::size(kNuiSpeechGetEventsVariants)},
    {"NuiSpeechDestroyEvent", kNuiSpeechDestroyEventVariants, std::size(kNuiSpeechDestroyEventVariants)},
    {"NuiSpeechEmulateRecognition", kNuiSpeechEmulateRecognitionVariants, std::size(kNuiSpeechEmulateRecognitionVariants)},
    {"D3DDevice_NuiMetaData", kD3DDeviceNuiMetaDataVariants, std::size(kD3DDeviceNuiMetaDataVariants)},
    {"NuipBuildXamNuiFrameData", kNuipBuildXamNuiFrameDataVariants, std::size(kNuipBuildXamNuiFrameDataVariants)},
    {"NuipCameraGetExposureRegionOfInterest", kNuipCameraGetExposureRegionOfInterestVariants, std::size(kNuipCameraGetExposureRegionOfInterestVariants)},
    {"NuipCameraGetProperty", kNuipCameraGetPropertyVariants, std::size(kNuipCameraGetPropertyVariants)},
    {"NuipCameraGetPropertyF", kNuipCameraGetPropertyFVariants, std::size(kNuipCameraGetPropertyFVariants)},
    {"NuipCreateInstance", kNuipCreateInstanceVariants, std::size(kNuipCreateInstanceVariants)},
    {"NuipFitnessInitialize", kNuipFitnessInitializeVariants, std::size(kNuipFitnessInitializeVariants)},
    {"NuipFitnessNewSkeletalFrame", kNuipFitnessNewSkeletalFrameVariants, std::size(kNuipFitnessNewSkeletalFrameVariants)},
    {"NuipFitnessShutdown", kNuipFitnessShutdownVariants, std::size(kNuipFitnessShutdownVariants)},
    {"NuipInitialize", kNuipInitializeVariants, std::size(kNuipInitializeVariants)},
    {"NuipLoadRegistry", kNuipLoadRegistryVariants, std::size(kNuipLoadRegistryVariants)},
    {"NuipModuleInit", kNuipModuleInitVariants, std::size(kNuipModuleInitVariants)},
    {"NuipModuleTerm", kNuipModuleTermVariants, std::size(kNuipModuleTermVariants)},
    {"NuipRegCreateKeyExW", kNuipRegCreateKeyExWVariants, std::size(kNuipRegCreateKeyExWVariants)},
    {"NuipRegEnumKeyExW", kNuipRegEnumKeyExWVariants, std::size(kNuipRegEnumKeyExWVariants)},
    {"NuipRegEnumValueW", kNuipRegEnumValueWVariants, std::size(kNuipRegEnumValueWVariants)},
    {"NuipRegOpenKeyExW", kNuipRegOpenKeyExWVariants, std::size(kNuipRegOpenKeyExWVariants)},
    {"NuipRegQueryValueExW", kNuipRegQueryValueExWVariants, std::size(kNuipRegQueryValueExWVariants)},
    {"NuipRegSetValueExW", kNuipRegSetValueExWVariants, std::size(kNuipRegSetValueExWVariants)},
    {"NuipUnloadRegistry", kNuipUnloadRegistryVariants, std::size(kNuipUnloadRegistryVariants)},
    {"NuipWaveInit", kNuipWaveInitVariants, std::size(kNuipWaveInitVariants)},
    {"NuipWaveUpdate", kNuipWaveUpdateVariants, std::size(kNuipWaveUpdateVariants)},
    // END AUTOGEN SIG ENTRIES: speech_fitness_wave_head_nuip
    {"CXbcImpl::Initialize", kXbcInitVariants, std::size(kXbcInitVariants)},
    {"CXbcImpl::DoWork", kXbcDoWorkVariants, std::size(kXbcDoWorkVariants)},
    {"CXbcImpl::SendJSON", kXbcSendJsonVariants, std::size(kXbcSendJsonVariants)},
    {"D3DDevice_NuiInitialize", kD3DNuiInitializeVariants,
     std::size(kD3DNuiInitializeVariants)},
    {"D3DDevice_NuiStart", kD3DNuiStartVariants, std::size(kD3DNuiStartVariants)},
    {"D3DDevice_NuiStop", kD3DNuiStopVariants, std::size(kD3DNuiStopVariants)},
};

template <typename TString>
void Dc3TrimWhitespace(TString* s) {
  size_t start = 0;
  while (start < s->size() &&
         std::isspace(static_cast<unsigned char>((*s)[start]))) {
    ++start;
  }
  size_t end = s->size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>((*s)[end - 1]))) {
    --end;
  }
  *s = s->substr(start, end - start);
}

bool Dc3ParseManifestU64(const rapidjson::Value& value, uint64_t* out_value) {
  if (value.IsUint64()) {
    *out_value = value.GetUint64();
    return true;
  }
  if (value.IsUint()) {
    *out_value = value.GetUint();
    return true;
  }
  if (value.IsString()) {
    return Dc3TryParseHexU64(value.GetString(), out_value);
  }
  return false;
}

const Dc3TargetSignatureSet* Dc3FindSignatureSet(std::string_view name) {
  for (const auto& set : kTargetSignatureSets) {
    if (name == set.name) {
      return &set;
    }
  }
  return nullptr;
}

bool Dc3SignatureMatchesAt(const uint8_t* text_mem, uint32_t text_address,
                          uint32_t candidate_address,
                          const Dc3SignatureVariant& variant) {
  const uint32_t offset = candidate_address - text_address;
  for (size_t i = 0; i < variant.word_count; ++i) {
    const uint32_t actual = xe::load_and_swap<uint32_t>(text_mem + offset + i * 4);
    const auto& expected = variant.words[i];
    if ((actual & expected.mask) != (expected.value & expected.mask)) {
      return false;
    }
  }
  return true;
}

bool Dc3ValidateSignatureCandidate(const Dc3TextSectionInfo& text_info, Memory* memory,
                                   uint32_t candidate_address,
                                   const Dc3SignatureVariant& variant) {
  if (!Dc3PatchTargetInText(text_info, candidate_address,
                            static_cast<uint32_t>(variant.word_count * 4))) {
    return false;
  }
  auto* mem = memory->TranslateVirtual<uint8_t*>(candidate_address);
  if (!mem) {
    return false;
  }
  const uint32_t first_word = xe::load_and_swap<uint32_t>(mem);
  if (first_word == 0) {
    return false;
  }
  return true;
}

std::optional<uint32_t> Dc3ResolveBySignature(const Dc3NuiPatchSpec& spec,
                                              const Dc3TextSectionInfo& text_info,
                                              Memory* memory) {
  if (!text_info.have_range || text_info.end <= text_info.start || !memory) {
    return std::nullopt;
  }
  const auto* signature_set = Dc3FindSignatureSet(spec.name);
  if (!signature_set) {
    return std::nullopt;
  }
  auto* text_mem = memory->TranslateVirtual<uint8_t*>(text_info.start);
  if (!text_mem) {
    return std::nullopt;
  }
  const uint32_t text_size = text_info.end - text_info.start;
  auto find_nearest_match_in_window =
      [&](uint32_t window_start, uint32_t window_end) -> std::optional<uint32_t> {
    bool found_local = false;
    uint32_t best_match = 0;
    uint32_t best_distance = UINT32_MAX;
    int local_match_count = 0;
    for (size_t variant_index = 0; variant_index < signature_set->variant_count;
         ++variant_index) {
      const auto& variant = signature_set->variants[variant_index];
      const uint32_t sig_size = static_cast<uint32_t>(variant.word_count * 4);
      if (!sig_size) {
        continue;
      }
      if (window_end < window_start || window_end - window_start + 4 < sig_size) {
        continue;
      }
      for (uint32_t candidate = window_start; candidate + sig_size <= window_end + 4;
           candidate += 4) {
        if (!Dc3SignatureMatchesAt(text_mem, text_info.start, candidate, variant)) {
          continue;
        }
        if (!Dc3ValidateSignatureCandidate(text_info, memory, candidate, variant)) {
          continue;
        }
        local_match_count++;
        const uint32_t distance =
            spec.address > candidate ? (spec.address - candidate)
                                     : (candidate - spec.address);
        if (!found_local || distance < best_distance) {
          found_local = true;
          best_match = candidate;
          best_distance = distance;
        }
      }
    }
    if (!found_local) {
      return std::nullopt;
    }
    if (best_match != spec.address) {
      XELOGI(
          "DC3: Signature resolver used local hint window for {} {:08X} -> "
          "{:08X} (delta=0x{:X}, local_matches={})",
          spec.name, spec.address, best_match, best_distance, local_match_count);
    }
    return best_match;
  };
  if (spec.address && Dc3PatchTargetInText(text_info, spec.address, 4)) {
    const uint32_t hint_window = 0x80;
    const uint32_t window_start =
        spec.address > hint_window ? std::max(text_info.start, spec.address - hint_window)
                                   : text_info.start;
    const uint32_t window_end =
        std::min(text_info.end - 4, spec.address + hint_window);
    if (auto local_match = find_nearest_match_in_window(window_start, window_end)) {
      return local_match;
    }
    XELOGW(
        "DC3: Signature resolver local hint window miss for {} near {:08X}; "
        "rejecting global signature scan for safety",
        spec.name, spec.address);
    return std::nullopt;
  }

  uint32_t matched_address = 0;
  bool found = false;
  bool ambiguous = false;
  uint32_t second_match_address = 0;
  bool found_exact_catalog_hint = false;
  bool found_near_catalog_hint = false;
  uint32_t near_catalog_match = 0;
  uint32_t near_catalog_distance = UINT32_MAX;
  for (size_t variant_index = 0; variant_index < signature_set->variant_count;
       ++variant_index) {
    const auto& variant = signature_set->variants[variant_index];
    const uint32_t sig_size = static_cast<uint32_t>(variant.word_count * 4);
    if (sig_size == 0 || sig_size > text_size) {
      continue;
    }
    for (uint32_t offset = 0; offset + sig_size <= text_size; offset += 4) {
      const uint32_t candidate = text_info.start + offset;
      if (!Dc3SignatureMatchesAt(text_mem, text_info.start, candidate, variant)) {
        continue;
      }
      if (!Dc3ValidateSignatureCandidate(text_info, memory, candidate, variant)) {
        continue;
      }
      if (candidate == spec.address) {
        found_exact_catalog_hint = true;
        matched_address = candidate;
        continue;
      }
      const uint32_t distance =
          spec.address > candidate ? (spec.address - candidate)
                                   : (candidate - spec.address);
      if (distance <= 0x80 &&
          (!found_near_catalog_hint || distance < near_catalog_distance)) {
        found_near_catalog_hint = true;
        near_catalog_match = candidate;
        near_catalog_distance = distance;
      }
      if (found && matched_address != candidate) {
        ambiguous = true;
        if (!second_match_address) {
          second_match_address = candidate;
        }
        continue;
      }
      matched_address = candidate;
      found = true;
    }
  }
  if (found_exact_catalog_hint) {
    return spec.address;
  }
  if (found_near_catalog_hint) {
    XELOGI(
        "DC3: Signature resolver disambiguated {} using nearby catalog hint "
        "{:08X} -> {:08X} (delta=0x{:X})",
        spec.name, spec.address, near_catalog_match, near_catalog_distance);
    return near_catalog_match;
  }
  if (ambiguous) {
    XELOGW(
        "DC3: Signature resolver found ambiguous matches for {} "
        "(at least {:08X} and {:08X}); rejecting signature result",
        spec.name, matched_address, second_match_address);
    return std::nullopt;
  }
  if (!found) {
    return std::nullopt;
  }
  return matched_address;
}

}  // namespace

bool Dc3TryParseHexU64(const std::string_view str, uint64_t* out_value) {
  if (str.empty()) {
    return false;
  }
  std::string tmp(str);
  const char* start = tmp.c_str();
  if (tmp.rfind("0x", 0) == 0 || tmp.rfind("0X", 0) == 0) {
    start += 2;
  }
  char* end = nullptr;
  unsigned long long value = std::strtoull(start, &end, 16);
  if (!end || end == start || *end != '\0') {
    return false;
  }
  *out_value = static_cast<uint64_t>(value);
  return true;
}

std::optional<std::filesystem::path> Dc3AutoProbeNuiSymbolMapPath() {
  const std::filesystem::path candidates[] = {
      "/home/free/code/milohax/dc3-decomp/config/373307D9/symbols.txt",
      "../dc3-decomp/config/373307D9/symbols.txt",
      "dc3-decomp/config/373307D9/symbols.txt",
  };
  for (const auto& path : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> Dc3AutoProbeFingerprintCachePath() {
  const std::filesystem::path candidates[] = {
      "/home/free/code/milohax/xenia/docs/dc3-boot/dc3_nui_fingerprints.txt",
      "docs/dc3-boot/dc3_nui_fingerprints.txt",
  };
  for (const auto& path : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> Dc3AutoProbePatchManifestPath() {
  const std::filesystem::path candidates[] = {
      "/home/free/code/milohax/dc3-decomp/build/373307D9/xenia_dc3_patch_manifest.json",
      "../dc3-decomp/build/373307D9/xenia_dc3_patch_manifest.json",
      "dc3-decomp/build/373307D9/xenia_dc3_patch_manifest.json",
  };
  for (const auto& path : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return std::nullopt;
}

std::optional<Dc3NuiSymbolManifest> Dc3LoadNuiSymbolManifest(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }

  Dc3NuiSymbolManifest manifest;
  std::string line;
  while (std::getline(file, line)) {
    auto eq_pos = line.find('=');
    if (eq_pos == std::string::npos) {
      continue;
    }
    auto semicolon_pos = line.find(';', eq_pos + 1);
    if (semicolon_pos == std::string::npos) {
      semicolon_pos = line.size();
    }

    std::string name = line.substr(0, eq_pos);
    std::string rhs = line.substr(eq_pos + 1, semicolon_pos - (eq_pos + 1));
    Dc3TrimWhitespace(&name);
    Dc3TrimWhitespace(&rhs);
    if (name.empty()) {
      continue;
    }
    if (rhs.rfind(".text:0x", 0) != 0 && rhs.rfind(".text:0X", 0) != 0) {
      continue;
    }
    uint64_t addr = 0;
    if (!Dc3TryParseHexU64(rhs.substr(rhs.find(':') + 1), &addr)) {
      continue;
    }
    manifest.text_symbols.emplace(std::move(name), static_cast<uint32_t>(addr));
  }

  return manifest;
}

std::optional<Dc3FingerprintCache> Dc3LoadFingerprintCacheFile(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  Dc3FingerprintCache cache;
  std::string line;
  while (std::getline(file, line)) {
    auto hash_pos = line.find('#');
    if (hash_pos != std::string::npos) {
      line.erase(hash_pos);
    }
    auto eq_pos = line.find('=');
    if (eq_pos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq_pos);
    std::string value = line.substr(eq_pos + 1);
    Dc3TrimWhitespace(&key);
    Dc3TrimWhitespace(&value);
    uint64_t parsed = 0;
    if (!Dc3TryParseHexU64(value, &parsed)) {
      continue;
    }
    if (key == "original") {
      cache.original = parsed;
    } else if (key == "decomp") {
      cache.decomp = parsed;
    }
  }
  return cache;
}

std::optional<Dc3NuiPatchManifest> Dc3LoadNuiPatchManifest(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    XELOGW("DC3: Patch manifest parse error in '{}': {} (offset {})",
           xe::path_to_utf8(path),
           rapidjson::GetParseError_En(doc.GetParseError()),
           static_cast<unsigned>(doc.GetErrorOffset()));
    return std::nullopt;
  }

  Dc3NuiPatchManifest manifest;
  if (auto it = doc.FindMember("build_label");
      it != doc.MemberEnd() && it->value.IsString()) {
    manifest.build_label = it->value.GetString();
  }
  if (auto pe_it = doc.FindMember("pe");
      pe_it != doc.MemberEnd() && pe_it->value.IsObject()) {
    const auto& pe_obj = pe_it->value;
    if (auto text_it = pe_obj.FindMember("text");
        text_it != pe_obj.MemberEnd() && text_it->value.IsObject()) {
      const auto& text_obj = text_it->value;
      if (auto fp_it = text_obj.FindMember("fnv1a64");
          fp_it != text_obj.MemberEnd()) {
        uint64_t parsed = 0;
        if (Dc3ParseManifestU64(fp_it->value, &parsed)) {
          manifest.text_fingerprint = parsed;
        }
      }
      if (auto fp_it = text_obj.FindMember("xenia_runtime_fnv1a64");
          fp_it != text_obj.MemberEnd()) {
        uint64_t parsed = 0;
        if (Dc3ParseManifestU64(fp_it->value, &parsed)) {
          manifest.runtime_text_fingerprint = parsed;
        }
      } else if (auto fp_it = text_obj.FindMember("runtime_fnv1a64");
                 fp_it != text_obj.MemberEnd()) {
        uint64_t parsed = 0;
        if (Dc3ParseManifestU64(fp_it->value, &parsed)) {
          manifest.runtime_text_fingerprint = parsed;
        }
      }
    }
  }

  auto parse_target_table = [](const rapidjson::Value& table,
                               std::unordered_map<std::string, uint32_t>* out) {
    if (!table.IsObject()) {
      return;
    }
    for (auto it = table.MemberBegin(); it != table.MemberEnd(); ++it) {
      if (!it->name.IsString() || !it->value.IsObject()) {
        continue;
      }
      auto addr_it = it->value.FindMember("address");
      if (addr_it == it->value.MemberEnd()) {
        continue;
      }
      uint32_t address = 0;
      if (addr_it->value.IsUint()) {
        address = addr_it->value.GetUint();
      } else if (addr_it->value.IsUint64()) {
        address = static_cast<uint32_t>(addr_it->value.GetUint64());
      } else if (addr_it->value.IsString()) {
        uint64_t parsed = 0;
        if (!Dc3TryParseHexU64(addr_it->value.GetString(), &parsed)) {
          continue;
        }
        address = static_cast<uint32_t>(parsed);
      } else {
        continue;
      }
      (*out)[it->name.GetString()] = address;
    }
  };

  if (auto targets_it = doc.FindMember("targets");
      targets_it != doc.MemberEnd()) {
    parse_target_table(targets_it->value, &manifest.targets);
  }
  if (auto crt_it = doc.FindMember("crt_sentinels");
      crt_it != doc.MemberEnd()) {
    parse_target_table(crt_it->value, &manifest.crt_sentinels);
  }
  return manifest;
}

bool Dc3PatchTargetInText(const Dc3TextSectionInfo& text, uint32_t address,
                          uint32_t size) {
  if (!text.have_range) {
    return true;
  }
  return address >= text.start && address + size <= text.end;
}

const char* Dc3PatchResolveMethodName(Dc3PatchResolveMethod method) {
  switch (method) {
    case Dc3PatchResolveMethod::kPatchManifest:
      return "manifest";
    case Dc3PatchResolveMethod::kCatalogAddress:
      return "catalog";
    case Dc3PatchResolveMethod::kSymbolMap:
      return "symbol";
    case Dc3PatchResolveMethod::kSignatureStub:
      return "signature";
  }
  return "unknown";
}

Dc3ResolvedNuiPatch Dc3ResolveNuiPatchTarget(
    const Dc3NuiPatchSpec& spec, const Dc3TextSectionInfo& text_info,
    const Dc3NuiPatchManifest* patch_manifest,
    const Dc3NuiSymbolManifest* symbol_manifest, std::string_view resolver_mode,
    xe::Memory* memory, bool enable_signature_resolver) {
  Dc3ResolvedNuiPatch resolved;
  resolved.spec = spec;

  const bool strict_mode = resolver_mode == "strict";
  const bool hybrid_mode = resolver_mode == "hybrid";
  const bool legacy_mode = resolver_mode == "legacy";

  auto is_viable_target = [&](uint32_t address) {
    if (!Dc3PatchTargetInText(text_info, address)) {
      return false;
    }
    auto* mem = memory->TranslateVirtual<uint8_t*>(address);
    if (!mem) {
      return false;
    }
    uint32_t first = xe::load_and_swap<uint32_t>(mem);
    return first != 0;
  };

  if ((hybrid_mode || strict_mode) && patch_manifest) {
    auto it = patch_manifest->targets.find(spec.name);
    if (it != patch_manifest->targets.end() && is_viable_target(it->second)) {
      Dc3NuiPatchSpec hinted_spec = spec;
      hinted_spec.address = it->second;
      if (auto sig_addr = Dc3ResolveBySignature(hinted_spec, text_info, memory)) {
        if (*sig_addr != it->second) {
          XELOGI(
              "DC3: Adjusted manifest target for {} from {:08X} to {:08X} "
              "using local signature hint",
              spec.name, it->second, *sig_addr);
        }
        resolved.resolved_address = *sig_addr;
        resolved.resolve_method =
            (*sig_addr == it->second) ? Dc3PatchResolveMethod::kPatchManifest
                                      : Dc3PatchResolveMethod::kSignatureStub;
        resolved.resolved = true;
        return resolved;
      }
      resolved.resolved_address = it->second;
      resolved.resolve_method = Dc3PatchResolveMethod::kPatchManifest;
      resolved.resolved = true;
      return resolved;
    }
  }

  if ((hybrid_mode || strict_mode) && symbol_manifest) {
    auto it = symbol_manifest->text_symbols.find(spec.name);
    if (it != symbol_manifest->text_symbols.end() && is_viable_target(it->second)) {
      Dc3NuiPatchSpec hinted_spec = spec;
      hinted_spec.address = it->second;
      if (auto sig_addr = Dc3ResolveBySignature(hinted_spec, text_info, memory)) {
        if (*sig_addr != it->second) {
          XELOGI(
              "DC3: Adjusted symbol target for {} from {:08X} to {:08X} "
              "using local signature hint",
              spec.name, it->second, *sig_addr);
        }
        resolved.resolved_address = *sig_addr;
        resolved.resolve_method =
            (*sig_addr == it->second) ? Dc3PatchResolveMethod::kSymbolMap
                                      : Dc3PatchResolveMethod::kSignatureStub;
        resolved.resolved = true;
        return resolved;
      }
      resolved.resolved_address = it->second;
      resolved.resolve_method = Dc3PatchResolveMethod::kSymbolMap;
      resolved.resolved = true;
      return resolved;
    }
  }

  if ((hybrid_mode || strict_mode) && enable_signature_resolver) {
    if (auto sig_addr = Dc3ResolveBySignature(spec, text_info, memory)) {
      resolved.resolved_address = *sig_addr;
      resolved.resolve_method = Dc3PatchResolveMethod::kSignatureStub;
      resolved.resolved = true;
      return resolved;
    }
  }
  if (strict_mode) {
    resolved.strict_rejected = true;
    return resolved;
  }

  if (legacy_mode || hybrid_mode || !strict_mode) {
    resolved.resolved_address = spec.address;
    resolved.resolve_method = Dc3PatchResolveMethod::kCatalogAddress;
    resolved.resolved = true;
  }
  return resolved;
}

}  // namespace dc3
}  // namespace xe
