/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE: the SDK facade.
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_sdk_facade.h"

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui/nui_device.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/memory.h"

DEFINE_bool(nui_hle, true,
            "Kinect (NUI) HLE: emulate the statically linked NUI SDK of a "
            "title whose XEX static-library header names a supported NUI "
            "version (2.0.21173). Titles without NUI are untouched.",
            "Kernel");
DEFINE_string(nui_symbol_map, "",
              "Kinect (NUI) HLE debug cross-check: a symbols.txt-style map "
              "('Name = .text:0xADDR; ...'). Every resolved SDK entry is "
              "compared with the map's address; a mismatch is logged as "
              "TAINTED.",
              "Kernel");

namespace xe {
namespace kernel {
namespace nui {

#include "xenia/kernel/nui/nui_sdk_sigs_2_0_21173.inc"

namespace {

struct SdkVersionTable {
  uint16_t major, minor, build;
  const NuiSdkSignature* sigs;
  size_t count;
};
const SdkVersionTable kTables[] = {
    {2, 0, 21173, kNuiSdk_2_0_21173, std::size(kNuiSdk_2_0_21173)},
};

constexpr size_t kMaxEntries = 64;
struct EntryState {
  const char* name = nullptr;
  uint32_t address = 0;
  std::atomic<uint64_t> hits{0};
};
std::array<EntryState, kMaxEntries> g_entries;

void NoteHit(size_t index) {
  if (g_entries[index].hits.fetch_add(1, std::memory_order_relaxed) == 0) {
    XELOGI("NUI HLE: first call {} ({:08X})", g_entries[index].name,
           g_entries[index].address);
  }
}

uint32_t Arg(cpu::ppc::PPCContext* ctx, int n) {
  return static_cast<uint32_t>(ctx->r[3 + n]);
}
void Return(cpu::ppc::PPCContext* ctx, uint32_t hr) {
  ctx->r[3] = static_cast<uint64_t>(static_cast<int64_t>(
      static_cast<int32_t>(hr)));
}

// --- Device-backed entries -------------------------------------------------

using Handler = uint32_t (*)(cpu::ppc::PPCContext* ctx);

uint32_t DoInitialize(cpu::ppc::PPCContext* ctx) {
  return NuiDevice::Get()->Initialize(Arg(ctx, 0));
}
uint32_t DoShutdown(cpu::ppc::PPCContext*) {
  return NuiDevice::Get()->Shutdown();
}
uint32_t DoTrackingEnable(cpu::ppc::PPCContext* ctx) {
  return NuiDevice::Get()->SkeletonTrackingEnable(Arg(ctx, 0), Arg(ctx, 1));
}
uint32_t DoTrackingDisable(cpu::ppc::PPCContext*) {
  return NuiDevice::Get()->SkeletonTrackingDisable();
}
uint32_t DoSetTracked(cpu::ppc::PPCContext* ctx) {
  return NuiDevice::Get()->SkeletonSetTrackedSkeletons(Arg(ctx, 0));
}
uint32_t DoGetNextFrame(cpu::ppc::PPCContext* ctx) {
  return NuiDevice::Get()->SkeletonGetNextFrame(Arg(ctx, 0), Arg(ctx, 1));
}

// --- Phase 1 "legacy-equivalent" entries -----------------------------------
// The value the DC3 title table returned for each (li r3,0|-1; blr), now
// resolved by SDK version instead of by title address, so a title behaves as
// before while phase 3 gives each its SDK semantics
// (docs/fork/nui/NUI_HLE_DESIGN.md sections 2.4, 3.3.3).
struct Behaviour {
  const char* name;
  Handler handler;    // device-backed, or null
  uint32_t legacy;    // returned when handler is null
};
const Behaviour kBehaviours[] = {
    {"NuiInitialize", DoInitialize, 0},
    {"NuiShutdown", DoShutdown, 0},
    {"NuiSkeletonTrackingEnable", DoTrackingEnable, 0},
    {"NuiSkeletonTrackingDisable", DoTrackingDisable, 0},
    {"NuiSkeletonSetTrackedSkeletons", DoSetTracked, 0},
    {"NuiSkeletonGetNextFrame", DoGetNextFrame, 0},
    {"NuiImageStreamGetNextFrame", nullptr, 0xFFFFFFFF},
    {"NuiAudioCreate", nullptr, 0xFFFFFFFF},
    {"NuiAudioCreatePrivate", nullptr, 0xFFFFFFFF},
    {"NuiFitnessStartTracking", nullptr, 0xFFFFFFFF},
    {"NuiFitnessPauseTracking", nullptr, 0xFFFFFFFF},
    {"NuiFitnessResumeTracking", nullptr, 0xFFFFFFFF},
    {"NuiFitnessStopTracking", nullptr, 0xFFFFFFFF},
    {"NuiFitnessGetCurrentFitnessData", nullptr, 0xFFFFFFFF},
    {"NuiWaveSetEnabled", nullptr, 0xFFFFFFFF},
    {"NuiWaveGetGestureOwnerProgress", nullptr, 0xFFFFFFFF},
    {"NuiSpeechGetEvents", nullptr, 0xFFFFFFFF},
};

const Behaviour* FindBehaviour(std::string_view name) {
  for (const auto& b : kBehaviours) {
    if (name == b.name) {
      return &b;
    }
  }
  return nullptr;
}

std::array<const Behaviour*, kMaxEntries> g_behaviour;

template <size_t I>
void Thunk(cpu::ppc::PPCContext* ctx, KernelState*) {
  NoteHit(I);
  const Behaviour* b = g_behaviour[I];
  if (b && b->handler && NuiDevice::Get()) {
    Return(ctx, b->handler(ctx));
  } else {
    Return(ctx, b ? b->legacy : 0);
  }
}

template <size_t... I>
constexpr std::array<cpu::GuestFunction::ExternHandler, sizeof...(I)>
MakeThunks(std::index_sequence<I...>) {
  return {&Thunk<I>...};
}
const auto kThunks = MakeThunks(std::make_index_sequence<kMaxEntries>());

std::unordered_map<std::string, uint32_t> LoadSymbolMap(
    const std::string& path) {
  std::unordered_map<std::string, uint32_t> map;
  std::ifstream in(path);
  std::string line;
  // symbols.txt: `Name = .text:0x829C2790; // type:function ...`
  static const std::regex re(R"(^\s*(\S+)\s*=\s*\.text:0x([0-9A-Fa-f]+);)");
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, re)) {
      map.emplace(m[1].str(), static_cast<uint32_t>(
                                  std::stoul(m[2].str(), nullptr, 16)));
    }
  }
  return map;
}

}  // namespace

