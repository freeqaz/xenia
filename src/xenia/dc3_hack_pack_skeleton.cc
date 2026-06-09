#include "xenia/dc3_hack_pack.h"

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"

DECLARE_bool(fake_kinect_data);

namespace xe {
namespace {

// GotoFirstScreen host override (ExternHandler is a plain fn ptr, no
// captures).  The game's App-boot call to UIManager::GotoFirstScreen happens
// before ui.dta has finished populating/scoping the screen graph; the real
// implementation (and any by-name GotoScreen we might substitute) then derefs
// incomplete .dta data and crashes -- empirically in DataNode::GetObj
// (0x8259E608) on the DataVariable path AND in DataArray::FindArray
// (0x825A0B80) on the by-name path.  So we DON'T do the work eagerly; instead
// we GATE it: only once ObjectDir::Main (sMainDir, *0x82F63B28) is non-null
// AND ObjectDir::FindObject("attract_screen") returns a real object do we call
// the by-NAME UIManager::GotoScreen (0x8277B378) to actually navigate.  Until
// then this is a safe no-op.  The game calls GotoFirstScreen only once, so the
// host NUI-poll nav bridge in emulator.cc re-invokes this override every
// interval while cur_screen is still NULL -- so as soon as the screen graph
// finishes loading, the very next re-invocation navigates with no race.
//   sMainDir              *0x82F63B28  (ObjectDir::Main())
//   ObjectDir::FindObject  0x82595960  (this, name, recurse, fail)
//   UIManager::GotoScreen  0x8277B378  (this, const char* name, b1, b2)
uint32_t g_attract_name_buf = 0;  // guest "attract_screen" string
constexpr uint32_t kSMainDirPtr = 0x82F63B28;
constexpr uint32_t kObjectDirFindObject = 0x82595960;
constexpr uint32_t kUIManagerGotoScreenByName = 0x8277B378;

void Dc3GotoFirstScreenExtern(cpu::ppc::PPCContext* ppc_context,
                              kernel::KernelState* kernel_state) {
  if (!g_attract_name_buf || !kernel_state) {
    return;
  }
  auto* processor = kernel_state->processor();
  auto* memory = kernel_state->memory();
  auto* thread_state = ppc_context->thread_state;
  uint32_t this_ptr = static_cast<uint32_t>(ppc_context->r[3]);  // UIManager*
  if (!processor || !memory || !thread_state || !this_ptr) {
    return;
  }
  // Read ObjectDir::Main() == sMainDir.
  static uint32_t s_call_count = 0;
  ++s_call_count;
  auto* main_ptr = memory->TranslateVirtual<uint8_t*>(kSMainDirPtr);
  uint32_t main_dir = main_ptr ? xe::load_and_swap<uint32_t>(main_ptr) : 0;
  if (!main_dir || main_dir >= 0xF0000000) {
    if ((s_call_count % 60) == 1) {
      XELOGI("DC3:GotoFirstScreen gate: sMainDir not ready (={:08X}) call#{}",
             main_dir, s_call_count);
    }
    return;  // ObjectDir not created yet -> nothing to navigate to.
  }
  // ObjectDir::FindObject(this, const char* name, bool parentDirs,
  //                       bool subDirs)  -> UIScreen* or 0 if not loaded.
  // Find<UIScreen> uses (name, parentDirs=false, subDirs=true); the screens
  // live in the "ui" sub-dir (TheUI is named "ui" under Main), so subDirs MUST
  // be true (4th arg = 1) to recurse into it.
  uint64_t find_args[4] = {main_dir, g_attract_name_buf, 0, 1};
  uint64_t found = processor->Execute(thread_state, kObjectDirFindObject,
                                      find_args, 4);
  uint32_t scr_obj = static_cast<uint32_t>(found);
  if (!scr_obj || scr_obj >= 0xF0000000) {
    if ((s_call_count % 60) == 1) {
      XELOGI("DC3:GotoFirstScreen gate: mainDir={:08X} but FindObject("
             "attract_screen)={:08X} (screen graph not loaded) call#{}",
             main_dir, scr_obj, s_call_count);
    }
    return;  // screen graph not loaded yet -> safe no-op, retry later.
  }
  // Screen exists: do the real navigation by name.
  XELOGI("DC3:GotoFirstScreen gate: navigating -> attract_screen obj={:08X} "
         "(mainDir={:08X}) call#{}",
         scr_obj, main_dir, s_call_count);
  uint64_t goto_args[4] = {this_ptr, g_attract_name_buf, 0, 0};
  processor->Execute(thread_state, kUIManagerGotoScreenByName, goto_args, 4);
}

}  // namespace

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
      // Debug::Fail (0x825CE1D0) non-main-thread path: the original Xbox build
      // parks any failing worker thread in an infinite spin
      //   while (true) { Timer::Sleep(200); PlatformDebugBreak(); }   (@0x825CE2D0)
      // so a real devkit debugger can attach.  Under headless Xenia nothing
      // attaches, so the SkeletonUpdate worker (thread start 0x8242E6A8) hits a
      // benign assert inside Update() after attract->title teardown, parks in
      // this spin forever, stops calling NuiSkeletonGetNextFrame, and freezes
      // the whole NUI poll (s_skel_calls stuck -> nav bridge never advances ->
      // all-black).  Patch the spin's loop-back branch (b -0xC @0x825CE2DC) to
      // jump to the function epilogue (b +0x90 -> 0x825CE36C) so the worker
      // returns and continues polling after one assert -- matching the native
      // port's "FAIL is non-fatal, continue" semantics (Debug.cpp HX_NATIVE).
      {0x825CE2DC, 0x48000090,
       "Debug::Fail: thread-fail spin -> return (worker survives assert)"},
      // NOTE: tried `blr` at Debug::Fail entry (0x825CE1D0) to make FAIL
      // non-fatal (match native) and limp past the preview.tmov fatal — it
      // REGRESSES (rc=139 early): blr skips the `if(mTry) throw msg` path that
      // MILO_TRY/MILO_CATCH blocks depend on, so code continues past a guarded
      // failure into worse state -> earlier SIGSEGV. A surgical fix must skip
      // only the Modal(kModalFail) halt for the SPECIFIC main-thread non-TRY
      // fatal, or resolve preview.tmov itself. See task #21.
      // (REMOVED 2026-06-02) SongAnimByDifficulty->null survival patch. It was a
      // crash-era diagnostic for when mSongAnims (HamDirector+0x5c) RB-tree nodes
      // were dangling (operator[] crash during the async-load stall). That stall
      // is now fixed (hackpack) and mSongAnims is HEALTHY (proven: diff 0/1/2 each
      // resolve to a valid RndPropAnim with an intact 5-node mPropKeys list).
      // Worse, this null-stub COLLIDED with the HamDirector::SongAnim "force expert
      // anim" redirect (emulator.cc ~3816): that redirect branched to 0x82473e5c
      // (this stub's `blr`, skipping `li r3,0`), so SongAnim returned r3 unchanged
      // == TheHamDirector -> ClipPlayer::Init called RndPropAnim::GetKeys with
      // this==HamDirector -> infinite GetKeys hang (3.3M SIGSEGV, ~36s, present
      // freeze). Fix = remove this stub + branch the SongAnim redirect to the real
      // entry 0x82473e58. SongAnimByDifficulty now runs `return mSongAnims[diff]`
      // on the healthy map -> a REAL expert anim with clip keyframes (animating).
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

