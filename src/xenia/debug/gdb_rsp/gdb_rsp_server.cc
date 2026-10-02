/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026.                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * In-process GDB RSP server. Moved out of app/emulator_headless.cc, where it
 * was the DC3-named "Dc3GdbRspHeadlessListener"; nothing in it is title
 * specific. Usage: docs/fork/debug/gdb_debugging.md.
 */

#include "xenia/debug/gdb_rsp/gdb_rsp_server.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "xenia/base/logging.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/debug/gdb_rsp/gdb_rsp_protocol.h"

DEFINE_bool(gdb_rsp_stub, false,
            "Enable an in-process GDB remote-serial-protocol server in "
            "xenia-headless (PowerPC target, guest addresses; Linux only).",
            "Debug");
DEFINE_string(gdb_rsp_host, "127.0.0.1",
              "Listen host for the GDB RSP server ('*' or 0.0.0.0 = any).",
              "Debug");
DEFINE_int32(gdb_rsp_port, 9001, "Listen port for the GDB RSP server.",
             "Debug");
DEFINE_bool(gdb_rsp_break_on_connect, true,
            "Pause the guest when a GDB client connects to the RSP server.",
            "Debug");
DEFINE_int32(gdb_rsp_prelaunch_sleep_ms, 0,
             "Sleep this long before launching the title (after the RSP "
             "server is listening), so a debugger can attach first.",
             "Debug");

// Deprecated aliases (the server used to be DC3-named). Honoured when the new
// name is left at its default; remove after one release.
DEFINE_bool(dc3_gdb_rsp_stub, false, "DEPRECATED: use --gdb_rsp_stub.", "DC3");
DEFINE_string(dc3_gdb_rsp_host, "127.0.0.1",
              "DEPRECATED: use --gdb_rsp_host.", "DC3");
DEFINE_int32(dc3_gdb_rsp_port, 9001, "DEPRECATED: use --gdb_rsp_port.", "DC3");
DEFINE_bool(dc3_gdb_rsp_break_on_connect, true,
            "DEPRECATED: use --gdb_rsp_break_on_connect.", "DC3");
DEFINE_int32(dc3_gdb_rsp_prelaunch_sleep_ms, 0,
             "DEPRECATED: use --gdb_rsp_prelaunch_sleep_ms.", "DC3");

namespace xe {
namespace debug {
namespace gdb_rsp {

namespace {

bool DeprecatedAliasUsed() {
  return cvars::dc3_gdb_rsp_stub || cvars::dc3_gdb_rsp_host != "127.0.0.1" ||
         cvars::dc3_gdb_rsp_port != 9001 ||
         !cvars::dc3_gdb_rsp_break_on_connect ||
         cvars::dc3_gdb_rsp_prelaunch_sleep_ms != 0;
}

bool Enabled() { return cvars::gdb_rsp_stub || cvars::dc3_gdb_rsp_stub; }

std::string EffectiveHost() {
  return cvars::gdb_rsp_host == "127.0.0.1" ? cvars::dc3_gdb_rsp_host
                                            : cvars::gdb_rsp_host;
}

int32_t EffectivePort() {
  return cvars::gdb_rsp_port == 9001 ? cvars::dc3_gdb_rsp_port
                                     : cvars::gdb_rsp_port;
}

bool EffectiveBreakOnConnect() {
  return cvars::gdb_rsp_break_on_connect &&
         cvars::dc3_gdb_rsp_break_on_connect;
}

}  // namespace

#ifdef __linux__

class GdbRspServer final : public cpu::DebugListener {
 public:
  explicit GdbRspServer(cpu::Processor* processor)
      : processor_(processor) {
    BuildTargetXml();
    server_thread_ = std::thread([this]() { ServerMain(); });
  }

  ~GdbRspServer() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stop_requested_ = true;
      detached_ = true;
      state_cv_.notify_all();
    }
    CloseSocketLocked(listen_fd_);
    CloseSocketLocked(client_fd_);
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    ClearAllBreakpoints();
  }

