/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: pose sources.
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_pose_source.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace nui {

namespace {

// synthetic_kinect.py STAND (NativeSkeletonProvider::FillDummySkeleton's
// neutral pose), DC3 game joint order, (x, y).
constexpr float kStandDc3[kJointCount][2] = {
    {0.00f, 0.90f},  {0.00f, 1.10f},  {0.00f, 1.40f},  {0.00f, 1.60f},
    {-0.20f, 1.40f}, {-0.25f, 1.15f}, {-0.22f, 0.90f}, {-0.22f, 0.85f},
    {0.20f, 1.40f},  {0.25f, 1.15f},  {0.22f, 0.90f},  {0.22f, 0.85f},
    {-0.12f, 0.85f}, {-0.12f, 0.45f}, {-0.12f, 0.05f}, {0.12f, 0.85f},
    {0.12f, 0.45f},  {0.12f, 0.05f},  {-0.12f, 0.00f}, {0.12f, 0.00f},
};

class EmptyPoseSource : public PoseSource {
 public:
  bool Open() override { return true; }
  bool Sample(uint64_t, PoseFrame* out) override {
    out->persons.clear();
    return true;
  }
  std::string Describe() const override { return "empty (nobody in view)"; }
};

class ConstantPoseSource : public PoseSource {
 public:
  bool Open() override {
    person_ = StandingPerson(5, 0.0f, 2.5f);
    return true;
  }
  bool Sample(uint64_t, PoseFrame* out) override {
    out->persons.assign(1, person_);
    return true;
  }
  std::string Describe() const override {
    return "constant (one person, id 5, standing at 2.5 m, all joints "
           "TRACKED)";
  }

 private:
  PosePerson person_;
};

}  // namespace

PosePerson StandingPerson(uint32_t tracking_id, float x_offset, float z) {
  PosePerson p;
  p.tracking_id = tracking_id;
  for (uint32_t n = 0; n < kJointCount; ++n) {
    const float* xy = kStandDc3[kNuiFromDc3[n]];
    p.joints[n].x = xy[0] + x_offset;
    p.joints[n].y = xy[1];
    p.joints[n].z = z;
    p.joints[n].state = JointState::kTracked;
  }
  return p;
}

std::unique_ptr<PoseSource> CreateEmptyPoseSource() {
  return std::make_unique<EmptyPoseSource>();
}

std::unique_ptr<PoseSource> CreateConstantPoseSource() {
  return std::make_unique<ConstantPoseSource>();
}

std::unique_ptr<PoseSource> CreatePoseSource(const std::string& spec) {
  if (spec == "empty") {
    return CreateEmptyPoseSource();
  }
  if (spec == "constant") {
    return CreateConstantPoseSource();
  }
  XELOGE("NUI HLE: unknown pose source '{}' (empty|constant)", spec);
  return nullptr;
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
