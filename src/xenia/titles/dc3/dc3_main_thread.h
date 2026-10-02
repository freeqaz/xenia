/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 guest main-thread hook (NOT upstream).
 *
 * One override of HolmesClientPollKeyboard (original debug.xex, 0x825F0F78),
 * called once per frame on the guest MAIN thread by KeyboardPoll <- SystemPoll
 * <- App::RunWithoutDebugging. Host code that must call guest functions (the
 * DTA channel, headless autonav) registers a task here instead of calling
 * Processor::Execute from some other guest thread: UI, ObjectDir and the
 * loaders are main-thread-only in the game (MILO_ASSERT(MainThread()) and the
 * CHECK_THREAD "called in the wrong thread" fails).
 *
 * The override is installed only when the first task is added, so a run with
 * no task keeps the stock body. With gHolmesStream == 0 (no Holmes host PC,
 * logged on the first poll) the stock body does nothing observable.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_MAIN_THREAD_H_
#define XENIA_TITLES_DC3_DC3_MAIN_THREAD_H_

#include <cstdint>

namespace xe {
class Memory;
namespace cpu {
class Processor;
class ThreadState;
}  // namespace cpu

namespace dc3 {

// A per-frame task. Runs on the guest main thread with that thread's
// ThreadState; the caller's PPCContext is saved before the tasks run and
// restored after, so a task may Execute guest functions freely.
using MainThreadTask = void (*)(cpu::ThreadState* thread_state,
                                uint64_t poll_index);

// Adds `task` (installing the override on first use). Returns false, and
// installs nothing, if the image is not the original debug.xex (fingerprint).
bool AddMainThreadTask(cpu::Processor* processor, Memory* memory,
                       const char* name, MainThreadTask task);

// The guest thread id of the first poll (0 before it).
uint32_t MainThreadId();

// True once the override is installed.
bool MainThreadHookInstalled();

// Main-loop passes seen so far: the poll count before the current frame's
// KeyboardPoll. SystemPoll runs JoypadPoll before KeyboardPoll, so during
// frame F's JoypadPoll this reads F (0 on the first frame), which is the
// native port's per-JoypadPoll frame counter (Joypad_Native.cpp).
uint64_t MainThreadFrame();

}  // namespace dc3
}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_MAIN_THREAD_H_
