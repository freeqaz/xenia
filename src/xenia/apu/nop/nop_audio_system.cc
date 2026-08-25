/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/nop/nop_audio_system.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace nop {

// A do-nothing driver that still CONSUMES: it discards submitted frames but
// releases the client semaphore at real hardware cadence (one 256-sample
// frame per channel at 48 kHz = 5.333 ms). Without this, CreateDriver
// returned NOT_IMPLEMENTED, XAudioRegisterRenderDriverClient handed the
// guest a dummy handle, the AudioSystem worker never pumped the guest's
// render callback, and titles that clock gameplay off the audio stream
// position (RB3: song time, gem scroll, score) sat at 0:00 forever with the
// game_screen otherwise healthy.
class NopAudioDriver : public AudioDriver {
 public:
  NopAudioDriver(Memory* memory, xe::threading::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {
    pacer_ = std::thread([this]() {
      xe::threading::set_name("Nop Audio Pacer");
      constexpr auto kFramePeriod = std::chrono::microseconds(5333);
      auto next = std::chrono::steady_clock::now();
      while (running_.load(std::memory_order_relaxed)) {
        next += kFramePeriod;
        std::this_thread::sleep_until(next);
        int pending = pending_.load(std::memory_order_relaxed);
        while (pending > 0) {
          if (pending_.compare_exchange_weak(pending, pending - 1,
                                             std::memory_order_relaxed)) {
            semaphore_->Release(1, nullptr);
            break;
          }
        }
      }
    });
  }

  ~NopAudioDriver() override {
    running_.store(false, std::memory_order_relaxed);
    if (pacer_.joinable()) {
      pacer_.join();
    }
  }

  void SubmitFrame(uint32_t frame_ptr) override {
    // Discard the samples; just account for the frame so the pacer thread
    // releases the semaphore for it on the next tick.
    pending_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  xe::threading::Semaphore* semaphore_;
  std::atomic<bool> running_{true};
  std::atomic<int> pending_{0};
  std::thread pacer_;
};

std::unique_ptr<AudioSystem> NopAudioSystem::Create(cpu::Processor* processor) {
  return std::make_unique<NopAudioSystem>(processor);
}

NopAudioSystem::NopAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t index,
                                      xe::threading::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  assert_not_null(out_driver);
  *out_driver = new NopAudioDriver(memory_, semaphore);
  return X_STATUS_SUCCESS;
}

void NopAudioSystem::DestroyDriver(AudioDriver* driver) { delete driver; }

}  // namespace nop
}  // namespace apu
}  // namespace xe
