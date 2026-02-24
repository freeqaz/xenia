/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#endif

#include "xenia/app/emulator_headless.h"
#include "xenia/base/cvar.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/dc3_runtime_telemetry.h"
#include "xenia/debug/dc3_gdb_rsp_protocol.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xthread.h"
#include "third_party/fmt/include/fmt/format.h"

DEFINE_bool(
    dc3_gdb_rsp_stub, false,
    "DC3: enable an in-process GDB RSP stub in xenia-headless (Phase 4 MVP).",
    "DC3");
DEFINE_string(dc3_gdb_rsp_host, "127.0.0.1",
              "DC3: listen host for the headless GDB RSP stub.", "DC3");
DEFINE_int32(dc3_gdb_rsp_port, 9001,
             "DC3: listen port for the headless GDB RSP stub.", "DC3");
DEFINE_bool(
    dc3_gdb_rsp_break_on_connect, true,
    "DC3: pause target execution when a GDB client connects to the headless "
    "RSP stub.",
    "DC3");

#ifdef __linux__
// Signal-based JIT IP sampler: sends SIGUSR2 to a target thread,
// the handler records RIP from the signal context so the diagnostic
// thread can map it back to a guest PPC address.
static std::atomic<uintptr_t> g_sampled_rip{0};
static std::atomic<bool> g_sampler_installed{false};

static void JitIpSampleHandler(int sig, siginfo_t* info, void* context) {
  auto* uc = static_cast<ucontext_t*>(context);
  g_sampled_rip.store(
      static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]),
      std::memory_order_release);
}

static void InstallJitIpSampler() {
  if (g_sampler_installed.exchange(true)) return;
  struct sigaction sa = {};
  sa.sa_sigaction = JitIpSampleHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR2, &sa, nullptr);
}
#endif  // __linux__

namespace xe {
namespace app {

#ifdef __linux__

class Dc3GdbRspHeadlessListener final : public cpu::DebugListener {
 public:
  explicit Dc3GdbRspHeadlessListener(cpu::Processor* processor)
      : processor_(processor) {
    BuildTargetXml();
    server_thread_ = std::thread([this]() { ServerMain(); });
  }

  ~Dc3GdbRspHeadlessListener() {
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
  using RspPacketReadResult = xe::debug::dc3_gdb_rsp::PacketReadResult;
  static constexpr auto kRspPacketKindPacket =
      xe::debug::dc3_gdb_rsp::PacketReadKind::kPacket;
  static constexpr auto kRspPacketKindInterrupt =
      xe::debug::dc3_gdb_rsp::PacketReadKind::kInterrupt;
  static constexpr auto kRspPacketKindBadChecksum =
      xe::debug::dc3_gdb_rsp::PacketReadKind::kBadChecksum;
  static constexpr auto kRspPacketKindEof =
      xe::debug::dc3_gdb_rsp::PacketReadKind::kEof;

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
    return xe::debug::dc3_gdb_rsp::ParseHexU32(s, value);
  }

  static bool ParseHexSize(const std::string& s, size_t& value) {
    return xe::debug::dc3_gdb_rsp::ParseHexSize(s, value);
  }

  bool CanUsePauseDebugOps() const { return processor_->stack_walker() != nullptr; }

  void LogNoStackWalkerOnce() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (warned_no_stack_walker_) return;
    warned_no_stack_walker_ = true;
    XELOGW("dc3_gdb_rsp_stub: stack walker unavailable in this headless build; "
           "live pause/step/breakpoint debugging is disabled (memory reads + "
           "handshake packets remain available)");
  }