void InstallNuiHle(KernelState* kernel_state, cpu::Processor* processor,
                   UserModule* module) {
  for (auto& e : g_entries) {
    e.name = nullptr;
    e.address = 0;
    e.hits = 0;
  }
  g_behaviour.fill(nullptr);
  auto* xex = module ? module->xex_module() : nullptr;
  if (!xex) {
    return;
  }
  xex2_opt_static_libraries* libs = nullptr;
  if (!xex->GetOptHeader(XEX_HEADER_STATIC_LIBRARIES, &libs) || !libs) {
    XELOGI("NUI HLE: no static-library header; device inert");
    return;
  }
  const xex2_opt_static_library* nui = nullptr;
  const uint32_t lib_count = (libs->size - 4) / 0x10;
  for (uint32_t i = 0; i < lib_count; ++i) {
    if (std::strncmp(libs->libraries[i].name, "NUI", 8) == 0) {
      nui = &libs->libraries[i];
    }
  }
  if (!nui) {
    XELOGI("NUI HLE: the image links no NUI SDK; device inert");
    return;
  }
  const uint16_t major = nui->version_major, minor = nui->version_minor,
                 build = nui->version_build;
  if (!cvars::nui_hle) {
    XELOGI("NUI HLE: NUI {}.{}.{} linked; facade disabled (--nui_hle=false)",
           major, minor, build);
    return;
  }
  const SdkVersionTable* table = nullptr;
  for (const auto& t : kTables) {
    if (t.major == major && t.minor == minor && t.build == build) {
      table = &t;
    }
  }
  if (!table) {
    XELOGW("NUI HLE: unsupported NUI {}.{}.{}; no SDK facade (the sensor is "
           "the kernel/XAM surface only)",
           major, minor, build);
    return;
  }
  auto* text = xex->GetPESection(".text");
  auto* memory = kernel_state->memory();
  if (!text || !text->size) {
    XELOGE("NUI HLE: NUI {}.{}.{} linked but the image has no .text", major,
           minor, build);
    return;
  }
  const uint8_t* text_mem = memory->TranslateVirtual<const uint8_t*>(
      text->address);
  std::vector<uint32_t> resolved(table->count, 0);
  size_t ok = 0;
  for (size_t i = 0; i < table->count; ++i) {
    const auto& sig = table->sigs[i];
    uint32_t addr = 0;
    uint32_t n = FindSignature(text_mem, text->address, text->size, sig, &addr);
    if (n == 1) {
      resolved[i] = addr;
      ++ok;
    } else {
      XELOGW("NUI HLE: {} {} ({} matches)", sig.name,
             n ? "AMBIGUOUS" : "unresolved", n);
    }
  }
  if (ok != table->count || table->count > kMaxEntries) {
    XELOGE("NUI HLE: NUI {}.{}.{} resolved {}/{}; refusing the facade (a "
           "partial SDK HLE would run SDK internals against a runtime "
           "NuiInitialize never set up)",
           major, minor, build, ok, table->count);
    return;
  }
  if (!cvars::nui_symbol_map.empty()) {
    auto map = LoadSymbolMap(cvars::nui_symbol_map);
    size_t agree = 0;
    for (size_t i = 0; i < table->count; ++i) {
      auto it = map.find(table->sigs[i].name);
      if (it == map.end()) {
        XELOGW("NUI HLE: symbol map has no {}", table->sigs[i].name);
      } else if (it->second != resolved[i]) {
        XELOGE("NUI HLE: {} resolved {:08X} but the symbol map says {:08X} "
               "(TAINTED)",
               table->sigs[i].name, resolved[i], it->second);
      } else {
        ++agree;
      }
    }
    XELOGI("NUI HLE: symbol map cross-check: {}/{} agree", agree,
           table->count);
  }
  for (size_t i = 0; i < table->count; ++i) {
    g_entries[i].name = table->sigs[i].name;
    g_entries[i].address = resolved[i];
    g_behaviour[i] = FindBehaviour(table->sigs[i].name);
    processor->RegisterGuestFunctionOverride(
        resolved[i], kThunks[i], std::string(table->sigs[i].name));
  }
  NuiDevice::Create(kernel_state);
  XELOGI("NUI HLE: NUI {}.{}.{} resolved {}/{} (.text {:08X}+{:X}); SDK "
         "facade installed",
         major, minor, build, ok, table->count, text->address, text->size);
}

void ShutdownNuiHle() {
  NuiDevice::Destroy();
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
