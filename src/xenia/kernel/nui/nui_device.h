/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: the sensor model behind the SDK facade.
 *
 * One device per running title. The skeleton stream follows the SDK's own
 * semantics (NuiSkeletonTrackingEnable / GetNextFrame / SetTrackedSkeletons
 * in nuiskeleton.cpp of NUI 2.0.21173; docs/fork/nui/NUI_DEVICE_SPEC.md):
 * a host frame clock (30 Hz on the guest clock) samples the pose source,
 * encodes a NUI_SKELETON_FRAME into the host-side ring and signals the
 * title's event. It never runs guest code and never writes guest memory; the
 * title's own worker copies the frame out through the GetNextFrame override,
 * on its own guest thread.
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_DEVICE_H_
#define XENIA_KERNEL_NUI_NUI_DEVICE_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/kernel/nui/nui_frame.h"
#include "xenia/kernel/nui/nui_pose_source.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xevent.h"

DECLARE_bool(nui_device_present);
DECLARE_string(nui_pose_source);
DECLARE_string(nui_tracked_policy);

namespace xe {
class Memory;
namespace kernel {
class KernelState;
namespace nui {

// HRESULTs the SDK returns (nuiskeleton.s, nuiruntime.s; winerror.h).
constexpr uint32_t kS_OK = 0;
constexpr uint32_t kE_INVALIDARG = 0x80070057;
constexpr uint32_t kE_PENDING = 0x8000000A;
constexpr uint32_t kE_NUI_DEVICE_NOT_CONNECTED = 0x8007048F;
constexpr uint32_t kE_ALREADY_INITIALIZED = 0x800704DF;
constexpr uint32_t kE_NUI_FRAME_NO_DATA = 0x83010001;
constexpr uint32_t kE_NUI_STREAM_NOT_ENABLED = 0x83010002;
constexpr uint32_t kE_NUI_FEATURE_NOT_INITIALIZED = 0x83010005;
constexpr uint32_t kE_NUI_SYSTEM_UI_PRESENT = 0x8301000B;
// NuiInitialize with no sensor attached. Measured, not read: the real SDK
// running on Xenia with XamNuiGetDeviceStatus reporting "not connected"
// returns it (docs/fork/nui/NUI_DEVICE_SPEC.md, the phase-0 probe).
constexpr uint32_t kE_NUI_NO_SENSOR_AT_INIT = 0x8301000D;

// NUI_SKELETON_TRACKING_FLAG_*.
constexpr uint32_t kTrackingFlagSuppressNoFrameData = 0x1;
constexpr uint32_t kTrackingFlagTitleSetsTracked = 0x2;
constexpr uint32_t kTrackingFlagEnableSeated = 0x4;

class NuiDevice {
 public:
  explicit NuiDevice(KernelState* kernel_state);
  ~NuiDevice();

  // The device for the running title, or null when the facade is not
  // installed (no supported NUI SDK in the image). Shared: a guest thread
  // blocked in GetNextFrame keeps it alive across Destroy().
  static std::shared_ptr<NuiDevice> Get();
  static void Create(KernelState* kernel_state);
  static void Destroy();

  // The sensor is plugged in (--nui_device_present; profile-set for Kinect
  // titles). Read at NuiInitialize.
  bool connected() const { return connected_; }

  // NUI SDK API semantics (return HRESULTs).
  uint32_t Initialize(uint32_t flags);
  uint32_t Shutdown();
  uint32_t SkeletonTrackingEnable(uint32_t event_handle, uint32_t flags);
  uint32_t SkeletonTrackingDisable();
  uint32_t SkeletonSetTrackedSkeletons(uint32_t ids_ptr);
  uint32_t SkeletonGetNextFrame(uint32_t timeout_ms, uint32_t frame_ptr);

  struct Counters {
    uint64_t frames_produced = 0;
    uint64_t events_set = 0;
    uint64_t get_next_calls = 0;
    uint64_t frames_served = 0;
    uint64_t no_frame = 0;
    uint64_t pending = 0;
  };
  Counters counters();

 private:
  void StartFrameClock();
  void StopFrameClock();
  void FrameClockThread();
  void ProduceFrame(uint64_t now_ms);

  KernelState* kernel_state_;
  Memory* memory_;

  std::mutex lock_;  // mirrors NuipSkeletonLock
  std::condition_variable frame_cv_;
  bool initialized_ = false;
  bool connected_ = false;
  uint32_t init_flags_ = 0;

  // Skeleton stream.
  bool tracking_enabled_ = false;
  uint32_t tracking_flags_ = 0;
  object_ref<XEvent> title_event_;
  TrackingSelection selection_;
  TrackedPolicy policy_ = TrackedPolicy::kFaithful;
  SlotMap slots_;
  std::array<SkeletonState, kSkeletonCount> slot_states_{};
  // The SDK keeps two ring entries and publishes one per depth frame; the
  // title always reads the newest. One published entry is equivalent.
  bool frame_ready_ = false;      // ring entry status 1 ("new")
  bool internal_event_ = false;   // the SDK's internal KEVENT (+0x878)
  std::array<uint8_t, kFrameSize> frame_{};
  uint32_t frame_number_ = 0;
  uint64_t clock_origin_ms_ = 0;

  std::unique_ptr<PoseSource> source_;
  std::thread clock_thread_;
  std::atomic<bool> clock_stop_{false};

  Counters counters_;
};

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_DEVICE_H_