  void BuildTargetXml() {
    std::string xml;
    xml += "<?xml version=\"1.0\"?>\n";
    xml += "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n";
    xml += "<target version=\"1.0\">\n";
    xml += "  <architecture>powerpc:common</architecture>\n";
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
      XELOGE("dc3_gdb_rsp_stub: socket() failed");
      return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cvars::dc3_gdb_rsp_port));
    if (cvars::dc3_gdb_rsp_host.empty() || cvars::dc3_gdb_rsp_host == "*" ||
        cvars::dc3_gdb_rsp_host == "0.0.0.0") {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, cvars::dc3_gdb_rsp_host.c_str(),
                         &addr.sin_addr) != 1) {
      XELOGE("dc3_gdb_rsp_stub: invalid listen host '{}'",
             cvars::dc3_gdb_rsp_host);
      close(fd);
      return -1;
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      XELOGE("dc3_gdb_rsp_stub: bind({}:{}) failed", cvars::dc3_gdb_rsp_host,
             cvars::dc3_gdb_rsp_port);
      close(fd);
      return -1;
    }
    if (listen(fd, 1) != 0) {
      XELOGE("dc3_gdb_rsp_stub: listen() failed");
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
    XELOGI("dc3_gdb_rsp_stub: listening on {}:{}",
           cvars::dc3_gdb_rsp_host.empty() ? "0.0.0.0" : cvars::dc3_gdb_rsp_host,
           cvars::dc3_gdb_rsp_port);
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
      char ip_buf[64] = {};
      inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
      XELOGI("dc3_gdb_rsp_stub: client connected {}:{}", ip_buf,
             ntohs(client_addr.sin_port));

      if (cvars::dc3_gdb_rsp_break_on_connect &&
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
      XELOGI("dc3_gdb_rsp_stub: entering serve loop");

      ServeClient(fd);

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        CloseSocketLocked(client_fd_);
      }
      XELOGI("dc3_gdb_rsp_stub: client disconnected");
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
    return xe::debug::dc3_gdb_rsp::ReadPacketFromSocket(fd);
  }

  bool SendPacket(int fd, const std::string& payload) {
    return xe::debug::dc3_gdb_rsp::SendPacketToSocket(fd, payload);
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
      xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, v);
    }
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, snap->pc);
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, 0);  // msr
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, snap->cr);
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, snap->lr);
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, snap->ctr);
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, snap->xer);
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, snap->fpscr);
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
    xe::debug::dc3_gdb_rsp::AppendBe32Hex(out, value);
    return out;
  }

  bool IsReadableGuestRange(uint32_t addr, size_t size) const {
    if (!size) return true;
    uint64_t end = uint64_t(addr) + uint64_t(size) - 1;
    if (end > 0xFFFFFFFFull) return false;
    // Memory page protection isn't exposed via xe::Memory; MVP only bounds
    // checks the range and relies on expected code/data accesses from GDB.
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
      xe::debug::dc3_gdb_rsp::AppendHexByte(out, p[i]);
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
               uint64_t /*host_pc*/) {
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

class Dc3GdbRspHeadlessListener : public cpu::DebugListener {
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

EmulatorHeadless::EmulatorHeadless(Emulator* emulator)
    : emulator_(emulator),
      emulator_thread_quit_requested_(false),
      emulator_thread_event_(nullptr) {}

EmulatorHeadless::~EmulatorHeadless() {
  if (emulator_ && emulator_->processor() &&
      emulator_->processor()->debug_listener() == dc3_gdb_rsp_listener_.get()) {
    emulator_->processor()->set_debug_listener(nullptr);
  }
  dc3_gdb_rsp_listener_.reset();

  // Shutdown emulator thread
  emulator_thread_quit_requested_.store(true, std::memory_order_relaxed);
  if (emulator_thread_event_) {
    emulator_thread_event_->Set();
  }
  if (emulator_thread_.joinable()) {
    emulator_thread_.join();
  }
}

bool EmulatorHeadless::Initialize(AudioSystemFactory audio_factory,
                                GraphicsSystemFactory graphics_factory,
                                InputDriverFactory input_factory) {
  // Create event for emulator thread communication
  emulator_thread_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  if (!emulator_thread_event_) {
    XELOGE("Failed to create emulator thread event");
    return false;
  }

  // Setup emulator with null backends (display_window = nullptr, imgui_drawer = nullptr)
  X_STATUS result = emulator_->Setup(nullptr, nullptr, true, audio_factory,
                                  graphics_factory, input_factory);
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: {:08X}", result);
    return false;
  }

  XELOGI("Emulator initialized with headless backends");
  XELOGI("  GPU: null");
  XELOGI("  APU: nop");
  XELOGI("  HID: nop");

  if (cvars::dc3_gdb_rsp_stub) {
#ifdef __linux__
    dc3_gdb_rsp_listener_ =
        std::make_unique<Dc3GdbRspHeadlessListener>(emulator_->processor());
    emulator_->processor()->set_debug_listener(dc3_gdb_rsp_listener_.get());
    XELOGI("DC3 headless RSP stub enabled ({}:{})", cvars::dc3_gdb_rsp_host,
           cvars::dc3_gdb_rsp_port);
#else
    XELOGW("dc3_gdb_rsp_stub is only implemented for Linux in xenia-headless");
#endif
  }

  return true;
}

void EmulatorHeadless::EmulatorThread(std::filesystem::path launch_path) {
  xe::threading::set_name("EmulatorHeadless");

  XELOGI("Emulator thread started, launching: {}", xe::path_to_utf8(launch_path));

  // Launch the game from this thread
  X_STATUS result = emulator_->LaunchPath(launch_path);
  if (XFAILED(result)) {
    XELOGE("Failed to launch title: {:08X}", result);
    std::cout << "ERROR: Failed to launch title: 0x" << std::hex << result << std::dec << std::endl;
    return;
  }

  XELOGI("Title launched, entering main loop...");

  // Run until quit requested
  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    // Wait for title to exit
    emulator_->WaitUntilExit();

    // Check if another title was requested
    if (emulator_->TitleRequested()) {
      emulator_->LaunchNextTitle();
    } else {
      // Title has exited, break out of the loop
      break;
    }
  }

  xe::Dc3RuntimeTelemetryRecordBootMilestone("emulator_thread_finished");
  xe::Dc3RuntimeTelemetryEndSession("emulator_thread_finished");
  XELOGI("Emulator thread finished");
}

void EmulatorHeadless::Run() {
  // Run with periodic status reporting until emulator thread exits.
  // This enables diagnostics even when using shell `timeout` externally.
  RunWithTimeout(-1);
}

