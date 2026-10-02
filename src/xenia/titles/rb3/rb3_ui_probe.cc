/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914): --rb3dx_ui_probe, a read-only sampler of the
 * guest UI / loader / session state (NOT upstream).
 *
 * Reads guest memory from a host thread every ~2 s; writes nothing and
 * installs no hooks. Its `RB3DX UI PROBE[n]: transState=...` and
 * `STREAM-CENSUS` lines are a harness contract (tools/fork-regress/analyze/
 * rb3_flow.py).
 *
 * Anchors (TU5/RB3DX-runtime-proven by RB3Enhanced's ports_xbox360.h):
 *   TheBandUI (BandUI instance)      = 0x82DFD2B0
 *   ObjectDir::sMainDir (ObjectDir*) = 0x82E054B8
 * Layouts (rb3-xenon decomp, Ghidra-verified for RB3-360 where noted):
 *   UIManager (virtual Hmx::Object base; vftable@0, vbtbl@8, vbase tail):
 *     mTransitionState @+0x10, mCurrentScreen @+0x2C, mTransitionScreen @+0x30.
 *   UIScreen (NON-virtual Hmx::Object base): name char* @+0x18, mPanelList
 *     std::list<PanelRef> = embedded {next,prev} dummy @+0x28; node =
 *     {next@0, prev@4, PanelRef@8 = {UIPanel* @+8, mActive @+0xC,
 *     mAlwaysLoad @+0xD, mLoaded @+0xE}}.
 *   UIPanel (VIRTUAL Hmx::Object base; vfptr@0, vbptr@4): mDir @+0x8,
 *     mLoader @+0xC, mLoaded @+0x1C, mState @+0x20 (0=kUnloaded 1=kUp
 *     2=kDown), mLoadRefs @+0x28. Name via vbase: vbp=[this+4],
 *     vbase=this+4+[vbp+4], name=[vbase+0x18].
 *   TheLoadMgr.mLoading: embedded list dummy @0x82E06E38, nodes
 *     {next, prev, Loader*}.
 *   SaveLoadManager ("saveload_mgr"), NetSync ("net_sync"), NetSession
 *     ("session": mState @head+0x68, mQNet @head+0x70), OvershellPanel
 *     ("overshell", vbase = head+0x4D4).
 ******************************************************************************
 */

#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/titles/probe_threads.h"
#include "xenia/titles/rb3/rb3_flags.h"
#include "xenia/titles/rb3/rb3_guest.h"
#include "xenia/titles/rb3/rb3_internal.h"
#include "xenia/titles/title_hooks.h"

namespace xe {
namespace titles {
namespace rb3 {

namespace {

constexpr uint32_t kLoadMgrLoading = 0x82E06E38;
constexpr uint32_t kStandardStreamVt = 0x820F6A8C;
constexpr uint32_t kLastXInputState = 0x82CCB8D8;  // + pad * 0x10

class UiProbe {
 public:
  UiProbe(Memory* memory, kernel::KernelState* kernel_state,
          cpu::Processor* processor)
      : reader_(memory, /*trust_image_windows=*/cvars::si_load_dll),
        kernel_state_(kernel_state),
        processor_(processor) {}

  // The full sample runs every 2 s; between samples the UI state line is
  // re-read every 250 ms and logged again whenever it changes, so a screen
  // that is current for less than one sample period (the menu autopilot can
  // advance through one in under 2 s) still appears in the log.
  void Run() {
    for (int tick = 1; ProbeSleep(250); ++tick) {
      if (tick % 8 == 0) {
        ++sample_;
        Sample();
      } else if (sample_ > 0) {
        UiStateLine(/*only_if_changed=*/true);
      }
    }
  }

 private:
  uint32_t r32(uint32_t a) const { return reader_.R32(a); }
  uint32_t r8(uint32_t a) const { return reader_.R8(a); }
  bool readable(uint32_t a) const { return reader_.Readable(a); }
  // The probe's string rendering, unchanged since the lines became a harness
  // contract: "<unreadable>", "<binary>", or the text ("..." if truncated).
  std::string rstr(uint32_t a) const {
    if (!readable(a)) return "<unreadable>";
    std::string s;
    for (int i = 0; i < 48; ++i) {
      if (!readable(a + i)) break;
      char c = static_cast<char>(reader_.membase()[a + i]);
      if (!c) return s;
      if (c < 0x20 || c > 0x7E) return "<binary>";
      s += c;
    }
    return s + "...";
  }
  std::string BackChain(uint32_t sp, int depth) const {
    std::string chain;
    for (int i = 0; i < depth && sp; ++i) {
      uint32_t bc = r32(sp);
      if (bc <= sp || bc - sp > 0x100000) break;
      chain += fmt::format(" {:08X}", r32(bc - 8));
      sp = bc;
    }
    return chain;
  }

  void Sample();
  // The `RB3DX UI PROBE[n]: transState=... curScreen=...'name'
  // transScreen=...'name'` line (a harness contract).
  void UiStateLine(bool only_if_changed);
  void ThreadCensus();
  void GuestThreads();
  void LoadQueue();
  void StreamCensus();
  void SiClaims();
  void Panels(uint32_t screen, const char* tag);
  void MainDirObjects(uint32_t cur);
  void ObjectWindows();
  void Joypads();
  void Overshell();

  GuestReader reader_;
  kernel::KernelState* kernel_state_;
  cpu::Processor* processor_;
  int sample_ = 0;
  std::string last_ui_state_;
  uint32_t saveload_obj_ = 0, netsync_obj_ = 0, session_obj_ = 0,
           overshell_obj_ = 0;
  bool dumped_names_ = false;
  uint32_t overshell_slots_vec_ = 0;
};

void UiProbe::Sample() {
  // Every live guest thread with its entry point and a saved-LR backchain,
  // for a few samples early in boot (this is what located the joypad reader
  // thread and, in s64, showed main never entering the frame loop, §8v).
  if (sample_ >= 3 && sample_ <= 9 && kernel_state_) {
    ThreadCensus();
  }
  UiStateLine(/*only_if_changed=*/false);
  uint32_t cur = r32(kTheBandUI + kUiCurrentScreen);
  uint32_t trans = r32(kTheBandUI + kUiTransitionScreen);
  if (processor_) {
    GuestThreads();
  }
  LoadQueue();
  if (cvars::rb3_stream_census) {
    StreamCensus();
  }
  if (cvars::rb3dx_si_claim_anchor != 0) {
    SiClaims();
  }
  Panels(trans, "T:");
  if (cur != trans) {
    Panels(cur, "C:");
  }
  MainDirObjects(cur);
  ObjectWindows();
  Joypads();
  Overshell();
}

void UiProbe::UiStateLine(bool only_if_changed) {
  uint32_t ts = ReadTransitionState(reader_);
  uint32_t cur = r32(kTheBandUI + kUiCurrentScreen);
  uint32_t trans = r32(kTheBandUI + kUiTransitionScreen);
  std::string cur_name = cur ? rstr(r32(cur + kScreenName)) : "<null>";
  std::string trans_name = trans ? rstr(r32(trans + kScreenName)) : "<null>";
  std::string line = fmt::format(
      "transState={} curScreen=0x{:08X}'{}' transScreen=0x{:08X}'{}'", ts, cur,
      cur_name, trans, trans_name);
  if (only_if_changed && line == last_ui_state_) {
    return;
  }
  last_ui_state_ = line;
  XELOGI("RB3DX UI PROBE[{}]: {}", sample_, line);
}

void UiProbe::ThreadCensus() {
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto& t : threads) {
    if (!t) continue;
    std::string chain;
    if (t->thread_state() && t->thread_state()->context()) {
      auto* c = t->thread_state()->context();
      chain = fmt::format(" lr={:08X} bc:", static_cast<uint32_t>(c->lr)) +
              BackChain(static_cast<uint32_t>(c->r[1]), 10);
    }
    XELOGI("RB3DX UI PROBE: THREAD tid={} start=0x{:08X} susp={}{}",
           t->thread_id(), t->creation_params()->start_address,
           t->suspend_count(), chain);
  }
}

void UiProbe::GuestThreads() {
  // Per-tid state (tids 6..31). The plural QueryThreadDebugInfos() filters
  // out kExited/kZombie threads, so query by id (entries persist post-exit):
  // 0=Alive 1=Waiting 2=Exited 3=Zombie.
  std::string census;
  for (uint32_t tid = 6; tid <= 31; ++tid) {
    if (auto* ti = processor_->QueryThreadDebugInfo(tid)) {
      census += fmt::format(" {}:{}", tid, static_cast<int>(ti->state));
    }
  }
  XELOGI("RB3DX UI PROBE[{}]: TIDCENSUS{}", sample_, census);
  // Main thread (tid 6) every sample, every live guest thread every 5th.
  // Unsynchronised reads of running threads' contexts: single samples may
  // tear; consistent repetition across samples is the signal. kWaiting is
  // accepted too: a busy-spin that dips into a guest wait can stay marked
  // kWaiting (OnThreadLeavingWait is not always paired).
  bool full_sweep = (sample_ % 5) == 1;
  for (auto* info : processor_->QueryThreadDebugInfos()) {
    if (!info || !info->thread ||
        (info->state != cpu::ThreadDebugInfo::State::kAlive &&
         info->state != cpu::ThreadDebugInfo::State::kWaiting)) {
      continue;
    }
    uint32_t tid = info->thread_id;
    if (!full_sweep && tid != 6) continue;
    auto* tstate = info->thread->thread_state();
    auto* ctx = tstate ? tstate->context() : nullptr;
    if (!ctx) continue;
    uint32_t sp = static_cast<uint32_t>(ctx->r[1]);
    XELOGI(
        "RB3DX UI PROBE[{}]:   thr{} st={} lr={:08X} sp={:08X} r3={:08X} "
        "r4={:08X} r5={:08X} chain:{}",
        sample_, tid, static_cast<int>(info->state),
        static_cast<uint32_t>(ctx->lr), sp, static_cast<uint32_t>(ctx->r[3]),
        static_cast<uint32_t>(ctx->r[4]), static_cast<uint32_t>(ctx->r[5]),
        BackChain(sp, 16));
  }
}

void UiProbe::LoadQueue() {
  // Walk TheLoadMgr.mLoading and dump each queued loader's header, any string
  // its early fields point at (names the file), and its vtable head.
  uint32_t node = r32(kLoadMgrLoading);
  for (int n = 0; n < 4 && node && node != kLoadMgrLoading; ++n) {
    uint32_t ldr = r32(node + 8);
    std::string hdr, strs;
    for (uint32_t d = 0; d < 0x80; d += 4) {
      uint32_t v = r32(ldr + d);
      hdr += fmt::format(" {:08X}", v);
      if (v >= 0x1000 &&
          (v < 0x50000000 || (v >= 0x82000000 && v < 0x83000000))) {
        std::string s = rstr(v);
        if (s.size() >= 3 && s.size() < 47 && s != "<binary>") {
          strs += fmt::format(" +0x{:X}->'{}'", d, s);
        }
      }
    }
    uint32_t vt = r32(ldr);
    std::string vts;
    for (uint32_t s = 0; s < 0x18; s += 4) {
      vts += fmt::format(" {:08X}", r32(vt + s));
    }
    XELOGI(
        "RB3DX UI PROBE[{}]:   loadq[{}] node={:08X} ldr={:08X}:{}{} "
        "vt[{:08X}]:{}",
        sample_, n, node, ldr, hdr, strs, vt, vts);
    node = r32(node);
  }
}

void UiProbe::StreamCensus() {
  // See the --rb3_stream_census help text. Page-at-a-time: one readability
  // query per page, then a raw sweep.
  uint8_t* base = reader_.membase();
  int found = 0;
  for (uint32_t page = 0x40000000u; page < 0x50000000u && found < 24;
       page += 0x1000u) {
    if (!readable(page)) continue;
    for (uint32_t a = page; a < page + 0x1000u && found < 24; a += 4) {
      if (xe::load_and_swap<uint32_t>(base + a) != kStandardStreamVt) continue;
      uint32_t rb = r32(a + 0x20), re = r32(a + 0x24);
      uint32_t cb = r32(a + 0x78), ce = r32(a + 0x7C);
      XELOGI(
          "RB3DX UI PROBE[{}]: STREAM-CENSUS 0x{:08X} mState={} "
          "recv=0x{:08X}..0x{:08X}(n={}) chans=0x{:08X}..0x{:08X}(n={})",
          sample_, a, r32(a + 0x14), rb, re, re > rb ? (re - rb) / 4 : 0, cb,
          ce, ce > cb ? (ce - cb) / 4 : 0);
      ++found;
    }
  }
  XELOGI("RB3DX UI PROBE[{}]: STREAM-CENSUS total={}", sample_, found);
}

void UiProbe::SiClaims() {
  // The DLL's own gClaims/gImpls: the ground truth for "two players on one
  // track".
  uint32_t anchor = static_cast<uint32_t>(cvars::rb3dx_si_claim_anchor);
  std::string claims;
  for (uint32_t i = 0; i < 3; ++i) {
    claims += fmt::format(" claim{}={{track={},cnt={}}}", i,
                          static_cast<int32_t>(r32(anchor + i * 8)),
                          r32(anchor + i * 8 + 4));
  }
  XELOGI("RB3DX UI PROBE[{}]:   SI claims: claimCount={} implCount={}{}",
         sample_, r32(anchor + 0x1C8), r32(anchor + 0x1CC), claims);
  // Band/pad snapshot: TheBandUserMgr slot-map guids (mgr+0x50 stride 0x10,
  // empty == all-zero), the participants vector (mgr+0x28/+0x2C of BandUser*,
  // guid on the vbase-adjusted subobject +0x30), and the Joypad
  // pad->LocalUser table (RB3E PORT_JOYPAD_USERPTR_BASE / PADFLAG_BASE).
  const uint32_t kJoypadFlagBase = 0x82CCB2A0;  // + p; 0 == connected
  uint32_t mgr = r32(kTheBandUserMgr);
  if (!mgr) return;
  std::string slots;
  for (uint32_t s = 0; s < 4; ++s) {
    uint32_t g = mgr + 0x50 + s * 0x10;
    slots += fmt::format(" slot{}={:08X}{:08X}{:08X}{:08X}", s, r32(g),
                         r32(g + 4), r32(g + 8), r32(g + 12));
  }
  uint32_t vb = r32(mgr + 0x28);
  uint32_t ve = r32(mgr + 0x2C);
  uint32_t n = (ve > vb && ve - vb < 0x100) ? (ve - vb) / 4 : 0;
  std::string parts;
  for (uint32_t i = 0; i < n && i < 4; ++i) {
    uint32_t u = r32(vb + i * 4);
    if (!u) continue;
    uint32_t inner = r32(u + 4);
    uint32_t adj = inner ? r32(inner + 4) : 0;
    uint32_t lb = (adj < 0x1000) ? u + adj : u;
    parts += fmt::format(
        " user{}={{bu=0x{:08X} track={} diff={} shell={} adj=0x{:X} "
        "guid={:08X}{:08X}{:08X}{:08X}}}",
        i, u, static_cast<int32_t>(r32(u + 0x10)),
        static_cast<int32_t>(r32(u + 0x8)), r32(u + 0x20), adj, r32(lb + 0x30),
        r32(lb + 0x34), r32(lb + 0x38), r32(lb + 0x3C));
  }
  std::string pads;
  for (uint32_t p = 0; p < 4; ++p) {
    pads += fmt::format(
        " pad{}={{conn={} lu=0x{:08X}}}", p,
        r8(kJoypadFlagBase + p) == 0 ? 1 : 0,
        r32(kJoypadData + kJoypadUserOffset + p * kJoypadStride));
  }
  XELOGI("RB3DX UI PROBE[{}]:   band: mgr=0x{:08X} n={}{} |{} |{}", sample_,
         mgr, n, slots, parts, pads);
}

void UiProbe::Panels(uint32_t screen, const char* tag) {
  if (!screen) return;
  auto panel_vbase = [&](uint32_t panel) -> uint32_t {
    uint32_t vbp = r32(panel + 4);
    if (!vbp) return 0;
    uint32_t delta = r32(vbp + 4);
    if (delta == 0 || delta > 0x400) return 0;
    return panel + 4 + delta;
  };
  uint32_t dummy = screen + 0x28;
  uint32_t node = r32(dummy);
  for (int n = 0; node && node != dummy && n < 24; ++n, node = r32(node)) {
    uint32_t panel = r32(node + 0x8);
    if (!panel) continue;
    uint32_t vb = panel_vbase(panel);
    std::string pname = vb ? rstr(r32(vb + 0x18)) : "<?>";
    uint32_t ploader = r32(panel + 0xC);
    XELOGI(
        "RB3DX UI PROBE[{}]:   {}panel[{}]@0x{:08X} 0x{:08X}'{}' "
        "active={} refLoaded={} mState={} mLoaded={} mLoader=0x{:08X} "
        "mLoadRefs={} mDir=0x{:08X}",
        sample_, tag, n, node, panel, pname, r8(node + 0xC), r8(node + 0xE),
        r32(panel + 0x20), r8(panel + 0x1C), ploader, r32(panel + 0x28),
        r32(panel + 0x8));
    // A panel stuck at kUnloaded with a live mLoader: tell "still mid-load"
    // from "done-but-not-adopted". DirLoader vtable @+0, mState PTMF code
    // @+0x20 (0x826C3888 = done), file path @+0x14.
    if (ploader && readable(ploader)) {
      uint32_t lstate = r32(ploader + 0x20);
      std::string row;
      for (uint32_t d = ploader; d < ploader + 0x40; d += 4) {
        row += fmt::format(" {:08X}", r32(d));
      }
      XELOGI(
          "RB3DX UI PROBE[{}]:     {}ldr 0x{:08X} vt={:08X} "
          "stateCode={:08X}{} file='{}':{}",
          sample_, tag, ploader, r32(ploader), lstate,
          lstate == 0x826C3888 ? "(DONE)" : "(mid)",
          rstr(r32(ploader + 0x14)), row);
    }
    // SyncGameStartPanel ('sync_audio_net_panel'), the song-start sync gate
    // (rb3-xenon SyncGameStartPanel.h, verified vs the retail ctor): mState
    // @0x3C (4 = StartGame issued, 5 = synced), LockStepMgr @0x40.
    if (pname == "sync_audio_net_panel") {
      XELOGI(
          "RB3DX UI PROBE[{}]:     syncstart mState={} lockMachine=0x{:08X} "
          "hasResponded={} lockSuccess={} externalBlock={}",
          sample_, r32(panel + 0x3C), r32(panel + 0x40 + 0x1C),
          r8(panel + 0x40 + 0x28), r8(panel + 0x40 + 0x29), r8(panel + 0x80));
    }
  }
}

void UiProbe::MainDirObjects(uint32_t cur) {
  if (saveload_obj_ && netsync_obj_ && session_obj_ && overshell_obj_ &&
      (dumped_names_ || !cur)) {
    return;
  }
  uint32_t dir = r32(kMainDirPtr);
  if (!dir) return;
  uint32_t entries = r32(dir + 0x8);
  int size = static_cast<int>(r32(dir + 0xC));
  if (!entries || size <= 0 || size >= 200000) return;
  struct Want {
    const char* name;
    uint32_t* slot;
  } wants[] = {{"saveload_mgr", &saveload_obj_},
               {"net_sync", &netsync_obj_},
               {"session", &session_obj_},
               {"overshell", &overshell_obj_}};
  // One-time (once the UI has a screen): log the whole main-dir name table so
  // other objects can be located offline.
  bool dump_names = !dumped_names_ && cur;
  for (int i = 0; i < size; ++i) {
    uint32_t name_p = r32(entries + i * 8);
    uint32_t obj = r32(entries + i * 8 + 4);
    if (!name_p || !obj) continue;
    std::string nm = rstr(name_p);
    for (auto& w : wants) {
      if (!*w.slot && nm == w.name) {
        *w.slot = obj;
        XELOGI("RB3DX UI PROBE: found {} obj=0x{:08X}", w.name, obj);
      }
    }
    if (dump_names) {
      XELOGI("RB3DX UI PROBE: maindir['{}'] = 0x{:08X}", nm, obj);
    }
  }
  if (dump_names) {
    dumped_names_ = true;
  }
}

void UiProbe::ObjectWindows() {
  // ObjectDir::Entry.obj is the Hmx::Object* BASE pointer; for classes with a
  // VIRTUAL Object base (MsgSource-derived SaveLoadManager; NetSync) that is
  // the vbase at the object's TAIL. Find the derived head by the inverse of
  // the vbptr math: head H has vfptr@H, vbptr P@H+4, [P+4] == vbase - (H+4).
  auto find_derived_head = [&](uint32_t vbase) -> uint32_t {
    for (uint32_t h = vbase - 8; h + 0x400 >= vbase; h -= 4) {
      uint32_t vf = r32(h);
      if (vf < 0x82000000 || vf >= 0x83000000) continue;
      uint32_t vbp = r32(h + 4);
      if (vbp < 0x82000000 || vbp >= 0x83000000) continue;
      if (h + 4 + r32(vbp + 4) == vbase) return h;
    }
    return 0;
  };
  auto dump_obj = [&](const char* tag, uint32_t o) {
    if (!o) return;
    uint32_t head = find_derived_head(o);
    uint32_t base_addr = head ? head : o;
    XELOGI("RB3DX UI PROBE[{}]:   {}('{}') head=0x{:08X} vbase=0x{:08X}",
           sample_, tag, rstr(r32(o + 0x18)), head, o);
    for (uint32_t row = 0; row < 0xC0; row += 0x20) {
      std::string words;
      for (uint32_t k = 0; k < 0x20; k += 4) {
        words += fmt::format(" {:08X}", r32(base_addr + row + k));
      }
      XELOGI("RB3DX UI PROBE[{}]:   {}[+0x{:02X}]:{}", sample_, tag, row,
             words);
    }
  };
  dump_obj("saveload_mgr", saveload_obj_);
  dump_obj("net_sync", netsync_obj_);
  // NetSession: the join gate. kRequestingNewUser(7) = an online join issued
  // and waiting for a response (what --rb3dx_offline_join short-circuits).
  if (session_obj_) {
    uint32_t head = find_derived_head(session_obj_);
    uint32_t b = head ? head : session_obj_;
    XELOGI(
        "RB3DX UI PROBE[{}]:   session head=0x{:08X} mState={} "
        "mQNet=0x{:08X} mUsers[begin=0x{:08X} end=0x{:08X}]",
        sample_, b, r32(b + 0x68), r32(b + 0x70), r32(b + 0x14),
        r32(b + 0x18));
  }
}

void UiProbe::Joypads() {
  // gJoypadData[4] (JoypadGetPadData @0x82524998: 0x82CCB2C8, 0xD4 per pad;
  // rb3-xenon Joypad.h: mButtons@0x00, mUser@0x44, mConnected@0x48,
  // mType@0x6C). mConnected is set by JoypadPollCommon iff
  // gLastXInputState[pad].dwPacketNumber != -1.
  std::string pads, pkts;
  for (uint32_t p = 0; p < 4; ++p) {
    uint32_t b = kJoypadData + p * kJoypadStride;
    pads += fmt::format(" [{}]conn={} user=0x{:08X} type={} btn=0x{:04X}", p,
                        r8(b + 0x48), r32(b + kJoypadUserOffset),
                        static_cast<int32_t>(r32(b + 0x6C)), r32(b + 0x0));
    pkts += fmt::format(" [{}]pkt=0x{:08X}", p,
                        r32(kLastXInputState + p * 0x10));
  }
  XELOGI("RB3DX UI PROBE[{}]:   joypads:{}", sample_, pads);
  XELOGI("RB3DX UI PROBE[{}]:   xinput:{}", sample_, pkts);
}

void UiProbe::Overshell() {
  // Auto-locate OvershellPanel::mSlots once: scan the head region for a
  // vector {begin, end} of 3..8 guest pointers whose pointees read mSlotNum
  // (slot+0x40) == their index. Slot layout (retail, rb3-xenon
  // OvershellSlot.h): mState@0x2C (a state object; its vtable names the
  // OvershellSlotState class), mSlotNum@0x40, mPotentialUsers vector@0x6C of
  // {LocalBandUser*, JoinState}.
  if (overshell_obj_ && !overshell_slots_vec_) {
    uint32_t head = overshell_obj_ - 0x4D4;
    for (uint32_t off = 0; off < 0x200 && !overshell_slots_vec_; off += 4) {
      uint32_t b = r32(head + off), e = r32(head + off + 4);
      if (!b || e <= b || (e - b) % 4 != 0) continue;
      uint32_t n = (e - b) / 4;
      if (n < 3 || n > 8) continue;
      bool ok = true;
      for (uint32_t i = 0; i < n && ok; ++i) {
        uint32_t slot = r32(b + i * 4);
        ok = slot >= 0x40000000 && slot < 0x80000000 && r32(slot + 0x40) == i;
      }
      if (ok) {
        overshell_slots_vec_ = head + off;
        XELOGI(
            "RB3DX UI PROBE: overshell head=0x{:08X} mSlots located at "
            "+0x{:X} ({} slots)",
            head, off, n);
      }
    }
  }
  if (!overshell_slots_vec_) return;
  uint32_t b = r32(overshell_slots_vec_), e = r32(overshell_slots_vec_ + 4);
  for (uint32_t sp = b; sp < e && sp < b + 0x20; sp += 4) {
    uint32_t slot = r32(sp);
    if (!slot) continue;
    uint32_t pu_b = r32(slot + 0x6C), pu_e = r32(slot + 0x70);
    std::string pus;
    for (uint32_t p = pu_b; p + 8 <= pu_e && p < pu_b + 0x40; p += 8) {
      pus += fmt::format(" {{user=0x{:08X} join={}}}", r32(p), r32(p + 4));
    }
    uint32_t st = r32(slot + 0x2C);
    XELOGI(
        "RB3DX UI PROBE[{}]:   oshell slot{} @0x{:08X} state=0x{:08X} "
        "stateVt=0x{:08X} npot={}{}",
        sample_, r32(slot + 0x40), slot, st, st ? r32(st) : 0,
        (pu_e > pu_b) ? (pu_e - pu_b) / 8 : 0, pus);
  }
}

}  // namespace

void StartUiProbe(const TitleLaunchContext& ctx) {
  if (!cvars::rb3dx_ui_probe) {
    if (cvars::rb3_stream_census || cvars::rb3dx_si_claim_anchor) {
      XELOGW(
          "RB3DX: --rb3_stream_census / --rb3dx_si_claim_anchor do nothing "
          "without --rb3dx_ui_probe");
    }
    return;
  }
  Memory* memory = ctx.memory;
  kernel::KernelState* kernel_state = ctx.kernel_state;
  cpu::Processor* processor = ctx.processor;
  SpawnProbeThread([memory, kernel_state, processor]() {
    UiProbe(memory, kernel_state, processor).Run();
  });
  XELOGI("RB3DX: UI probe sampler thread started (--rb3dx_ui_probe)");
}

}  // namespace rb3
}  // namespace titles
}  // namespace xe
