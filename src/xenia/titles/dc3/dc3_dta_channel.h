/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_DC3_DTA_CHANNEL_H_
#define XENIA_DC3_DTA_CHANNEL_H_

#include <string>

namespace xe {

class Memory;

namespace cpu {
class Processor;
}

// DC3 (373307D9, ORIGINAL debug.xex layout only) DTA evaluation channel.
//
// A host thread serves a unix socket (--dc3_dta_channel=<path>). Each request
// body is DTA text; it is queued and evaluated ON THE GUEST MAIN THREAD, from
// an override of HolmesClientPollKeyboard (called once per frame by
// KeyboardPoll <- SystemPoll <- App::RunWithoutDebugging). Evaluation uses the
// game's own DataReadString + DataArray::Execute. The reply follows the
// RB3Enhanced /dta/eval body contract that tools/console/dc3_eval.py parses:
// one "=> <value>" line per top-level command, "=> !! refused: <why>" for a
// command that failed, and a body of exactly "!! parse error" when the text
// does not parse.
//
// Wire (both directions little-endian):
//   request : u32 len, len bytes of DTA text
//   response: u32 status (200 ok, 413 too large, 503 not ready, 504 timeout),
//             u32 len, len bytes of body
//
// Returns false (and logs) if the channel could not be installed.
bool Dc3DtaChannelInstall(cpu::Processor* processor, Memory* memory,
                          const std::string& socket_path);

// Stops and joins the socket server thread (title terminate / shutdown).
void Dc3DtaChannelShutdown();

}  // namespace xe

#endif  // XENIA_DC3_DTA_CHANNEL_H_