void EmulatorHeadless::RunWithTimeout(int32_t timeout_ms) {
  // Run until timeout or emulator thread exits
  auto start_time = std::chrono::steady_clock::now();
  int64_t last_report_ms = 0;

  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start_time)
                      .count();

    if (timeout_ms >= 0 && elapsed >= timeout_ms) {
      XELOGI("Timeout of {}ms reached, terminating...", timeout_ms);
      xe::Dc3RuntimeTelemetryRecordBootMilestone("headless_timeout_reached");
      xe::Dc3RuntimeTelemetryEndSession("headless_timeout", timeout_ms);
      std::cout << "TIMEOUT: " << timeout_ms << "ms reached" << std::endl;
      // Use _exit to avoid assertion failures during cleanup
      // The emulator thread is stuck in WaitUntilExit() and can't be cleanly joined
      std::_Exit(0);
    }

    // Periodic thread state report every 3 seconds
    if (elapsed - last_report_ms >= 3000) {
      last_report_ms = elapsed;
      auto* kernel_state = emulator_->kernel_state();
      auto* processor = emulator_->processor();
      if (kernel_state) {
        auto threads = kernel_state->object_table()->GetObjectsByType<kernel::XThread>(
            kernel::XObject::Type::Thread);
        auto segv_count = ExceptionHandler::GetSigsegvCount();
        auto last_fault = ExceptionHandler::GetLastFaultAddress();
        auto last_rip = ExceptionHandler::GetLastFaultRip();
        fprintf(stderr, "=== Thread Status Report (%ldms) === %zu threads, SIGSEGV=%lu last_fault=0x%lX last_rip=0x%lX",
                elapsed, threads.size(), segv_count, last_fault, last_rip);
        // Map last_rip to guest function if it's in the JIT code cache
        if (last_rip != 0 && processor) {
          auto* backend = processor->backend();
          auto* code_cache = backend ? backend->code_cache() : nullptr;
          if (code_cache) {
            auto* jit_fn = code_cache->LookupFunction(last_rip);
            if (jit_fn) {
              uint32_t guest_pc = jit_fn->MapMachineCodeToGuestAddress(last_rip);
              fprintf(stderr, " crash_guest=0x%08X [%s]", guest_pc, jit_fn->name().c_str());
            }
          }
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        for (auto& thread : threads) {
          auto* ppc_ctx = thread->thread_state() ? thread->thread_state()->context() : nullptr;
          if (!ppc_ctx) {
            fprintf(stderr, "  Thread %d: <no context>\n", thread->thread_id());
            fflush(stderr);
            continue;
          }
          uint32_t sp = (uint32_t)ppc_ctx->r[1];
          uint32_t lr = (uint32_t)ppc_ctx->lr;
          // Resolve LR to function name
          std::string lr_name = "?";
          if (processor && lr >= 0x82000000) {
            auto* fn = processor->QueryFunction(lr);
            if (fn) lr_name = fn->name();
          }
          fprintf(stderr, "  Thread %d: LR=0x%08X [%s] SP=0x%08X\n",
                  thread->thread_id(), lr, lr_name.c_str(), sp);
          fflush(stderr);
          // Generic stack frame walk for active threads (nonzero LR), once only
          static uint32_t walked_threads = 0;
          uint32_t tid_bit = 1u << (thread->thread_id() & 31);
          if (lr != 0 && !(walked_threads & tid_bit) && emulator_->memory()) {
            walked_threads |= tid_bit;
            auto* mem_walk = emulator_->memory();
            fprintf(stderr, "    Stack frame walk:\n");
            uint32_t frame_sp = sp;
            for (int fi = 0; fi < 16; ++fi) {
              if (frame_sp < 0x70000000 || frame_sp >= 0x80000000) break;
              auto* p_frame = mem_walk->TranslateVirtual(frame_sp);
              if (!p_frame) break;
              uint32_t back_sp = xe::load_and_swap<uint32_t>(p_frame);
              // Saved LR is at back_sp + 4 (PPC ABI: LR save area at sp+4 in caller's frame)
              // But in the PPC calling convention, LR is saved by the callee at caller_sp+4
              // So we read saved_lr from frame_sp + some offset. Typically at sp+N where
              // the function stored mflr r0; stw r0, N(r1). On Xbox 360, LR is usually
              // saved at offset 0x04 relative to the *caller's* frame pointer (back chain).
              uint32_t saved_lr = 0;
              if (back_sp >= 0x70000000 && back_sp < 0x80000000) {
                auto* p_back = mem_walk->TranslateVirtual(back_sp);
                if (p_back) {
                  // LR save word is at back_sp + 4 in Xbox 360 PPC ABI
                  saved_lr = xe::load_and_swap<uint32_t>(p_back + 4);
                }
              }
              std::string fn_name = "";
              if (processor && saved_lr >= 0x82000000) {
                auto* fn = processor->QueryFunction(saved_lr);
                if (fn) fn_name = fn->name();
              }
              fprintf(stderr, "      [%2d] sp=0x%08X -> 0x%08X  LR=0x%08X [%s]\n",
                      fi, frame_sp, back_sp, saved_lr, fn_name.c_str());
              if (!back_sp || back_sp <= frame_sp || back_sp >= 0x80000000) break;
              frame_sp = back_sp;
            }
            fflush(stderr);
          }
          // For the main game thread, dump key registers to help debug guest code spins
          if (thread->thread_id() == 6) {
            uint32_t ctr = (uint32_t)ppc_ctx->ctr;
            uint32_t r3 = (uint32_t)ppc_ctx->r[3];
            uint32_t r8 = (uint32_t)ppc_ctx->r[8];
            uint32_t r9 = (uint32_t)ppc_ctx->r[9];
            uint32_t r10 = (uint32_t)ppc_ctx->r[10];
            uint32_t r11 = (uint32_t)ppc_ctx->r[11];
            uint32_t r12 = (uint32_t)ppc_ctx->r[12];
            uint32_t r29 = (uint32_t)ppc_ctx->r[29];
            uint32_t r30 = (uint32_t)ppc_ctx->r[30];
            uint32_t r31 = (uint32_t)ppc_ctx->r[31];
            uint32_t r4 = (uint32_t)ppc_ctx->r[4];
            uint32_t r5 = (uint32_t)ppc_ctx->r[5];
            fprintf(stderr, "    regs: r3=0x%08X r4=0x%08X r5=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X\n",
                    r3, r4, r5, r8, r9, r10);
            fprintf(stderr, "          r11=0x%08X r12=0x%08X r30=0x%08X r31=0x%08X CTR=0x%08X\n",
                    r11, r12, r30, r31, ctr);
            // Crash forensics: inspect object-ish pointers that frequently
            // participate in decomp data-as-code crashes.
            if (auto* mem_words = emulator_->memory()) {
              auto dump_words = [&](const char* tag, uint32_t addr) {
                if (addr < 0x82000000 || addr >= 0xA0000000) return;
                auto* p = mem_words->TranslateVirtual(addr);
                if (!p) {
                  fprintf(stderr, "    %s @0x%08X unmapped\n", tag, addr);
                  return;
                }
                fprintf(stderr, "    %s @0x%08X:", tag, addr);
                for (int wi = 0; wi < 8; ++wi) {
                  fprintf(stderr, " %08X",
                          xe::load_and_swap<uint32_t>(p + wi * 4));
                }
                fprintf(stderr, "\n");
              };
              dump_words("r3", r3);
              dump_words("r30", r30);
              dump_words("r29", r29);
              dump_words("r12", r12);

              if (lr == 0x835B3D5C) {
                static bool dumped_invarg_region = false;
                static bool dumped_vsnprintf_region = false;
                static bool dumped_vsnprintf_stack = false;
                static bool dumped_hx_snprintf_region = false;
                static bool dumped_formatstring_region = false;
                if (!dumped_invarg_region) {
                  dumped_invarg_region = true;
                  constexpr uint32_t kInvargStart = 0x835B9B08;  // _initp_misc_invarg
                  constexpr uint32_t kInvargEnd = 0x835B9C20;    // past _invoke_watson
                  fprintf(stderr,
                          "    [dc3-debug] invarg.obj code dump near trap loop (LR=0x835B3D5C):\n");
                  for (uint32_t a = kInvargStart; a < kInvargEnd; a += 4) {
                    auto* p = mem_words->TranslateVirtual(a);
                    uint32_t w = p ? xe::load_and_swap<uint32_t>(p) : 0;
                    fprintf(stderr, "      0x%08X: %08X\n", a, w);
                  }
                }
                if (!dumped_vsnprintf_region) {
                  dumped_vsnprintf_region = true;
                  constexpr uint32_t kVsnprintfStart = 0x835B3CE0;
                  constexpr uint32_t kVsnprintfEnd = 0x835B3E10;
                  fprintf(stderr,
                          "    [dc3-debug] _vsnprintf_l code dump near trap caller (LR=0x835B3D5C):\n");
                  for (uint32_t a = kVsnprintfStart; a < kVsnprintfEnd; a += 4) {
                    auto* p = mem_words->TranslateVirtual(a);
                    uint32_t w = p ? xe::load_and_swap<uint32_t>(p) : 0;
                    const char* mark = (a == 0x835B3D58) ? "  <callsite>" : "";
                    fprintf(stderr, "      0x%08X: %08X%s\n", a, w, mark);
                  }
                }
                if (!dumped_vsnprintf_stack) {
                  dumped_vsnprintf_stack = true;
                  fprintf(stderr,
                          "    [dc3-debug] stack dump near trap loop SP=0x%08X (Thread 6):\n",
                          sp);
                  for (int i = -8; i < 32; ++i) {
                    uint32_t a = sp + static_cast<uint32_t>(i * 4);
                    auto* p = mem_words->TranslateVirtual(a);
                    uint32_t w = p ? xe::load_and_swap<uint32_t>(p) : 0;
                    fprintf(stderr, "      [sp%+04d] 0x%08X: %08X\n", i * 4, a, w);
                  }
                  fprintf(stderr,
                          "    [dc3-debug] stack frame chain (saved LR at sp-8):\n");
                  uint32_t frame_sp = sp;
                  for (int fi = 0; fi < 8; ++fi) {
                    auto* p_back = mem_words->TranslateVirtual(frame_sp);
                    auto* p_lr = mem_words->TranslateVirtual(frame_sp - 8);
                    uint32_t back = p_back ? xe::load_and_swap<uint32_t>(p_back) : 0;
                    uint32_t saved_lr =
                        p_lr ? xe::load_and_swap<uint32_t>(p_lr) : 0;
                    fprintf(stderr,
                            "      frame[%d] sp=0x%08X saved_lr=0x%08X next_sp=0x%08X\n",
                            fi, frame_sp, saved_lr, back);
                    if (!back || back == frame_sp || back < 0x70000000 ||
                        back >= 0x80000000) {
                      break;
                    }
                    frame_sp = back;
                  }
                }
                if (!dumped_hx_snprintf_region) {
                  dumped_hx_snprintf_region = true;
                  constexpr uint32_t kHxSnprintfStart = 0x83477F78;
                  constexpr uint32_t kHxSnprintfEnd = 0x83478020;
                  fprintf(stderr,
                          "    [dc3-debug] Hx_snprintf code dump (frame[1], saved LR=0x83477FC0):\n");
                  for (uint32_t a = kHxSnprintfStart; a < kHxSnprintfEnd; a += 4) {
                    auto* p = mem_words->TranslateVirtual(a);
                    uint32_t w = p ? xe::load_and_swap<uint32_t>(p) : 0;
                    const char* mark = (a == 0x83477FC0) ? "  <saved_lr>" : "";
                    fprintf(stderr, "      0x%08X: %08X%s\n", a, w, mark);
                  }
                }
                if (!dumped_formatstring_region) {
                  dumped_formatstring_region = true;
                  constexpr uint32_t kFmtStart = 0x83346E94;
                  constexpr uint32_t kFmtEnd = 0x83346FA0;
                  fprintf(stderr,
                          "    [dc3-debug] FormatString::operator<< code dump (frame[2], saved LR=0x83346F4C):\n");
                  for (uint32_t a = kFmtStart; a < kFmtEnd; a += 4) {
                    auto* p = mem_words->TranslateVirtual(a);
                    uint32_t w = p ? xe::load_and_swap<uint32_t>(p) : 0;
                    const char* mark = (a == 0x83346F4C) ? "  <saved_lr>" : "";
                    fprintf(stderr, "      0x%08X: %08X%s\n", a, w, mark);
                  }
                }
              }
            }
            fflush(stderr);
            // Read strings from registers that might be pointers to guest memory
            auto* mem_str = emulator_->memory();
            if (mem_str) {
              auto read_guest_str = [&](const char* label, uint32_t addr) {
                if (addr >= 0x82000000 && addr < 0x90000000) {
                  auto* ptr = mem_str->TranslateVirtual(addr);
                  if (ptr) {
                    char buf[64] = {};
                    memcpy(buf, ptr, 63);
                    buf[63] = '\0';
                    // Check if it looks like a printable string
                    bool printable = true;
                    int len = 0;
                    for (int si = 0; si < 63 && buf[si]; si++) {
                      if (buf[si] < 0x20 || buf[si] > 0x7E) { printable = false; break; }
                      len++;
                    }
                    if (printable && len > 0) {
                      fprintf(stderr, "    %s[0x%08X] = \"%.*s\"\n", label, addr, len, buf);
                    }
                  }
                }
              };
              read_guest_str("r3", r3);
              read_guest_str("r4", r4);
              read_guest_str("r5", r5);
              read_guest_str("r31", r31);
              read_guest_str("r30", r30);
              // Also read from stack - NextName stores path on stack
              for (int si = 0; si < 4; si++) {
                uint32_t stack_addr = sp + 0x30 + si * 0x20;
                read_guest_str("stk", stack_addr);
              }
              fflush(stderr);
              // Dump raw RTL_CRITICAL_SECTION bytes at r31+4 (CritSec wrapper)
              // X_RTL_CRITICAL_SECTION: X_DISPATCH_HEADER(16B) lock_count(4B) recursion(4B) owner(4B)
              // X_DISPATCH_HEADER: type(1B) absolute(1B) size(1B) insert(1B) signal(4B) wait_list(8B)
              auto dump_cs_raw = [&](const char* label, uint32_t addr) {
                if (addr >= 0x40000000 && addr < 0x50000000) {
                  auto* p = mem_str->TranslateVirtual(addr);
                  if (p) {
                    fprintf(stderr, "    %s RTL_CS[0x%08X]: ", label, addr);
                    for (int bi = 0; bi < 28; bi++) {
                      fprintf(stderr, "%02X", p[bi]);
                      if (bi == 3 || bi == 7 || bi == 11 || bi == 15 || bi == 19 || bi == 23) fprintf(stderr, " ");
                    }
                    // Parse key fields (big-endian in guest memory)
                    uint8_t cs_type = p[0];
                    uint8_t cs_abs = p[1];
                    int32_t lock = xe::load_and_swap<int32_t>(p + 0x10);
                    int32_t rec = xe::load_and_swap<int32_t>(p + 0x14);
                    uint32_t owner = xe::load_and_swap<uint32_t>(p + 0x18);
                    fprintf(stderr, "\n      type=%d abs=%d lock=%d rec=%d owner=0x%08X\n",
                            cs_type, cs_abs, lock, rec, owner);
                    fflush(stderr);
                  }
                }
              };
              // The CriticalSection C++ wrapper stores RTL_CS at offset 4
              dump_cs_raw("r31+4", r31 + 4);
              // Also check if r3 directly points to an RTL_CS (passed to RtlEnterCS)
              if (r3 != r31 + 4 && r3 >= 0x40000000 && r3 < 0x50000000) {
                dump_cs_raw("r3", r3);
              }
            }
#ifdef __linux__
            // Sample the game thread's x86 IP via SIGUSR2 multiple times to
            // detect tight loops vs forward progress.
            {
              InstallJitIpSampler();
              auto* xe_thread = thread->thread();
              if (xe_thread) {
                auto* native = xe_thread->native_handle();
                if (native) {
                  pthread_t pt = reinterpret_cast<pthread_t>(native);
                  auto* backend = processor->backend();
                  auto* code_cache = backend ? backend->code_cache() : nullptr;
                  uint32_t guest_samples[3] = {};
                  std::string fn_names[3];
                  for (int si = 0; si < 3; si++) {
                    if (si > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    g_sampled_rip.store(0, std::memory_order_release);
                    pthread_kill(pt, SIGUSR2);
                    for (int w = 0; w < 100 && g_sampled_rip.load(std::memory_order_acquire) == 0; w++) {
                      std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    uintptr_t rip = g_sampled_rip.load(std::memory_order_acquire);
                    if (rip != 0 && code_cache) {
                      auto* jit_fn = code_cache->LookupFunction(static_cast<uint64_t>(rip));
                      if (jit_fn) {
                        guest_samples[si] = jit_fn->MapMachineCodeToGuestAddress(rip);
                        fn_names[si] = jit_fn->name();
                      }
                    }
                  }
                  fprintf(stderr, "    JIT IP samples (50ms apart):\n");
                  for (int si = 0; si < 3; si++) {
                    fprintf(stderr, "      [%d] guest 0x%08X [%s]\n",
                            si, guest_samples[si], fn_names[si].c_str());
                  }
                  if (guest_samples[0] != 0 && guest_samples[0] == guest_samples[1] &&
                      guest_samples[1] == guest_samples[2]) {
                    fprintf(stderr, "    *** LIKELY TIGHT LOOP at guest 0x%08X ***\n",
                            guest_samples[0]);
                  } else if (fn_names[0] == fn_names[1] && fn_names[1] == fn_names[2] &&
                             !fn_names[0].empty()) {
                    fprintf(stderr, "    *** SPINNING IN SAME FUNCTION: %s ***\n",
                            fn_names[0].c_str());
                  }
                  fflush(stderr);
                }
              }
            }
#endif  // __linux__
            // Dump IAT and thunk code for RtlEnterCriticalSection
            auto* mem2 = emulator_->memory();
            if (mem2) {
              // NOTE: SP=0 helper patch DISABLED -- address 0x8311B8E0 is stale
              // after dc3-decomp relink (now falls inside SleepEx). Re-enable
              // only if the SP=0 fault recurs at a new address.
              auto* iat_ptr = mem2->TranslateVirtual(0x823996E8);
              if (iat_ptr) {
                uint32_t iat_val = xe::load_and_swap<uint32_t>(iat_ptr);
                fprintf(stderr, "    IAT[RtlEnterCS @ 0x823996E8] = 0x%08X\n", iat_val);
              }
              // Dump thunk code at 0x83A00964 (should be sc 2; blr after patching)
              auto* thunk_ptr = mem2->TranslateVirtual(0x83A00964);
              if (thunk_ptr) {
                fprintf(stderr, "    Thunk[0x83A00964]:");
                for (int ti = 0; ti < 4; ti++) {
                  uint32_t instr = xe::load_and_swap<uint32_t>(thunk_ptr + ti * 4);
                  fprintf(stderr, " %08X", instr);
                }
                fprintf(stderr, " (expect: 44000042 4E800020 60000000 60000000)\n");
              }
              fflush(stderr);
            }
            // Check import thunk function state via processor
            auto* fn_at_thunk = processor->QueryFunction(0x83A00964);
            if (fn_at_thunk) {
              auto* guest_fn = static_cast<cpu::GuestFunction*>(fn_at_thunk);
              fprintf(stderr, "    Thunk fn: status=%d behavior=%d has_handler=%d machine_code=%p\n",
                      (int)guest_fn->status(), (int)guest_fn->behavior(),
                      guest_fn->extern_handler() != nullptr,
                      guest_fn->machine_code());
            } else {
              fprintf(stderr, "    Thunk fn: NOT FOUND by QueryFunction\n");
            }
            // Read indirection table entry directly (host address = guest address for 0x8xxxxxxx)
            uint32_t* indir_entry = reinterpret_cast<uint32_t*>(
                static_cast<uintptr_t>(0x83A00964));
            fprintf(stderr, "    Indirection[0x83A00964] = 0x%08X\n", *indir_entry);
            // Kernel shim call counters
            fprintf(stderr, "    RtlEnterCS=%u RtlInitCS=%u RtlLeaveCS=%u\n",
                    kernel::xboxkrnl::GetRtlEnterCsCount(),
                    kernel::xboxkrnl::GetRtlInitCsCount(),
                    kernel::xboxkrnl::GetRtlLeaveCsCount());
            // Function probes: check if key present-pipeline functions are JIT-compiled
            {
              struct FnProbe {
                uint32_t addr;
                const char* name;
              };
              static const FnProbe probes[] = {
                // CRT entry pipeline
                {0x8311ab84, "mainCRTStartup"},
                {0x82463b60, "main()"},
                {0x8300d43c, "App::App()"},
                // Init functions called by App::App
                {0x833a3c74, "DxRnd::PreInit"},
                {0x835182f8, "SystemInit"},
                {0x82ae9d48, "DataInit"},
                {0x83154278, "ObjectDir::PreInit"},
                {0x83151a48, "ObjectDir::Init"},
                {0x83413dbc, "FileInit"},
                {0x8343b98c, "LoadMgr::Init"},
                {0x833a3f64, "DxRnd::Init"},
                {0x8342e508, "UIManager::Init"},
                {0x8342bf88, "UIManager ctor"},
                // App main loop
                {0x8300dccc, "App::Run()"},
                {0x8300c2f4, "RunWithoutDbg"},
                {0x8300ad30, "App::DrawRegular"},
                {0x8300b228, "MainThread"},
                // Present pipeline
                {0x8339fcb0, "DxRnd::Present"},
                {0x8301bd90, "Game::PostUpdate"},
                {0x837ea5c0, "D3DDevice_Swap"},
                // Current decomp-only loop / telemetry hotspots
                {0x83940f44, "PIXAddPixOpWithData"},
                {0x8256ef00, "NavListSortMgr::ClearHeaders"},
                {0x8256f020, "NavListSortMgr::GetFirstChildSymbolFromHeaderSymbol"},
              };
              fprintf(stderr, "    Function probes (present pipeline):\n");
              for (const auto& p : probes) {
                auto* fn = processor->QueryFunction(p.addr);
                if (fn) {
                  auto* gfn = static_cast<cpu::GuestFunction*>(fn);
                  fprintf(stderr, "      0x%08X %-22s COMPILED status=%d mc=%p\n",
                          p.addr, p.name, (int)gfn->status(),
                          gfn->machine_code());
                } else {
                  fprintf(stderr, "      0x%08X %-22s NOT COMPILED\n",
                          p.addr, p.name);
                }
              }
              fflush(stderr);
            }
            // One-shot: dump D3D::UnlockResource PPC code to understand the loop
            {
              static bool unlock_dumped = false;
              if (!unlock_dumped) {
                unlock_dumped = true;
                // D3D::UnlockResource: 0x83779C98 to 0x83779DF0
                fprintf(stderr, "    D3D::UnlockResource (0x83779C98) PPC code:\n");
                for (uint32_t a = 0x83779C98; a < 0x83779DF0; a += 4) {
                  auto* pp = mem2->TranslateVirtual(a);
                  if (!pp) break;
                  uint32_t instr = xe::load_and_swap<uint32_t>(pp);
                  const char* note = "";
                  if ((instr & 0xFC000001) == 0x48000001) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  bl 0x%08X\n", a, instr, t);
                    continue;
                  }
                  if ((instr & 0xFC000003) == 0x48000000) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  b  0x%08X\n", a, instr, t);
                    continue;
                  }
                  if (instr == 0x4E800020) note = "  blr";
                  if ((instr >> 16) == 0x4200) note = "  bdnz (CTR loop)";
                  if ((instr >> 16) == 0x4082) note = "  bne";
                  if ((instr >> 16) == 0x4182) note = "  beq";
                  if ((instr >> 16) == 0x409A) note = "  bge (cr2)";
                  if ((instr >> 16) == 0x419A) note = "  blt (cr2)";
                  if ((instr >> 16) == 0x4080) note = "  bge";
                  if ((instr >> 16) == 0x4180) note = "  blt";
                  fprintf(stderr, "      0x%08X: %08X%s\n", a, instr, note);
                }
                // Also dump D3DVertexBuffer_Unlock: 0x8377A188 to 0x8377A1E0
                fprintf(stderr, "    D3DVertexBuffer_Unlock (0x8377A188) PPC code:\n");
                for (uint32_t a = 0x8377A188; a < 0x8377A1E0; a += 4) {
                  auto* pp = mem2->TranslateVirtual(a);
                  if (!pp) break;
                  uint32_t instr = xe::load_and_swap<uint32_t>(pp);
                  if ((instr & 0xFC000001) == 0x48000001) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  bl 0x%08X\n", a, instr, t);
                    continue;
                  }
                  if ((instr & 0xFC000003) == 0x48000000) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  b  0x%08X\n", a, instr, t);
                    continue;
                  }
                  const char* note = "";
                  if (instr == 0x4E800020) note = "  blr";
                  fprintf(stderr, "      0x%08X: %08X%s\n", a, instr, note);
                }
                // Dump merged_VBLockDtor: 0x834B0184 to 0x834B019C
                fprintf(stderr, "    merged_VBLockDtor (0x834B0184) PPC code:\n");
                for (uint32_t a = 0x834B0184; a < 0x834B019C; a += 4) {
                  auto* pp = mem2->TranslateVirtual(a);
                  if (!pp) break;
                  uint32_t instr = xe::load_and_swap<uint32_t>(pp);
                  if ((instr & 0xFC000001) == 0x48000001) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  bl 0x%08X\n", a, instr, t);
                    continue;
                  }
                  const char* note = "";
                  if (instr == 0x4E800020) note = "  blr";
                  fprintf(stderr, "      0x%08X: %08X%s\n", a, instr, note);
                }
                // Dump D3D::FlushCachedMemory: 0x8379478C to 0x83794860
                fprintf(stderr, "    D3D::FlushCachedMemory (0x8379478C) PPC code:\n");
                for (uint32_t a = 0x8379478C; a < 0x83794860; a += 4) {
                  auto* pp = mem2->TranslateVirtual(a);
                  if (!pp) break;
                  uint32_t instr = xe::load_and_swap<uint32_t>(pp);
                  if ((instr & 0xFC000001) == 0x48000001) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  bl 0x%08X\n", a, instr, t);
                    continue;
                  }
                  if ((instr & 0xFC000003) == 0x48000000) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  b  0x%08X\n", a, instr, t);
                    continue;
                  }
                  const char* note = "";
                  if (instr == 0x4E800020) note = "  blr";
                  if ((instr >> 16) == 0x4200) note = "  bdnz (CTR loop)";
                  if ((instr >> 16) == 0x4082) note = "  bne";
                  if ((instr >> 16) == 0x4182) note = "  beq";
                  fprintf(stderr, "      0x%08X: %08X%s\n", a, instr, note);
                }
                fflush(stderr);
              }
            }
            // One-shot: dump key function call targets
            {
              static bool app_calls_dumped = false;
              if (!app_calls_dumped) {
                app_calls_dumped = true;
                // Dump MainThread PPC instructions (small function, ~72 bytes)
                fprintf(stderr, "    MainThread (0x82F1A070) PPC code:\n");
                for (uint32_t a = 0x82F1A070; a < 0x82F1A0C0; a += 4) {
                  auto* pp = mem2->TranslateVirtual(a);
                  if (!pp) break;
                  uint32_t instr = xe::load_and_swap<uint32_t>(pp);
                  const char* note = "";
                  if ((instr & 0xFC000001) == 0x48000001) {
                    int32_t off = instr & 0x03FFFFFC;
                    if (off & 0x02000000) off |= 0xFC000000;
                    uint32_t t = a + off;
                    fprintf(stderr, "      0x%08X: %08X  bl 0x%08X\n", a, instr, t);
                    continue;
                  }
                  if (instr == 0x4E800020) note = "  blr";
                  if ((instr >> 26) == 19 && ((instr >> 1) & 0x3FF) == 528)
                    note = "  bctr/bctrl";
                  fprintf(stderr, "      0x%08X: %08X%s\n", a, instr, note);
                }
                fflush(stderr);
                // Dump App::App() call targets
                uint32_t fn_start = 0x82F1C284;
                uint32_t fn_end = 0x82F1CB14;
                fprintf(stderr, "    App::App() call targets (bl instructions):\n");
                for (uint32_t addr = fn_start; addr < fn_end; addr += 4) {
                  auto* p = mem2->TranslateVirtual(addr);
                  if (!p) continue;
                  uint32_t instr = xe::load_and_swap<uint32_t>(p);
                  // PPC bl instruction: opcode 18 (bits 0-5 = 010010), LK=1 (bit 31)
                  if ((instr & 0xFC000001) == 0x48000001) {
                    // Extract signed 26-bit offset
                    int32_t offset = instr & 0x03FFFFFC;
                    if (offset & 0x02000000) offset |= 0xFC000000; // sign extend
                    uint32_t target = addr + offset;
                    // Check if target function is compiled
                    auto* fn = processor->QueryFunction(target);
                    fprintf(stderr, "      +0x%03X bl 0x%08X %s\n",
                            addr - fn_start, target,
                            fn ? "COMPILED" : "not-compiled");
                  }
                }
                fflush(stderr);
              }
            }
            // Check page state at XEX base (0x82000000) for SIGSEGV diagnosis
            {
              auto* heap82 = mem2->LookupHeap(0x82000000);
              if (heap82) {
                uint32_t alloc_len = 0;
                uint32_t prot = 0;
                uint32_t state = 0;
                heap82->QuerySize(0x82000000, &alloc_len);
                fprintf(stderr, "    Heap[0x82000000]: type=%d alloc_len=0x%X\n",
                        (int)heap82->heap_type(), alloc_len);
              }
              // Try reading from host address (diagnostic thread context)
              auto* host82 = mem2->TranslateVirtual(0x82000000);
              if (host82) {
                fprintf(stderr, "    Host ptr for 0x82000000: %p\n", host82);
                // Read first 4 bytes (safe from diagnostic thread)
                uint32_t val = xe::load_and_swap<uint32_t>(host82);
                fprintf(stderr, "    Read 0x82000000: 0x%08X (%c%c)\n",
                        val, (char)(val >> 24), (char)((val >> 16) & 0xFF));
              }
            }
            // Dump guest code near current LR to identify hang site
            if (lr >= 0x82000000 && lr < 0x84000000) {
              uint32_t dump_start = (lr > 0x60) ? (lr - 0x60) : lr;
              fprintf(stderr, "    Guest code near LR=0x%08X:\n", lr);
              for (int ci = 0; ci < 40; ci++) {
                uint32_t addr = dump_start + ci * 4;
                auto* cip = mem2->TranslateVirtual(addr);
                if (!cip) break;
                uint32_t instr = xe::load_and_swap<uint32_t>(cip);
                fprintf(stderr, "      0x%08X: %08X%s\n",
                        addr, instr, (addr == lr) ? "  <-- LR" : "");
              }
            }
            // Verify XAUDIO2 stubs are still in guest memory
            {
              struct { uint32_t addr; const char* name; } stubs[] = {
                {0x8340E198, "CX2SourceVoice::Initialize"},
                {0x8340E578, "CX2SourceVoice::Start"},
                {0x8340E6CC, "CX2SourceVoice::Stop"},
              };
              for (const auto& s : stubs) {
                auto* sp = mem2->TranslateVirtual(s.addr);
                if (sp) {
                  uint32_t w0 = xe::load_and_swap<uint32_t>(sp);
                  uint32_t w1 = xe::load_and_swap<uint32_t>(
                      reinterpret_cast<const uint8_t*>(sp) + 4);
                  bool is_stub = (w0 == 0x38600000 && w1 == 0x4E800020);
                  fprintf(stderr,
                      "    Stub@0x%08X %-30s: %08X %08X %s\n",
                      s.addr, s.name, w0, w1,
                      is_stub ? "OK (li r3,0; blr)" : "*** NOT STUBBED ***");
                }
              }
            }
            // Dump linked list at XapiThreadNotifyRoutineList (from MAP)
            // The list sentinel is the list head address itself
            // Address regenerated from MAP: 2026-02-22 (Session 9)
            {
              uint32_t sentinel = 0x83B14C3C;
              auto* head_ptr = mem2->TranslateVirtual(sentinel);
              if (head_ptr) {
                uint32_t head_val = xe::load_and_swap<uint32_t>(head_ptr);
                fprintf(stderr, "    NotifyList[0x%08X] head=0x%08X %s\n",
                        sentinel, head_val,
                        (head_val == sentinel) ? "(EMPTY)" : "(NON-EMPTY)");
                // Walk the list (max 8 entries)
                if (head_val != sentinel && head_val != 0) {
                  uint32_t node_addr = head_val;
                  for (int ni = 0; ni < 8 && node_addr != sentinel && node_addr != 0; ni++) {
                    auto* node_ptr = mem2->TranslateVirtual(node_addr);
                    if (!node_ptr) break;
                    uint32_t next = xe::load_and_swap<uint32_t>(node_ptr);
                    uint32_t field4 = xe::load_and_swap<uint32_t>(node_ptr + 4);
                    uint32_t callback = xe::load_and_swap<uint32_t>(node_ptr + 8);
                    fprintf(stderr, "      node[%d] @0x%08X: next=0x%08X +4=0x%08X callback=0x%08X\n",
                            ni, node_addr, next, field4, callback);
                    node_addr = next;
                  }
                }
              }
            }
            fflush(stderr);
          }
          // Walk PPC stack frames using __savegprlr convention (LR at back_chain - 8)
          // Accept SPs in thread stack area (0x70000000-0x78000000) OR heap area
          // (0x00010000-0x50000000) where game-allocated stacks live.
          auto* mem = emulator_->memory();
          auto sp_valid = [](uint32_t a) {
            return (a & 3) == 0 && a >= 0x00010000 && a < 0x78000000;
          };
          if (sp_valid(sp) && (sp & 0xFFFF) != 0) {
            for (int frame = 0; frame < 20; frame++) {
              if (!sp_valid(sp)) {
                fprintf(stderr, "    [%d] STOP: sp=0x%08X invalid\n", frame, sp);
                break;
              }
              auto* host_ptr = mem->TranslateVirtual(sp);
              if (!host_ptr) {
                fprintf(stderr, "    [%d] STOP: sp=0x%08X unmapped\n", frame, sp);
                break;
              }
              uint32_t back_chain = xe::load_and_swap<uint32_t>(host_ptr);
              uint32_t saved_lr = xe::load_and_swap<uint32_t>(host_ptr + 4);
              // Also read lr from back_chain-8 (__savegprlr convention)
              uint32_t lr_bc8 = 0;
              if (sp_valid(back_chain) && back_chain >= 8) {
                auto* bc_ptr = mem->TranslateVirtual(back_chain - 8);
                if (bc_ptr) lr_bc8 = xe::load_and_swap<uint32_t>(bc_ptr);
              }
              // Pick best LR: prefer lr_bc8 if it looks like a code address
              uint32_t best_lr = saved_lr;
              if (lr_bc8 >= 0x82000000 && lr_bc8 < 0x8A000000) best_lr = lr_bc8;
              else if (saved_lr >= 0x82000000 && saved_lr < 0x8A000000) best_lr = saved_lr;
              // Resolve saved LR to function name
              std::string fn_name = "";
              if (processor && best_lr >= 0x82000000) {
                auto* fn = processor->QueryFunction(best_lr);
                if (fn) fn_name = " [" + fn->name() + "]";
              }
              fprintf(stderr, "    [%d] sp=0x%08X back=0x%08X lr_sp4=0x%08X lr_bc8=0x%08X%s\n",
                      frame, sp, back_chain, saved_lr, lr_bc8, fn_name.c_str());
              fflush(stderr);
              if (back_chain == 0 || back_chain == sp || !sp_valid(back_chain)) {
                fprintf(stderr, "    [%d] END: back=0x%08X (zero=%d same=%d valid=%d)\n",
                        frame, back_chain, back_chain == 0, back_chain == sp,
                        sp_valid(back_chain));
                break;
              }
              sp = back_chain;
            }
          }
        }
      }
    }

    // Check if thread is still alive
    if (!emulator_thread_.joinable()) {
      break;
    }

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for thread to finish (only if we exited normally, not via timeout)
  if (emulator_thread_.joinable()) {
    emulator_thread_.join();
  }
}

