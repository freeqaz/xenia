/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: where skeleton frames come from.
 *
 * A pose source is sampled by the device's frame clock (30 Hz on the guest
 * clock). It never touches guest memory and never runs guest code. Sources:
 *   empty      sensor present, nobody in view (every slot NOT_TRACKED)
 *   constant   one person standing still (synthetic_kinect.py STAND)
 * Phase 4 adds `tape:<path>` and `socket:<path>` (the native port's
 * DC3_POSE_SOCKET v2 protocol, decoded by DecodeV2Packet in nui_frame.h).
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_POSE_SOURCE_H_
#define XENIA_KERNEL_NUI_NUI_POSE_SOURCE_H_

#include <memory>
#include <string>

#include "xenia/kernel/nui/nui_frame.h"

namespace xe {
namespace kernel {
namespace nui {

class PoseSource {
 public:
  virtual ~PoseSource() = default;
  virtual bool Open() = 0;
  // The newest frame at or before sensor time `now_ms`. Returns false when
  // the sensor has no new depth frame (the device then raises no event).
  // The device stamps sensor_time_ms and frame_number itself unless the
  // source sets `out->sensor_time_ms` (a recorded or socket stream does).
  virtual bool Sample(uint64_t now_ms, PoseFrame* out) = 0;
  // Logged once: kind and parameters.
  virtual std::string Describe() const = 0;
};

// Sensor present, room empty: a frame (all NOT_TRACKED) every depth frame,
// as a real sensor raises the skeleton event whether or not anyone is in
// view.
std::unique_ptr<PoseSource> CreateEmptyPoseSource();

// One person (tracking id 5, the id synthetic_kinect.py gives its first
// person) standing 2.5 m from the sensor in synthetic_kinect.STAND, every
// joint TRACKED.
std::unique_ptr<PoseSource> CreateConstantPoseSource();

// "empty" | "constant". Unknown specs log an error and return null.
std::unique_ptr<PoseSource> CreatePoseSource(const std::string& spec);

// The joints of synthetic_kinect.STAND in NUI order, at depth `z`, x offset
// `x_offset`. Exposed for tests.
PosePerson StandingPerson(uint32_t tracking_id, float x_offset, float z);

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_POSE_SOURCE_H_
