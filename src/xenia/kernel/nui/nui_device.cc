/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: the sensor model behind the SDK facade.
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_device.h"

#include <chrono>
#include <cstring>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"

DEFINE_string(nui_pose_source, "",
              "Kinect (NUI) HLE: where skeleton frames come from while the "
              "sensor is present. 'empty' (nobody in view) or 'constant' (one "
              "person standing still, all joints tracked). Unset: 'empty'.",
              "Kernel");
DEFINE_string(nui_tracked_policy, "faithful",
              "Kinect (NUI) HLE: which people are TRACKED (full joints). "
              "'faithful': the SDK's rule (the ids the title passes to "
              "NuiSkeletonSetTrackedSkeletons when it asked to choose, else "
              "the two nearest). 'all': everyone present (native-port parity).",
              "Kernel");

namespace xe {
namespace kernel {
namespace nui {

namespace {
std::mutex g_device_lock;
std::shared_ptr<NuiDevice> g_device;
constexpr double kFramePeriodMs = 1000.0 / 30.0;
}  // namespace

std::shared_ptr<NuiDevice> NuiDevice::Get() {
  std::lock_guard<std::mutex> guard(g_device_lock);
  return g_device;
}

void NuiDevice::Create(KernelState* kernel_state) {
  std::lock_guard<std::mutex> guard(g_device_lock);
  g_device = std::make_shared<NuiDevice>(kernel_state);
}

void NuiDevice::Destroy() {
  std::shared_ptr<NuiDevice> device;
  {
    std::lock_guard<std::mutex> guard(g_device_lock);
    device = std::move(g_device);
  }
  if (device) {
    // Stops the clock and releases any guest thread waiting for a frame;
    // the last reference (possibly that thread's) frees the device.
    device->StopFrameClock();
  }
}

NuiDevice::NuiDevice(KernelState* kernel_state)
    : kernel_state_(kernel_state), memory_(kernel_state->memory()) {}

NuiDevice::~NuiDevice() { StopFrameClock(); }

NuiDevice::Counters NuiDevice::counters() {
  std::lock_guard<std::mutex> guard(lock_);
  return counters_;
}

uint32_t NuiDevice::Initialize(uint32_t flags) {
  std::unique_lock<std::mutex> guard(lock_);
  if (flags & 0x200) {
    // NuiInitialize: a reserved flag bit is E_INVALIDARG (nuiruntime.s).
    return kE_INVALIDARG;
  }
  if (initialized_) {
    XELOGW("NUI HLE: NuiInitialize({:08X}) while initialized; S_OK", flags);
    return kS_OK;
  }
  connected_ = cvars::nui_device_present;
  if (!connected_) {
    // What the SDK itself answers with the sensor unplugged (phase-0
    // probe); a Kinect-only title stops here, as on a console.
    XELOGW("NUI HLE: NuiInitialize({:08X}) with no sensor -> {:08X}", flags,
           kE_NUI_NO_SENSOR_AT_INIT);
    return kE_NUI_NO_SENSOR_AT_INIT;
  }
  initialized_ = true;
  init_flags_ = flags;
  policy_ = cvars::nui_tracked_policy == "all" ? TrackedPolicy::kAll
                                               : TrackedPolicy::kFaithful;
  if (cvars::nui_tracked_policy != "all" &&
      cvars::nui_tracked_policy != "faithful") {
    XELOGE("NUI HLE: unknown --nui_tracked_policy '{}'; using faithful",
           cvars::nui_tracked_policy);
  }
  const std::string spec =
      cvars::nui_pose_source.empty() ? "empty" : cvars::nui_pose_source;
  source_ = CreatePoseSource(spec);
  if (!source_) {
    // An unknown or not-yet-built source (tape:/socket: are phase 4): the
    // sensor still runs, with nobody in view.
    source_ = CreateEmptyPoseSource();
  } else if (!source_->Open()) {
    XELOGE("NUI HLE: pose source '{}' failed to open; nobody in view", spec);
    source_ = CreateEmptyPoseSource();
    source_->Open();
  }
  XELOGI("NUI HLE: NuiInitialize({:08X}) sensor={} pose source: {} "
         "tracked policy: {}",
         flags, connected_ ? "connected" : "NOT connected",
         source_ ? source_->Describe() : "none",
         policy_ == TrackedPolicy::kAll ? "all" : "faithful");
  guard.unlock();
  if (connected_) {
    StartFrameClock();
  }
  return kS_OK;
}

uint32_t NuiDevice::Shutdown() {
  StopFrameClock();
  std::lock_guard<std::mutex> guard(lock_);
  if (title_event_) {
    title_event_->Set(0, false);
    title_event_.reset();
  }
  tracking_enabled_ = false;
  initialized_ = false;
  XELOGI("NUI HLE: NuiShutdown (produced {} frames, served {})",
         counters_.frames_produced, counters_.frames_served);
  return kS_OK;
}

uint32_t NuiDevice::SkeletonTrackingEnable(uint32_t event_handle,
                                           uint32_t flags) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!initialized_) {
    return kE_NUI_FEATURE_NOT_INITIALIZED;
  }
  if (flags & ~uint32_t(7)) {
    return kE_INVALIDARG;
  }
  if (tracking_enabled_) {
    return kE_ALREADY_INITIALIZED;
  }
  object_ref<XEvent> ev;
  if (event_handle) {
    ev = kernel_state_->object_table()->LookupObject<XEvent>(event_handle);
    if (!ev) {
      // ObReferenceObjectByHandle(ExEventObjectType) failed.
      return kE_INVALIDARG;
    }
  }
  title_event_ = std::move(ev);
  tracking_enabled_ = true;
  tracking_flags_ = flags;
  selection_ = TrackingSelection();
  selection_.title_sets_tracked = (flags & kTrackingFlagTitleSetsTracked) != 0;
  slots_.Reset();
  slot_states_.fill(SkeletonState::kNotTracked);
  frame_ready_ = false;
  XELOGI("NUI HLE: NuiSkeletonTrackingEnable(event {:08X}, flags {:X}){}",
         event_handle, flags,
         selection_.title_sets_tracked ? " -- title sets tracked skeletons"
                                       : "");
  // NuipSetSkeletonFrameEvents(0): both events are raised at once unless the
  // title suppressed no-frame wakeups.
  if (!(flags & kTrackingFlagSuppressNoFrameData)) {
    internal_event_ = true;
    frame_cv_.notify_all();
    if (title_event_) {
      title_event_->Set(0, false);
    }
  }
  return kS_OK;
}

uint32_t NuiDevice::SkeletonTrackingDisable() {
  std::lock_guard<std::mutex> guard(lock_);
  if (tracking_enabled_) {
    tracking_enabled_ = false;
    selection_ = TrackingSelection();
    internal_event_ = true;
    frame_cv_.notify_all();
    if (title_event_) {
      title_event_->Set(0, false);
      title_event_.reset();
    }
    XELOGI("NUI HLE: NuiSkeletonTrackingDisable");
  }
  return kS_OK;
}

uint32_t NuiDevice::SkeletonSetTrackedSkeletons(uint32_t ids_ptr) {
  if (!ids_ptr) {
    return kE_INVALIDARG;
  }
  std::lock_guard<std::mutex> guard(lock_);
  if (!tracking_enabled_) {
    return kE_NUI_STREAM_NOT_ENABLED;
  }
  if (!selection_.title_sets_tracked) {
    return kE_NUI_FEATURE_NOT_INITIALIZED;
  }
  auto* ids = memory_->TranslateVirtual<xe::be<uint32_t>*>(ids_ptr);
  uint32_t a = ids[0], b = ids[1];
  if (a != selection_.title_ids[0] || b != selection_.title_ids[1]) {
    XELOGI("NUI HLE: NuiSkeletonSetTrackedSkeletons({}, {})",
           static_cast<int32_t>(a), static_cast<int32_t>(b));
  }
  selection_.title_ids[0] = a;
  selection_.title_ids[1] = b;
  return kS_OK;
}

uint32_t NuiDevice::SkeletonGetNextFrame(uint32_t timeout_ms,
                                         uint32_t frame_ptr) {
  if (!frame_ptr) {
    return kE_INVALIDARG;
  }
  std::unique_lock<std::mutex> guard(lock_);
  ++counters_.get_next_calls;
  if (!tracking_enabled_) {
    return kE_NUI_STREAM_NOT_ENABLED;
  }
  if (!connected_) {
    return kE_NUI_DEVICE_NOT_CONNECTED;
  }
  uint32_t wait_ms = timeout_ms;
  if (!(tracking_flags_ & kTrackingFlagSuppressNoFrameData) &&
      (timeout_ms == 0xFFFFFFFFu || timeout_ms > 8000)) {
    wait_ms = 8000;
  }
  // The SDK waits on its internal event with the (capped) timeout. A host
  // wait here blocks only the calling guest thread.
  auto ready = [this] {
    return internal_event_ || !tracking_enabled_ || clock_stop_.load();
  };
  if (!ready()) {
    if (wait_ms == 0) {
      ++counters_.pending;
      return kE_PENDING;
    }
    if (wait_ms == 0xFFFFFFFFu) {
      frame_cv_.wait(guard, ready);
    } else {
      // Guest milliseconds; the host wait is scaled like every guest wait.
      const auto host_ms = Clock::ScaleGuestDurationMillis(wait_ms);
      if (!frame_cv_.wait_for(guard, std::chrono::milliseconds(host_ms),
                              ready)) {
        ++counters_.pending;
        return kE_PENDING;
      }
    }
  }
  uint32_t result;
  if (frame_ready_) {
    std::memcpy(memory_->TranslateVirtual(frame_ptr), frame_.data(),
                kFrameSize);
    frame_ready_ = false;
    ++counters_.frames_served;
    result = kS_OK;
  } else if (!connected_ ||
             (tracking_flags_ & kTrackingFlagSuppressNoFrameData)) {
    result = kE_NUI_DEVICE_NOT_CONNECTED;
  } else {
    ++counters_.no_frame;
    result = kE_NUI_FRAME_NO_DATA;
  }
  // Every result but E_PENDING resets the title event and the internal one.
  internal_event_ = false;
  if (title_event_) {
    title_event_->Reset();
  }
  return result;
}

void NuiDevice::StartFrameClock() {
  if (clock_thread_.joinable()) {
    return;
  }
  clock_stop_ = false;
  clock_origin_ms_ = Clock::QueryGuestUptimeMillis();
  clock_thread_ = std::thread([this] { FrameClockThread(); });
}

void NuiDevice::StopFrameClock() {
  clock_stop_ = true;
  frame_cv_.notify_all();
  if (clock_thread_.joinable()) {
    clock_thread_.join();
  }
}

void NuiDevice::FrameClockThread() {
  xe::threading::set_name("NUI frame clock");
  double next_ms = static_cast<double>(Clock::QueryGuestUptimeMillis());
  auto last_report = std::chrono::steady_clock::now();
  Counters last{};
  while (!clock_stop_.load()) {
    const double now = static_cast<double>(Clock::QueryGuestUptimeMillis());
    if (now + 0.5 < next_ms) {
      const double wait = std::min(next_ms - now, 5.0);
      std::this_thread::sleep_for(
          std::chrono::microseconds(static_cast<int64_t>(wait * 1000.0)));
      continue;
    }
    ProduceFrame(static_cast<uint64_t>(next_ms));
    next_ms += kFramePeriodMs;
    if (now - next_ms > 10 * kFramePeriodMs) {
      next_ms = now;  // a stall (debugger, suspend): do not burst
    }
    auto host_now = std::chrono::steady_clock::now();
    if (host_now - last_report >= std::chrono::seconds(10)) {
      Counters c = counters();
      const double secs =
          std::chrono::duration<double>(host_now - last_report).count();
      XELOGI("NUI HLE: frame clock {:.1f}/s produced={} events={} "
             "get_next={} served={} no_frame={} pending={}",
             (c.frames_produced - last.frames_produced) / secs,
             c.frames_produced, c.events_set, c.get_next_calls,
             c.frames_served, c.no_frame, c.pending);
      last = c;
      last_report = host_now;
    }
  }
}

void NuiDevice::ProduceFrame(uint64_t now_ms) {
  PoseFrame frame;
  if (!source_ || !source_->Sample(now_ms - clock_origin_ms_, &frame)) {
    return;
  }
  std::lock_guard<std::mutex> guard(lock_);
  if (!tracking_enabled_) {
    return;
  }
  ++frame_number_;
  if (!frame.sensor_time_ms) {
    frame.sensor_time_ms = now_ms - clock_origin_ms_;
  }
  if (!frame.frame_number) {
    frame.frame_number = frame_number_;
  }
  std::array<int, kSkeletonCount> person_for_slot;
  slots_.Assign(frame.persons, &person_for_slot);
  DecideSkeletonStates(policy_, selection_, frame, person_for_slot,
                       &slot_states_);
  EncodeSkeletonFrame(frame, person_for_slot, slot_states_, frame_.data());
  frame_ready_ = true;
  internal_event_ = true;
  ++counters_.frames_produced;
  frame_cv_.notify_all();
  if (title_event_) {
    title_event_->Set(0, false);
    ++counters_.events_set;
  }
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