  void OnFocus() override {}

  void OnDetached() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    detached_ = true;
    state_cv_.notify_all();
  }

  void OnExecutionPaused() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    paused_ = true;
    snapshots_valid_ = false;
    ++stop_epoch_;
    last_signal_ = "S05";
    state_cv_.notify_all();
  }

  void OnExecutionContinued() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    paused_ = false;
    snapshots_valid_ = false;
  }

  void OnExecutionEnded() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ended_ = true;
    paused_ = true;
    snapshots_valid_ = false;
    ++stop_epoch_;
    last_signal_ = "W00";
    state_cv_.notify_all();
  }

  void OnStepCompleted(cpu::ThreadDebugInfo* thread_info) override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (thread_info) {
      selected_thread_id_ = thread_info->thread_id;
    }
    snapshots_valid_ = false;
  }

  void OnBreakpointHit(cpu::Breakpoint* breakpoint,
                       cpu::ThreadDebugInfo* thread_info) override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (thread_info) {
      selected_thread_id_ = thread_info->thread_id;
    }
    if (breakpoint) {
      last_break_guest_pc_ = breakpoint->guest_address();
    }
    snapshots_valid_ = false;
  }

 private:
  using RspPacketReadResult = gdb_rsp::PacketReadResult;
  static constexpr auto kRspPacketKindPacket =
      gdb_rsp::PacketReadKind::kPacket;
  static constexpr auto kRspPacketKindInterrupt =
      gdb_rsp::PacketReadKind::kInterrupt;
  static constexpr auto kRspPacketKindBadChecksum =
      gdb_rsp::PacketReadKind::kBadChecksum;
  static constexpr auto kRspPacketKindEof =
      gdb_rsp::PacketReadKind::kEof;

  struct ThreadSnapshot {
    uint32_t thread_id = 0;
    bool alive = false;
    bool suspended = false;
    uint32_t pc = 0;
    std::array<uint32_t, 32> gpr{};
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t cr = 0;
    uint32_t xer = 0;
    uint32_t fpscr = 0;
  };

  static void CloseSocketLocked(int& fd) {
    if (fd >= 0) {
      shutdown(fd, SHUT_RDWR);
      close(fd);
      fd = -1;
    }
  }

  static bool ParseHexU32(const std::string& s, uint32_t& value) {
    return gdb_rsp::ParseHexU32(s, value);
  }

  static bool ParseHexSize(const std::string& s, size_t& value) {
    return gdb_rsp::ParseHexSize(s, value);
  }

  bool CanUsePauseDebugOps() const { return processor_->stack_walker() != nullptr; }

  void LogNoStackWalkerOnce() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (warned_no_stack_walker_) return;
    warned_no_stack_walker_ = true;
    XELOGW("gdb_rsp: stack walker unavailable in this headless build; "
           "live pause/step/breakpoint debugging is disabled (memory reads + "
           "handshake packets remain available)");
  }

  void BuildTargetXml() {
    std::string xml;
    xml += "<?xml version=\"1.0\"?>\n";
    xml += "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n";
    xml += "<target version=\"1.0\">\n";
    // NOTE: Deliberately omit a <architecture> element. Host gdb (17.2) crashes
    // (SIGABRT inside rs6000 tdep) when a PPC <architecture> is combined with
    // the standard org.gnu.gdb.power.core feature served over qXfer. With the
    // architecture left out, the client selects it via
    // `set architecture powerpc:common[64]; set endian big` and the standard
    // power.core feature is accepted cleanly. See
    // docs/fork/debug/gdb_debugging.md.
    xml += "  <feature name=\"org.gnu.gdb.power.core\">\n";
    int regnum = 0;
    for (int i = 0; i < 32; ++i, ++regnum) {
      xml += fmt::format(
          "    <reg name=\"r{}\" bitsize=\"32\" regnum=\"{}\" type=\"uint32\"/>\n",
          i, regnum);
    }
    for (const char* name : {"pc", "msr", "cr", "lr", "ctr", "xer",
                             "fpscr"}) {
      xml += fmt::format(
          "    <reg name=\"{}\" bitsize=\"32\" regnum=\"{}\" type=\"uint32\"/>\n",
          name, regnum++);
    }
    xml += "  </feature>\n";
    xml += "</target>\n";
    target_xml_ = std::move(xml);
  }

  int OpenListenSocket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      XELOGE("gdb_rsp: socket() failed");
      return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(EffectivePort()));
    if (EffectiveHost().empty() || EffectiveHost() == "*" ||
        EffectiveHost() == "0.0.0.0") {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, EffectiveHost().c_str(),
                         &addr.sin_addr) != 1) {
      XELOGE("gdb_rsp: invalid listen host '{}'",
             EffectiveHost());
      close(fd);
      return -1;
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      XELOGE("gdb_rsp: bind({}:{}) failed", EffectiveHost(),
             EffectivePort());
      close(fd);
      return -1;
    }
    if (listen(fd, 1) != 0) {
      XELOGE("gdb_rsp: listen() failed");
      close(fd);
      return -1;
    }
    return fd;
  }

  void ServerMain() {
    listen_fd_ = OpenListenSocket();
    if (listen_fd_ < 0) {
      return;
    }
    XELOGI("gdb_rsp: listening on {}:{}",
           EffectiveHost().empty() ? "0.0.0.0" : EffectiveHost(),
           EffectivePort());
    while (true) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_ || detached_) break;
      }
      sockaddr_in client_addr = {};
      socklen_t client_len = sizeof(client_addr);
      int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr),
                      &client_len);
      if (fd < 0) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_ || detached_) break;
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        client_fd_ = fd;
        no_ack_mode_ = false;
      }
      // Enable TCP keepalive so a client that crashes/disconnects without a
      // clean D/k is eventually detected and the single-client slot is freed
      // for reconnect (host gdb 17.2 can crash mid-handshake on this target).
      {
        int ka = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        int idle = 2, intvl = 2, cnt = 2;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
      }
      char ip_buf[64] = {};
      inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
      XELOGI("gdb_rsp: client connected {}:{}", ip_buf,
             ntohs(client_addr.sin_port));

      if (EffectiveBreakOnConnect() &&
          processor_->execution_state() == cpu::ExecutionState::kRunning) {
        if (CanUsePauseDebugOps()) {
          uint64_t epoch = 0;
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            epoch = stop_epoch_;
          }
          processor_->Pause();
          WaitForStop(epoch);
        } else {
          LogNoStackWalkerOnce();
        }
      }
      XELOGI("gdb_rsp: entering serve loop");

      ServeClient(fd);

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        CloseSocketLocked(client_fd_);
      }
      // If the client vanished while the target was paused (e.g. a crashed gdb
      // that never sent a clean detach), resume execution so the game is not
      // frozen and a fresh client can attach to a live target.
      if (!stop_requested_ && !detached_ && CanUsePauseDebugOps() &&
          processor_->execution_state() == cpu::ExecutionState::kPaused) {
        ClearAllBreakpoints();
        processor_->Continue();
        XELOGI("gdb_rsp: client gone while paused; auto-continued "
               "target and cleared breakpoints");
      }
      XELOGI("gdb_rsp: client disconnected");
    }
    CloseSocketLocked(listen_fd_);
  }

  void ServeClient(int fd) {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_ || detached_) return;
      }
      RspPacketReadResult pkt = ReadPacket(fd);
      if (pkt.kind == kRspPacketKindEof) {
        return;
      }
      if (pkt.kind == kRspPacketKindBadChecksum) {
        if (!no_ack_mode_) {
          send(fd, "-", 1, 0);
        }
        continue;
      }
      if (pkt.kind == kRspPacketKindInterrupt) {
        if (processor_->execution_state() == cpu::ExecutionState::kRunning) {
          if (CanUsePauseDebugOps()) {
            uint64_t epoch = 0;
            {
              std::lock_guard<std::mutex> lock(state_mutex_);
              epoch = stop_epoch_;
            }
            processor_->Pause();
            WaitForStop(epoch);
          } else {
            LogNoStackWalkerOnce();
          }
        }
        SendPacket(fd, CurrentStopSignal());
        continue;
      }
      if (!no_ack_mode_) {
        send(fd, "+", 1, 0);
      }
      std::string resp = HandlePacket(pkt.payload);
      if (!SendPacket(fd, resp)) {
        return;
      }
      if (pkt.payload == "D" || pkt.payload == "k") {
        return;
      }
    }
  }

  RspPacketReadResult ReadPacket(int fd) {
    return gdb_rsp::ReadPacketFromSocket(fd);
  }

  bool SendPacket(int fd, const std::string& payload) {
    return gdb_rsp::SendPacketToSocket(fd, payload);
  }

  std::string HandlePacket(const std::string& pkt) {
    if (pkt == "?") {
      EnsureSnapshotsForDebugRead();
      return CurrentStopSignal();
    }
    if (pkt == "qSupported" || pkt.rfind("qSupported:", 0) == 0) {
      std::string caps = "PacketSize=4000;QStartNoAckMode+;qXfer:features:read+";
      if (CanUsePauseDebugOps()) {
        caps += ";swbreak+";
      }
      return caps;
    }
    if (pkt == "QStartNoAckMode") {
      no_ack_mode_ = true;
      return "OK";
    }
    if (pkt == "qAttached") return "1";
    if (pkt == "qfThreadInfo" || pkt == "qfThreadInfo:") return ThreadInfoList();
    if (pkt == "qsThreadInfo") return "l";
    if (pkt.rfind("Hg", 0) == 0 || pkt.rfind("Hc", 0) == 0) {
      HandleThreadSelect(pkt.substr(2));
      return "OK";
    }
    if (pkt == "qTStatus") return "";
    if (pkt.rfind("qSymbol", 0) == 0) return "OK";
    if (pkt == "QThreadSuffixSupported") return "OK";
    if (pkt == "vMustReplyEmpty") return "";
    if (pkt == "vCont?") return "vCont;c;s";
    if (pkt.rfind("vCont;", 0) == 0) return HandleVCont(pkt.substr(6));
    if (pkt.rfind("qOffsets", 0) == 0) return "Text=0;Data=0;Bss=0";
    if (pkt.rfind("qC", 0) == 0) return CurrentThreadPacket();
    if (pkt.rfind("T", 0) == 0) return HandleThreadAlive(pkt.substr(1));
    if (pkt == "g") return ReadAllRegistersPacket();
    if (pkt.rfind("p", 0) == 0) return ReadSingleRegisterPacket(pkt.substr(1));
    if (pkt.rfind("m", 0) == 0) return ReadMemoryPacket(pkt.substr(1));
    if (pkt.rfind("Z0,", 0) == 0 || pkt.rfind("z0,", 0) == 0) {
      return HandleBreakpointPacket(pkt);
    }
    if (pkt == "c" || pkt.rfind("c", 0) == 0) return ContinueOrStep(false);
    if (pkt == "s" || pkt.rfind("s", 0) == 0) return ContinueOrStep(true);
    if (pkt.rfind("qXfer:features:read:target.xml:", 0) == 0) {
      return HandleTargetXmlXfer(pkt);
    }
    if (pkt == "D" || pkt == "k") return "OK";
    return "";
  }

  std::string HandleTargetXmlXfer(const std::string& pkt) {
    auto pos = pkt.find("target.xml:");
    if (pos == std::string::npos) return "E20";
    std::string suffix = pkt.substr(pos + std::strlen("target.xml:"));
    auto comma = suffix.find(',');
    if (comma == std::string::npos) return "E20";
    uint32_t off = 0;
    size_t length = 0;
    if (!ParseHexU32(suffix.substr(0, comma), off) ||
        !ParseHexSize(suffix.substr(comma + 1), length)) {
      return "E20";
    }
    if (off >= target_xml_.size()) return "l";
    size_t chunk_len = std::min(length, target_xml_.size() - off);
    bool more = (off + chunk_len) < target_xml_.size();
    return std::string(more ? "m" : "l") +
           target_xml_.substr(off, chunk_len);
  }

  std::string ContinueOrStep(bool step) {
    if (!CanUsePauseDebugOps()) {
      LogNoStackWalkerOnce();
      return step ? "E33" : CurrentStopSignal();
    }
    uint64_t epoch = 0;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      epoch = stop_epoch_;
    }

    if (step) {
      if (!EnsureSnapshotsForDebugRead()) return "E32";
      uint32_t tid = 0;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        tid = selected_thread_id_;
      }
      if (!tid) return "E30";
      if (processor_->execution_state() != cpu::ExecutionState::kPaused) {
        return "E31";
      }
      processor_->StepGuestInstruction(tid);
    } else if (processor_->execution_state() == cpu::ExecutionState::kPaused) {
      processor_->Continue();
    }

    WaitForStop(epoch);
    EnsureSnapshotsForDebugRead();
    return CurrentStopSignal();
  }

  void WaitForStop(uint64_t prev_epoch) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait(lock, [&]() {
      return stop_requested_ || detached_ || stop_epoch_ != prev_epoch;
    });
  }

  std::string CurrentStopSignal() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (last_signal_ == "S05" && selected_thread_id_) {
      return fmt::format("T05thread:{:x};", selected_thread_id_);
    }
    return last_signal_;
  }

  bool EnsureSnapshotsForDebugRead() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (snapshots_valid_) return true;
      if (!paused_ && CanUsePauseDebugOps()) return false;
    }
    RefreshSnapshots();
    std::lock_guard<std::mutex> lock(state_mutex_);
    return snapshots_valid_;
  }

  void HandleThreadSelect(const std::string& spec) {
    if (spec.empty() || spec == "0" || spec == "-1") return;
    uint32_t tid = 0;
    if (!ParseHexU32(spec, tid)) return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    selected_thread_id_ = tid;
  }

  std::string CurrentThreadPacket() {
    if (!EnsureSnapshotsForDebugRead()) return "QC1";
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!selected_thread_id_) return "QC1";
    return fmt::format("QC{:x}", selected_thread_id_);
  }

  std::string ThreadInfoList() {
    if (!EnsureSnapshotsForDebugRead()) return "m1";
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (snapshots_.empty()) return "m1";
    std::string out = "m";
    bool first = true;
    for (const auto& kv : snapshots_) {
      if (!first) out.push_back(',');
      out += fmt::format("{:x}", kv.first);
      first = false;
    }
    return out;
  }

  std::string HandleThreadAlive(const std::string& spec) {
    uint32_t tid = 0;
    if (!ParseHexU32(spec, tid)) return "E40";
    if (!EnsureSnapshotsForDebugRead()) return (tid == 1) ? "OK" : "E41";
    std::lock_guard<std::mutex> lock(state_mutex_);
    return snapshots_.count(tid) ? "OK" : "E41";
  }

  void RefreshSnapshots() {
    auto infos = processor_->QueryThreadDebugInfos();
    std::map<uint32_t, ThreadSnapshot> next;
    bool can_pause_debug = CanUsePauseDebugOps();
    for (auto* info : infos) {
      if (!info || !info->thread) continue;
      if (!info->thread->can_debugger_suspend()) continue;
      if (info->state == cpu::ThreadDebugInfo::State::kExited ||
          info->state == cpu::ThreadDebugInfo::State::kZombie) {
        continue;
      }
      ThreadSnapshot snap;
      snap.thread_id = info->thread_id;
      snap.alive = true;
      snap.suspended = info->suspended;
      const cpu::ppc::PPCContext* ctx = nullptr;
      if (can_pause_debug) {
        ctx = &info->guest_context;
      } else if (info->thread->thread_state() && info->thread->thread_state()->context()) {
        // Headless fallback without a stack walker: sample directly from the
        // live PPC context. Guest PC is not available here, so it remains 0.
        ctx = info->thread->thread_state()->context();
      }
      if (!ctx) continue;
      for (size_t i = 0; i < 32; ++i) {
        snap.gpr[i] = static_cast<uint32_t>(ctx->r[i]);
      }
      snap.lr = static_cast<uint32_t>(ctx->lr);
      snap.ctr = static_cast<uint32_t>(ctx->ctr);
      snap.cr = static_cast<uint32_t>(ctx->cr());
      snap.xer = (ctx->xer_ca ? (1u << 29) : 0) | (ctx->xer_ov ? (1u << 30) : 0) |
                 (ctx->xer_so ? (1u << 31) : 0);
      snap.fpscr = ctx->fpscr.value;
      if (can_pause_debug) {
        for (const auto& frame : info->frames) {
          if (frame.guest_pc) {
            snap.pc = frame.guest_pc;
            break;
          }
        }
      }
      next.emplace(snap.thread_id, std::move(snap));
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    snapshots_ = std::move(next);
    snapshots_valid_ = true;
    if (!selected_thread_id_ || !snapshots_.count(selected_thread_id_)) {
      auto plausible_sp = [](const ThreadSnapshot& s) {
        uint32_t sp = s.gpr[1];
        return (sp & 3) == 0 && sp >= 0x00010000u && sp < 0x80000000u;
      };
      for (const auto& kv : snapshots_) {
        if (kv.second.suspended) {
          selected_thread_id_ = kv.first;
          return;
        }
      }
      for (const auto& kv : snapshots_) {
        if (plausible_sp(kv.second) && kv.second.pc) {
          selected_thread_id_ = kv.first;
          return;
        }
      }
      for (const auto& kv : snapshots_) {
        if (plausible_sp(kv.second) && kv.second.lr) {
          selected_thread_id_ = kv.first;
          return;
        }
      }
      for (const auto& kv : snapshots_) {
        if (plausible_sp(kv.second)) {
          selected_thread_id_ = kv.first;
          return;
        }
      }
      for (const auto& kv : snapshots_) {
        if (kv.second.pc || kv.second.lr) {
          selected_thread_id_ = kv.first;
          return;
        }
      }
      if (!snapshots_.empty()) {
        selected_thread_id_ = snapshots_.begin()->first;
      }
    }
  }

  const ThreadSnapshot* SelectedSnapshotLocked() const {
    if (selected_thread_id_) {
      auto it = snapshots_.find(selected_thread_id_);
      if (it != snapshots_.end()) return &it->second;
    }
    if (!snapshots_.empty()) return &snapshots_.begin()->second;
    return nullptr;
  }

  std::string ReadAllRegistersPacket() {
    if (!EnsureSnapshotsForDebugRead()) return "E10";
    std::lock_guard<std::mutex> lock(state_mutex_);
    const ThreadSnapshot* snap = SelectedSnapshotLocked();
    if (!snap) return "E10";
    std::string out;
    out.reserve((32 + 7) * 8);
    for (uint32_t v : snap->gpr) {
      gdb_rsp::AppendBe32Hex(out, v);
    }
    gdb_rsp::AppendBe32Hex(out, snap->pc);
    gdb_rsp::AppendBe32Hex(out, 0);  // msr
    gdb_rsp::AppendBe32Hex(out, snap->cr);
    gdb_rsp::AppendBe32Hex(out, snap->lr);
    gdb_rsp::AppendBe32Hex(out, snap->ctr);
    gdb_rsp::AppendBe32Hex(out, snap->xer);
    gdb_rsp::AppendBe32Hex(out, snap->fpscr);
    return out;
  }

  std::string ReadSingleRegisterPacket(const std::string& reg_hex) {
    uint32_t regno = 0;
    if (!ParseHexU32(reg_hex, regno)) return "E11";
    if (!EnsureSnapshotsForDebugRead()) return "E12";
    std::lock_guard<std::mutex> lock(state_mutex_);
    const ThreadSnapshot* snap = SelectedSnapshotLocked();
    if (!snap) return "E12";
    uint32_t value = 0;
    if (regno < 32) {
      value = snap->gpr[regno];
    } else {
      switch (regno) {
        case 32:
          value = snap->pc;
          break;
        case 33:
          value = 0;
          break;
        case 34:
          value = snap->cr;
          break;
        case 35:
          value = snap->lr;
          break;
        case 36:
          value = snap->ctr;
          break;
        case 37:
          value = snap->xer;
          break;
        case 38:
          value = snap->fpscr;
          break;
        default:
          return "E13";
      }
    }
    std::string out;
    gdb_rsp::AppendBe32Hex(out, value);
    return out;
  }

  bool IsReadableGuestRange(uint32_t addr, size_t size) const {
    if (!size) return true;
    uint64_t end = uint64_t(addr) + uint64_t(size) - 1;
    if (end > 0xFFFFFFFFull) return false;
    // Validate every touched page against the guest heap protections. Returning
    // an error for unmapped/no-access pages is important: gdb's PPC frame
    // unwinder walks the back-chain by reading memory, and it terminates the
    // walk cleanly when a read fails. Without this it can follow garbage
    // pointers into unmapped space and crash.
    auto* memory = processor_->memory();
    if (!memory) return false;
    constexpr uint32_t kPageMask = 0xFFFu;
    for (uint64_t a = addr & ~uint64_t(kPageMask); a <= end;
         a += (kPageMask + 1)) {
      uint32_t page_addr = static_cast<uint32_t>(a);
      const auto* heap = memory->LookupHeap(page_addr);
      if (!heap) return false;
      uint32_t protect = 0;
      if (!const_cast<xe::BaseHeap*>(heap)->QueryProtect(page_addr, &protect)) {
        return false;
      }
      if (!(protect & xe::kMemoryProtectRead)) {
        return false;
      }
    }
    return true;
  }

  std::string ReadMemoryPacket(const std::string& args) {
    auto comma = args.find(',');
    if (comma == std::string::npos) return "E01";
    uint32_t addr = 0;
    size_t length = 0;
    if (!ParseHexU32(args.substr(0, comma), addr) ||
        !ParseHexSize(args.substr(comma + 1), length)) {
      return "E01";
    }
    if (length > 0x1000) {
      length = 0x1000;
    }
    if (!IsReadableGuestRange(addr, length)) return "E02";
    auto* p = processor_->memory()->TranslateVirtual<const uint8_t*>(addr);
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
      gdb_rsp::AppendHexByte(out, p[i]);
    }
    return out;
  }

  std::string HandleBreakpointPacket(const std::string& pkt) {
    bool add = pkt[0] == 'Z';
    auto first_comma = pkt.find(',');
    auto second_comma = pkt.find(',', first_comma + 1);
    if (first_comma == std::string::npos || second_comma == std::string::npos) {
      return "E03";
    }
    uint32_t addr = 0;
    if (!ParseHexU32(pkt.substr(first_comma + 1, second_comma - first_comma - 1),
                     addr)) {
      return "E03";
    }
    if (add) {
      return AddGuestBreakpoint(addr) ? "OK" : "E04";
    }
    RemoveGuestBreakpoint(addr);
    return "OK";
  }

  bool AddGuestBreakpoint(uint32_t addr) {
    if (!CanUsePauseDebugOps()) {
      LogNoStackWalkerOnce();
      return false;
    }
    std::lock_guard<std::mutex> lock(bp_mutex_);
    if (guest_breakpoints_.count(addr)) return true;
    auto bp = std::make_unique<cpu::Breakpoint>(
        processor_, cpu::Breakpoint::AddressType::kGuest, addr,
        [this](cpu::Breakpoint* breakpoint, cpu::ThreadDebugInfo* thread_info,
               uint64_t host_pc) {
          XELOGI("gdb_rsp: breakpoint hit guest_pc={:08X} host_pc={:X}",
                 breakpoint ? breakpoint->guest_address() : 0, host_pc);
          std::lock_guard<std::mutex> state_lock(state_mutex_);
          if (thread_info) {
            selected_thread_id_ = thread_info->thread_id;
          }
          if (breakpoint) {
            last_break_guest_pc_ = breakpoint->guest_address();
          }
          last_signal_ = "S05";
        });
    processor_->AddBreakpoint(bp.get());
    XELOGI("gdb_rsp: added guest breakpoint @{:08X} (exec_state={})",
           addr, static_cast<int>(processor_->execution_state()));
    guest_breakpoints_.emplace(addr, std::move(bp));
    return true;
  }

  void RemoveGuestBreakpoint(uint32_t addr) {
    std::lock_guard<std::mutex> lock(bp_mutex_);
    auto it = guest_breakpoints_.find(addr);
    if (it == guest_breakpoints_.end()) return;
    processor_->RemoveBreakpoint(it->second.get());
    guest_breakpoints_.erase(it);
  }

  void ClearAllBreakpoints() {
    std::lock_guard<std::mutex> lock(bp_mutex_);
    for (auto& it : guest_breakpoints_) {
      processor_->RemoveBreakpoint(it.second.get());
    }
    guest_breakpoints_.clear();
  }

  std::string HandleVCont(const std::string& args) {
    if (args.empty()) return "";
    // MVP: honor the first action only, ignore thread suffixes.
    char action = args[0];
    if (action == 'c') return ContinueOrStep(false);
    if (action == 's') return ContinueOrStep(true);
    return "";
  }

  cpu::Processor* processor_ = nullptr;

  std::thread server_thread_;
  int listen_fd_ = -1;
  int client_fd_ = -1;
  bool no_ack_mode_ = false;

  mutable std::mutex state_mutex_;
  std::condition_variable state_cv_;
  bool stop_requested_ = false;
  bool detached_ = false;
  bool paused_ = false;
  bool ended_ = false;
  bool snapshots_valid_ = false;
  bool warned_no_stack_walker_ = false;
  uint64_t stop_epoch_ = 0;
  std::string last_signal_ = "S05";
  uint32_t selected_thread_id_ = 0;
  uint32_t last_break_guest_pc_ = 0;
  std::map<uint32_t, ThreadSnapshot> snapshots_;
  std::string target_xml_;

  std::mutex bp_mutex_;
  std::map<uint32_t, std::unique_ptr<cpu::Breakpoint>> guest_breakpoints_;
};

