#include "xenia/titles/dc3/dc3_hack_pack.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/titles/dc3/dc3_hacks.h"

DECLARE_bool(fake_kinect_data);

namespace xe {

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

  // (The PPC constant-frame stub that used to be written over
  // NuiSkeletonGetNextFrame 0x829C2790 here is gone: the nui.get_next_frame
  // override at the same address shadowed it -- the override audit measured
  // handler hits on every frame and the guest body never resolved.)

  struct BinaryPatch {
    const char* id;
    uint32_t address;
    uint32_t value;
    const char* name;
  };
  BinaryPatch skel_patches[] = {
      {"skel.wait_33ms", 0x8242E74C, 0x3B800021,
       "SkeletonUpdateThread: timeout INFINITE -> 33ms"},
      {"skel.is_override_nop", 0x8242E1B0, 0x60000000,
       "SkeletonUpdate::Update: NOP IsOverride branch"},
      // (REMOVED 2026-10-02) Debug::Fail thread-fail spin -> return
      // (0x825CE2DC <- b +0x90). It made a failing worker return instead of
      // parking in the devkit "wait for debugger" spin, but skipped
      // MemPopHeap and `mFailing = 0`, so mFailing stayed latched and every
      // later MILO_FAIL was silent (BASELINE). The worker fail it survived
      // ("BinkMovieImpl::Ready called in the wrong thread (expected 6, cur
      // 15)") came from host automation calling UI code on the SkeletonUpdate
      // worker; that now runs on the main thread (dc3_autonav.cc). The spin
      // is faithful again; dc3_fail_tripwire.cc reports any worker fail.
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
    if (!dc3::HackGate(p.id, p.name)) {
      result.skipped++;
      continue;
    }
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


  // (RETIRED 2026-10-02) BinkMovieImpl::Ready -> 1, MoviePanel::IsLoaded -> 1
  // and the host-gated UIManager::GotoFirstScreen override. The two virtual
  // overrides were INERT until the cpu override fix (reached only through
  // the vtable); all three covered the Bink/lost-resume chain, and S1 runs
  // the real functions now (docs/fork/dc3/BASELINE.md).

  return result;
}

}  // namespace xe
