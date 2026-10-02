/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026.                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_DEBUG_GDB_RSP_GDB_RSP_SERVER_H_
#define XENIA_DEBUG_GDB_RSP_GDB_RSP_SERVER_H_

#include <cstdint>
#include <memory>

#include "xenia/base/cvar.h"
#include "xenia/cpu/debug_listener.h"

DECLARE_bool(gdb_rsp_stub);
DECLARE_int32(gdb_rsp_prelaunch_sleep_ms);

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
namespace debug {
namespace gdb_rsp {

// An in-process GDB remote-serial-protocol server (PowerPC target, guest
// addresses): memory reads, registers, pause/continue/step and guest
// breakpoints through the processor's debugger API. Linux only.
//
class GdbRspServer;
struct GdbRspServerDeleter {
  void operator()(GdbRspServer* server) const;
};
using GdbRspServerPtr = std::unique_ptr<GdbRspServer, GdbRspServerDeleter>;

// Returns the server, listening, if --gdb_rsp_stub (or its deprecated alias
// --dc3_gdb_rsp_stub) is set; the caller installs AsDebugListener(server)
// with Processor::set_debug_listener and owns the server. nullptr if
// disabled.
GdbRspServerPtr CreateServerIfEnabled(cpu::Processor* processor);
cpu::DebugListener* AsDebugListener(GdbRspServer* server);

// --gdb_rsp_prelaunch_sleep_ms (or the deprecated
// --dc3_gdb_rsp_prelaunch_sleep_ms): how long to wait before launching the
// title, so a debugger can attach first.
int32_t PrelaunchSleepMs();

}  // namespace gdb_rsp
}  // namespace debug
}  // namespace xe

#endif  // XENIA_DEBUG_GDB_RSP_GDB_RSP_SERVER_H_