void EmulatorHeadless::StartEmulatorThread(std::filesystem::path launch_path) {
  emulator_thread_quit_requested_.store(false, std::memory_order_relaxed);
  emulator_thread_ = std::thread(&EmulatorHeadless::EmulatorThread, this, launch_path);
}

void EmulatorHeadless::SetupBootReporting() {
  boot_reporting_enabled_ = true;

  // Set up launch callback - reports when title is loaded
  // Delegate signature: uint32_t title_id, const std::string_view game_title
  emulator_->on_launch.AddListener([this](uint32_t title_id,
                                      const std::string_view game_title) {
    if (boot_reporting_enabled_) {
      std::cout << "BOOT: Title loaded successfully" << std::endl;
      std::cout << "BOOT: Title ID: 0x" << std::hex << title_id << std::dec
                << std::endl;
      if (!game_title.empty()) {
        std::cout << "BOOT: Title Name: " << game_title << std::endl;
      }

      // Report some additional info
      auto kernel_state = emulator_->kernel_state();
      if (kernel_state) {
        std::cout << "BOOT: Kernel state initialized" << std::endl;
      }
    }

    // Signal the emulator thread event (for synchronization if needed)
    if (emulator_thread_event_) {
      emulator_thread_event_->Set();
    }
  });

  // Set up terminate callback
  emulator_->on_terminate.AddListener([this]() {
    std::cout << "BOOT: Title terminated" << std::endl;
  });

  // Set up exit callback
  emulator_->on_exit.AddListener([this]() {
    std::cout << "BOOT: Emulator exit requested" << std::endl;
  });
}