  // Attract->title boot gate (headless original layout): the attract movie's
  // async file read never completes, so BinkMovieImpl::Ready() (retail/debug.xex
  // VA 0x82E221C8) returns false forever. That keeps MoviePanel::IsLoaded ->
  // UIScreen::CheckIsLoaded false, so the attract_screen->title_screen
  // transition never fires and TheUI->Draw() emits zero draws (all-black).
  // Force Ready()=true so the load-gate clears and the proper UIScreen::Enter
  // path runs (game-driven UIManager::Update, and emulator.cc's force-ENTER
  // fallback which is gated on CheckIsLoaded==true).
  if (ctx.processor) {
    const uint32_t kBinkMovieImplReady = 0x82E221C8;
    ctx.processor->RegisterGuestFunctionOverride(
        kBinkMovieImplReady,
        [](cpu::ppc::PPCContext* ppc_context,
           kernel::KernelState* kernel_state) {
          ppc_context->r[3] = 1;  // BinkMovieImpl::Ready() -> true
        },
        "DC3:BinkMovieImpl::Ready");
    XELOGI("DC3: Registered BinkMovieImpl::Ready=true override at {:08X}",
           kBinkMovieImplReady);
    result.applied++;

    // Blocker 1 (boot non-determinism): the attract Bink movie's framebuffer
    // setup (BinkRegisterFrameBuffers/BinkGetFrameBuffersInfo @0x82EE8C30 /
    // 0x82EE8B58) is INTERMITTENT under headless Xenia -- on most boots the
    // decode pump never fires, so MoviePanel::IsLoaded() -> false forever
    // (mMovie.Ready() false AND/OR subtitles never load), the attract screen
    // never instantiates, the NUI sequencer stalls, and cur_screen stays 0.
    // The BinkMovieImpl::Ready=true override above is NOT sufficient because
    // MoviePanel::IsLoaded also gates on the subtitles loader and the panel's
    // own UIPanel::IsLoaded.  Override at the PANEL level so the screen graph
    // instantiates deterministically regardless of the Bink decode race.
    //   ?IsLoaded@MoviePanel@@UBA_NXZ  guest VA 0x82E0EFE8 (verified in
    //   ham_xbox_r.map -> meta:MoviePanel.obj)
    const uint32_t kMoviePanelIsLoaded = 0x82E0EFE8;
    ctx.processor->RegisterGuestFunctionOverride(
        kMoviePanelIsLoaded,
        [](cpu::ppc::PPCContext* ppc_context,
           kernel::KernelState* kernel_state) {
          ppc_context->r[3] = 1;  // MoviePanel::IsLoaded() -> true
        },
        "DC3:MoviePanel::IsLoaded");
    XELOGI("DC3: Registered MoviePanel::IsLoaded=true override at {:08X}",
           kMoviePanelIsLoaded);
    result.applied++;

    // Blocker 1 (true root cause): the App boot sequence calls
    // UIManager::GotoFirstScreen() (?GotoFirstScreen@UIManager@@QAAXXZ, guest
    // VA 0x8277B140) which, per Ghidra, is:
    //   GotoScreen(DataVariable("first_screen").Obj<UIScreen>(), false, false)
    // ui.dta does `{set $first_screen attract_screen}`, so first_screen
    // resolves to the attract_screen UIScreen object via
    // DataNode::GetObj -> gDataDir->FindObject("attract_screen").  Under
    // headless Xenia this is a BOOT-ORDERING RACE: on ~5/6 boots the App boot
    // thread reaches GotoFirstScreen before ui.dta has finished
    // populating/scoping the screen graph, so the DataNode read inside
    // DataNode::GetObj (0x8259E608) dereferences guest NULL and the app/game
    // thread takes a host SIGSEGV (last_fault=0x100000000, crash_guest=
    // 0x8259E608, return addr 0x8277B174 = GotoFirstScreen+0x34).  That kills
    // the game thread, the NUI poll freezes after ~60 calls, and cur_screen
    // stays 0 to timeout.
    //
    // We initially tried substituting the by-NAME GotoScreen unconditionally,
    // but that just relocates the same race into DataArray::FindArray
    // (0x825A0B80).  The override (Dc3GotoFirstScreenExtern, above) now GATES
    // the navigation on ObjectDir::FindObject("attract_screen") returning a
    // real object, so it's a safe no-op until ui.dta has loaded and a true
    // navigation afterwards.  The host NUI-poll nav bridge re-invokes this
    // override while cur_screen is NULL, so the first post-load invocation
    // navigates with no race.
    g_attract_name_buf = memory->SystemHeapAlloc(0x20, 0x10);
    if (g_attract_name_buf) {
      auto* nm = memory->TranslateVirtual<char*>(g_attract_name_buf);
      if (nm) {
        std::strcpy(nm, "attract_screen");
      }
    }
    const uint32_t kUIManagerGotoFirstScreen = 0x8277B140;
    ctx.processor->RegisterGuestFunctionOverride(
        kUIManagerGotoFirstScreen, Dc3GotoFirstScreenExtern,
        "DC3:UIManager::GotoFirstScreen(gated host-driven)");
    XELOGI("DC3: Registered GotoFirstScreen gated override at {:08X} "
           "(navigates to attract_screen once FindObject resolves; "
           "name_buf={:08X})",
           kUIManagerGotoFirstScreen, g_attract_name_buf);
    result.applied++;
  } else {
    result.skipped++;
  }

  return result;
}

}  // namespace xe
