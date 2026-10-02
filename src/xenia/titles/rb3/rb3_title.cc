/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) title module (NOT upstream).
 *
 * ApplyRb3LaunchHooks runs from Emulator::CompleteLaunch, before the title's
 * main thread exists. Everything here is gated on the title ID and on its own
 * default-off cvar (rb3_flags.cc). The workarounds below are the ones the
 * documented RB3 recipes need (§8v-§8x of
 * docs/fork/rb3/jit-fault-wiki/WORKSTREAM-rb3-on-xenia-bringup.md, "the
 * WORKSTREAM" below); the diagnostics and harness automation live in their
 * own files.
 ******************************************************************************
 */

#include "xenia/titles/rb3/rb3_title.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/titles/probe_threads.h"
#include "xenia/titles/rb3/rb3_flags.h"
#include "xenia/titles/rb3/rb3_guest.h"
#include "xenia/titles/rb3/rb3_internal.h"
#include "xenia/titles/title_hooks.h"
#include "xenia/titles/title_ids.h"

namespace xe {

namespace {

using titles::TitleLaunchContext;
using titles::rb3::GuestReader;

// Writes `bytes` over guest memory at `address` after making its page
// writable through the guest heap. False (nothing written) if the heap
// refuses.
bool PatchGuestBytes(Memory* memory, uint32_t address, const void* bytes,
                     size_t size) {
  auto* heap = memory->LookupHeap(address);
  if (!heap || !heap->Protect(address, static_cast<uint32_t>(size),
                              kMemoryProtectRead | kMemoryProtectWrite)) {
    return false;
  }
  std::memcpy(memory->virtual_membase() + address, bytes, size);
  return true;
}

// --- --rb3dx_offline_join ----------------------------------------------------
//
// Overrides the guest NetSession::IsHost(). Headless with XNet/XSession/Quazal
// stubbed, a Quazal session object still gets created (mQNet @ this+0x70 != 0)
// but this machine is not the session's "duplication master", so the real
// IsHost() returns false. That drives NetSession::AddLocalUser down its
// NON-host branch -- SetState(kRequestingNewUser=7); deliver an
// AddUserRequestMsg over the dead network -- instead of the host branch
// (AddLocalToSession + fire AddUserResultMsg(1) synchronously). The response
// never arrives, so the overshell slot never reaches allowing-input and
// splash_screen's kSplashScreen_WaitOvershell gate never advances. The RB3
// native port hit the same gate and fixed it by making IsHost() return true
// offline (rb3_netsession_native.cpp:195).
//
// We replicate the real IsHost() using the target's own field reads (mState @
// this+0x68; offsets and state constants from the guest disassembly) and
// diverge ONLY where the real code would consult the Quazal duplication
// master: offline the local machine IS the host. kRequestingNewUser(7) and the
// joining states (3..6) are returned as not-host exactly as the real function
// would.
void IsHostOfflineExtern(cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
  if (!ppc_context || !kernel_state) {
    return;
  }
  uint8_t* base = kernel_state->memory()->virtual_membase();
  uint32_t self = static_cast<uint32_t>(ppc_context->r[3]);  // NetSession*
  uint32_t state = xe::load_and_swap<uint32_t>(base + self + 0x68);
  bool not_host = (state == 7 || (state >= 3 && state <= 6));
  static std::atomic<uint32_t> s_calls{0};
  uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 8) {
    XELOGI(
        "RB3DX offline-join: IsHost hit #{} this=0x{:08X} mState={} "
        "lr=0x{:08X} -> host={}",
        n, self, state, static_cast<uint32_t>(ppc_context->lr),
        not_host ? 0 : 1);
  }
  ppc_context->r[3] = not_host ? 0 : 1;
}

void InstallOfflineJoin(const TitleLaunchContext& ctx) {
  // The running image (retail TU5 or the RB3DX repack) is a different build
  // than the rb3-xenon decomp image, so IsHost() is located by a
  // relocation-independent signature anchored on `lwz r11,0x68(r3)` (mState)
  // and the 7/3/4/5/6 state compares (branch words wildcarded):
  //   [+0x00] lwz   r11, 0x68(r3)   0x81630068
  //   [+0x04] cmpwi cr6, r11, 7     0x2F0B0007
  //   [+0x0C] cmpwi cr6, r11, 3     0x2F0B0003
  //   [+0x14] cmpwi cr6, r11, 4     0x2F0B0004
  //   [+0x1C] cmpwi cr6, r11, 5     0x2F0B0005
  //   [+0x24] cmpwi cr6, r11, 6     0x2F0B0006
  // IsHost entry = anchor - 0xC (mflr r12 / stw r12,-8(r1) / stwu).
  GuestReader reader(ctx.memory);
  uint32_t found = 0;
  for (uint32_t page = 0x82000000; page < 0x83000000 && !found;
       page += 0x1000) {
    if (!reader.Readable(page)) continue;
    for (uint32_t a = page; a + 0x28 <= page + 0x1000; a += 4) {
      if (reader.R32(a) != 0x81630068 || reader.R32(a + 0x04) != 0x2F0B0007 ||
          reader.R32(a + 0x0C) != 0x2F0B0003 ||
          reader.R32(a + 0x14) != 0x2F0B0004 ||
          reader.R32(a + 0x1C) != 0x2F0B0005 ||
          reader.R32(a + 0x24) != 0x2F0B0006) {
        continue;
      }
      uint32_t entry = a - 0xC;
      if (reader.R32(entry) == 0x7D8802A6 &&
          reader.R32(entry + 0x4) == 0x9181FFF8) {
        found = entry;
      }
      break;
    }
  }
  if (!found) {
    XELOGW(
        "RB3DX: offline-join NOT installed (NetSession::IsHost signature not "
        "found in 0x82000000-0x83000000)");
    return;
  }
  ctx.processor->RegisterGuestFunctionOverride(
      found, IsHostOfflineExtern, "RB3DX:NetSession::IsHost(offline)");
  XELOGI(
      "RB3DX: offline single-host join enabled via NetSession::IsHost "
      "override at 0x{:08X}",
      found);
}

// --- --rb3_no_char_preview ---------------------------------------------------
//
// No-op CharSync::UpdateCharCache (0x82564698, void return), exactly like the
// rb3 native port's RB3_NO_CHAR_PREVIEW: the band-member preview char cache
// (world/shared/extras/male_extras0N.milo) is never streamed. Registered here,
// before the JIT compiles App::App's direct `bl 0x82564698` (0x82271490).
void NoCharPreviewExtern(cpu::ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {
  static std::atomic<uint32_t> s_calls{0};
  uint32_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 4 && ppc_context) {
    XELOGI(
        "RB3 no-char-preview: UpdateCharCache hit #{} suppressed "
        "(lr=0x{:08X})",
        n, static_cast<uint32_t>(ppc_context->lr));
  }
}

// --- --rb3_tu5_app_run_direct ------------------------------------------------
//
// See the cvar help: retail App::Run (0x822703D0) reaches the frame loop
// App::RunWithoutDebugging (0x82270080) only through a deliberate null store
// -> unhandled-exception -> filter chain, which --protect_zero=false defuses.
// Patch main's `bl App::Run` to call the frame loop directly (r3 = App* is
// already set by the addi at 0x82272E8C).
//
// Note: this compensates for --protect_zero=false committing guest page 0.
// A core fix that delivers the page-0 store as a guest access violation
// (while still satisfying the separate 0x8275026C page-0 read) would make the
// title's own filter path work and retire this patch.
void InstallAppRunDirect(const TitleLaunchContext& ctx) {
  const uint32_t kMainRunCall = 0x82272E90;
  uint32_t cur = xe::load_and_swap<uint32_t>(ctx.memory->virtual_membase() +
                                             kMainRunCall);
  if (cur == 0x4BFFD1F1u || cur == 0x4280D1F1u) {
    // Already calls 0x82270080 (a pre-patched image, e.g. RB3DX lineage).
    XELOGI("RB3: app-run-direct: image already patched @0x{:08X} (0x{:08X})",
           kMainRunCall, cur);
    return;
  }
  if (cur != 0x4BFFD541u) {  // bl 0x822703D0 (pristine clean TU5)
    XELOGW(
        "RB3: app-run-direct NOT installed (0x{:08X} is 0x{:08X}, not the "
        "App::Run bl)",
        kMainRunCall, cur);
    return;
  }
  uint8_t word[4];
  xe::store_and_swap<uint32_t>(word, 0x4BFFD1F1u);  // bl 0x82270080
  if (!PatchGuestBytes(ctx.memory, kMainRunCall, word, sizeof(word))) {
    XELOGW("RB3: app-run-direct NOT installed (Protect failed @0x{:08X})",
           kMainRunCall);
    return;
  }
  XELOGI(
      "RB3: app-run-direct installed -- main's `bl App::Run(0x822703D0)` "
      "@0x{:08X} -> `bl App::RunWithoutDebugging(0x82270080)`",
      kMainRunCall);
}

// --- --rb3_mogg_key_table ----------------------------------------------------
void InstallMoggKeyTable(const TitleLaunchContext& ctx) {
  const uint32_t kMoggKeyTable = 0x82C76258;
  std::vector<uint8_t> bytes;
  int hi = -1;
  bool bad = false;
  for (char c : cvars::rb3_mogg_key_table) {
    if (c == ' ' || c == '\t' || c == ',' || c == '\n') continue;
    int v;
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else {
      bad = true;
      break;
    }
    if (hi < 0) {
      hi = v;
    } else {
      bytes.push_back(static_cast<uint8_t>((hi << 4) | v));
      hi = -1;
    }
  }
  if (bad || hi >= 0 || bytes.size() != 64) {
    XELOGW(
        "RB3: mogg-key-table NOT installed (need exactly 64 hex bytes, "
        "parsed {})",
        bytes.size());
  } else if (PatchGuestBytes(ctx.memory, kMoggKeyTable, bytes.data(), 64)) {
    XELOGI("RB3: mogg-key-table installed -- 64 bytes @0x{:08X}",
           kMoggKeyTable);
  } else {
    XELOGW("RB3: mogg-key-table NOT installed (Protect failed @0x{:08X})",
           kMoggKeyTable);
  }
}

// --- --rb3dx_skip_calibration ------------------------------------------------
//
// The splash flow, at kSplashScreen_EndOvershell, evaluates
// `{! {profile_mgr get_has_seen_first_time_calibration}}` (ui/splash/
// splash.dta): false on a fresh profile routes to `push_screen
// first_time_calibration` (the interactive cal_audio_screen, uncompletable
// headless with null audio); true routes to `goto_screen main_hub_screen`.
// ProfileMgr::Handle reads mHasSeenFirstTimeCalibration INLINE (the
// standalone getter is never entered at boot), so a guest-function override
// cannot intercept it; we set the backing byte instead.
//
// The ObjectDir stores the Hmx::Object VBASE pointer (object tail) for the
// MsgSource-derived ProfileMgr, so the member (this+0x54) sits at a NEGATIVE
// offset from that pointer. Empirically pinned: on first_time_calibration
// enter the setter flips two adjacent bytes together at obj-0x90 and obj-0x74
// (spacing 0x1C == 0x54-0x38 = flag vs mGlobalOptionsDirty), fixing
// this = obj-0xC8, so the flag byte is obj-0x74 (cross-checked by obj-0x60 ==
// this+0x68 == mOverscan).
//
// The byte is re-asserted until the UI reaches main_hub_screen (the GlobalOptions
// load can re-zero it before the splash decision), then the thread exits.
void SkipCalibrationThread(Memory* memory) {
  const int32_t kFlagOffset = -0x74;
  GuestReader reader(memory);
  uint8_t* base = memory->virtual_membase();
  auto writable = [&](uint32_t addr) {
    auto* heap = memory->LookupHeap(addr);
    uint32_t prot = 0;
    return heap && heap->QueryProtect(addr, &prot) &&
           (prot & kMemoryProtectWrite) != 0;
  };
  uint32_t profile_mgr = 0;
  uint32_t pokes = 0;
  bool warned = false;
  do {
    if (!profile_mgr) {
      profile_mgr = reader.FindMainDirObject("profile_mgr");
      if (profile_mgr) {
        XELOGI(
            "RB3DX skip-calibration: resolved profile_mgr obj=0x{:08X} (flag "
            "byte @0x{:08X})",
            profile_mgr, profile_mgr + kFlagOffset);
      }
    }
    if (profile_mgr) {
      uint32_t flag_addr = profile_mgr + kFlagOffset;
      if (writable(flag_addr) && base[flag_addr] != 1) {
        uint8_t before = base[flag_addr];
        base[flag_addr] = 1;
        if (pokes < 8) {
          XELOGI("RB3DX skip-calibration: poke #{} [0x{:08X}] {} -> 1", pokes,
                 flag_addr, before);
        }
        ++pokes;
      }
    }
    std::string screen = titles::rb3::ReadCurrentScreenName(reader);
    if (screen == "main_hub_screen") {
      XELOGI(
          "RB3DX skip-calibration: main_hub_screen reached after {} poke(s); "
          "done",
          pokes);
      return;
    }
    if (!warned && (screen == "first_time_calibration" ||
                    screen == "cal_audio_screen")) {
      warned = true;
      XELOGW("RB3DX skip-calibration: '{}' entered despite the flag", screen);
    }
  } while (titles::ProbeSleep(150));
}

}  // namespace

void ApplyRb3LaunchHooks(const TitleLaunchContext& ctx) {
  if (!ctx.title_id.has_value() || ctx.title_id.value() != titles::kTitleRb3) {
    return;
  }
  // Push --rb3dx_alloc_probe down into the MMIO fault handler (the cpu library
  // must not depend on a title cvar).
  cpu::MMIOHandler::SetAllocProbeEnabled(cvars::rb3dx_alloc_probe);

  titles::rb3::InstallSaveGprLr23Hook(ctx);

  if (cvars::rb3_no_char_preview) {
    const uint32_t kUpdateCharCache = 0x82564698;
    ctx.processor->RegisterGuestFunctionOverride(
        kUpdateCharCache, NoCharPreviewExtern,
        "RB3:CharSync::UpdateCharCache(no-op)");
    XELOGI("RB3: UpdateCharCache no-op override installed at 0x{:08X}",
           kUpdateCharCache);
  }
  if (cvars::rb3_tu5_app_run_direct) {
    InstallAppRunDirect(ctx);
  }
  if (!cvars::rb3_mogg_key_table.empty()) {
    InstallMoggKeyTable(ctx);
  }
  titles::rb3::InstallAllocTrace(ctx);
  titles::rb3::StartUiProbe(ctx);
  titles::rb3::StartAutopilot(ctx);
  if (cvars::rb3dx_offline_join) {
    InstallOfflineJoin(ctx);
  }
  if (cvars::rb3dx_skip_calibration) {
    Memory* memory = ctx.memory;
    titles::SpawnProbeThread([memory]() { SkipCalibrationThread(memory); });
    XELOGI(
        "RB3DX: first-boot calibration skip thread started "
        "(--rb3dx_skip_calibration)");
  }
  titles::rb3::InstallSiHarness(ctx);
}

namespace {

// Note (from the retired --rb3_mount_update experiment): do NOT symlink
// "update:" to the game mount. RB3 probes update:\gen\patch_xbox.hdr for
// title-update content; pointing update: at the disc dir exposes the bundled
// patch_xbox.* which, in this content set, is the RB3DX "LOLZ" header that
// retail rejects with a "dirty disc" bail-out. A real update: mount belongs
// to a separate, matching title-update package.

void Rb3OnTerminateTitle() {
  // Stop and join the probe threads before the title (and later memory_) goes
  // away under them (fork-cleanup-review.md C10).
  titles::JoinProbeThreads();
}

void Rb3OnShutdown() { titles::JoinProbeThreads(); }

}  // namespace

void RegisterRb3TitleHooks() {
  titles::TitleHooks hooks;
  hooks.name = "rb3";
  hooks.apply_launch_hooks = ApplyRb3LaunchHooks;
  hooks.on_terminate_title = Rb3OnTerminateTitle;
  hooks.on_shutdown = Rb3OnShutdown;
  titles::RegisterTitleHooks(hooks);
}

}  // namespace xe