#else

// The server needs BSD sockets; elsewhere it is never created.
class GdbRspServer final : public cpu::DebugListener {
 public:
  void OnFocus() override {}
  void OnDetached() override {}
  void OnExecutionPaused() override {}
  void OnExecutionContinued() override {}
  void OnExecutionEnded() override {}
  void OnStepCompleted(cpu::ThreadDebugInfo*) override {}
  void OnBreakpointHit(cpu::Breakpoint*, cpu::ThreadDebugInfo*) override {}
};

#endif  // __linux__

void GdbRspServerDeleter::operator()(GdbRspServer* server) const {
  delete server;
}

cpu::DebugListener* AsDebugListener(GdbRspServer* server) { return server; }

GdbRspServerPtr CreateServerIfEnabled(cpu::Processor* processor) {
  if (!Enabled()) {
    return nullptr;
  }
  if (DeprecatedAliasUsed()) {
    XELOGW(
        "gdb_rsp: the dc3_gdb_rsp_* options are deprecated; use gdb_rsp_*");
  }
#ifdef __linux__
  GdbRspServerPtr server(new GdbRspServer(processor));
  XELOGI("gdb_rsp: server enabled ({}:{})", EffectiveHost(), EffectivePort());
  return server;
#else
  XELOGW("gdb_rsp: the RSP server is only implemented for Linux");
  return nullptr;
#endif
}

int32_t PrelaunchSleepMs() {
  return cvars::gdb_rsp_prelaunch_sleep_ms
             ? cvars::gdb_rsp_prelaunch_sleep_ms
             : cvars::dc3_gdb_rsp_prelaunch_sleep_ms;
}

}  // namespace gdb_rsp
}  // namespace debug
}  // namespace xe