void EmulatorHeadless::ReportCrash(Emulator* emulator, uint32_t pc,
                                   cpu::ThreadState* thread_state) {
  std::cout << "CRASH: PC = 0x" << std::hex << pc << std::endl;

  if (thread_state && thread_state->context()) {
    auto ctx = thread_state->context();
    std::cout << "CRASH: Registers:" << std::endl;
    for (int i = 0; i < 32; i++) {
      std::cout << "  r" << std::dec << i << " = 0x" << std::hex << ctx->r[i]
                << std::dec;
      if (i % 4 == 3) {
        std::cout << std::endl;
      } else {
        std::cout << "  ";
      }
    }

    std::cout << "  lr = 0x" << std::hex << ctx->lr << std::dec << std::endl;
    std::cout << "  ctr = 0x" << std::hex << ctx->ctr << std::dec << std::endl;
    std::cout << "  cr = 0x" << std::hex << ctx->cr() << std::dec << std::endl;
    std::cout << "  xer(ca,ov,so) = (0x" << std::hex
              << (uint32_t)ctx->xer_ca << "," << (uint32_t)ctx->xer_ov << ","
              << (uint32_t)ctx->xer_so << ")" << std::dec << std::endl;
  }

  auto kernel_state = emulator->kernel_state();
  if (kernel_state) {
    std::cout << "CRASH: Title ID: 0x" << std::hex << emulator->title_id()
              << std::dec << std::endl;
  }
}

}  // namespace app
}  // namespace xe
