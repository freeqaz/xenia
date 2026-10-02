/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: the canonical pose and the guest skeleton frame.
 * See docs/fork/nui/NUI_HLE_DESIGN.md (sections 2.3, 2.5, 3.3.2) and
 * docs/fork/nui/NUI_DEVICE_SPEC.md. Title-agnostic: no title id, no address.
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_NUI_FRAME_H_
#define XENIA_KERNEL_NUI_NUI_FRAME_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace nui {

// NUI_SKELETON_FRAME, big-endian in guest memory (nuiskeleton.h; the SDK
// copies exactly 0xAB0 bytes, NuiSkeletonGetNextFrame).
constexpr uint32_t kFrameSize = 0xAB0;
constexpr uint32_t kSkeletonCount = 6;
constexpr uint32_t kSkeletonBase = 0x30;
constexpr uint32_t kSkeletonStride = 0x1C0;
constexpr uint32_t kJointCount = 20;
// Offsets inside NUI_SKELETON_DATA.
constexpr uint32_t kSkelTrackingState = 0x000;
constexpr uint32_t kSkelTrackingId = 0x004;
constexpr uint32_t kSkelEnrollmentIndex = 0x008;
constexpr uint32_t kSkelUserIndex = 0x00C;
constexpr uint32_t kSkelPosition = 0x010;
constexpr uint32_t kSkelJoints = 0x020;
constexpr uint32_t kSkelJointStates = 0x160;
constexpr uint32_t kSkelQualityFlags = 0x1B0;
static_assert(kSkeletonBase + kSkeletonCount * kSkeletonStride == kFrameSize,
              "NUI_SKELETON_FRAME is 0xAB0 bytes");
static_assert(kSkelJoints + kJointCount * 16 == kSkelJointStates,
              "20 vec4 joints precede the joint states");

// NUI_SKELETON_TRACKING_STATE.
enum class SkeletonState : uint32_t {
  kNotTracked = 0,
  kPositionOnly = 1,
  kTracked = 2,
};

// NUI_SKELETON_POSITION_TRACKING_STATE.
enum class JointState : uint8_t {
  kNotTracked = 0,
  kInferred = 1,
  kTracked = 2,
};

// Canonical pose: NUI joint order (HIP_CENTER ... FOOT_RIGHT), camera space,
// metres, right-handed, +y up.
struct PoseJoint {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  JointState state = JointState::kNotTracked;
};

struct PosePerson {
  // > 0 and stable while the person persists (the SDK never reports 0 for a
  // detected body; titles test > 0).
  uint32_t tracking_id = 0;
  std::array<PoseJoint, kJointCount> joints{};
  uint32_t quality_flags = 0;  // NUI_SKELETON_QUALITY_CLIPPED_* bits
};

struct PoseFrame {
  // Sensor time in milliseconds (liTimeStamp). Titles take the delta of two
  // frames as elapsed ms.
  uint64_t sensor_time_ms = 0;
  uint32_t frame_number = 0;
  float floor_clip_plane[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  float normal_to_gravity[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  std::vector<PosePerson> persons;  // at most kSkeletonCount used
};

// Wire layouts of the DC3_POSE_SOCKET v2 protocol (dc3-decomp
// native/src/platform/Skeleton_Native.cpp, scripts/synthetic_kinect.py).
// kNui20 is new: NUI order, so a non-DC3 producer needs no permutation.
enum class WireLayout : uint8_t {
  kCoco17 = 0,
  kDc3_20 = 1,
  kNui20 = 2,
};
constexpr uint32_t kPoseProtocolMagic = 0x44503302;
constexpr size_t kPoseV2HeaderSize = 28;

// nui[n] = dc3[kNuiFromDc3[n]]: DC3's game joint order differs from NUI's
// only in the leg block (sJointRemap, Skeleton.cpp): NUI 15 FOOT_LEFT is game
// 18, NUI 16-18 (HIP/KNEE/ANKLE_RIGHT) are game 15-17.
extern const std::array<uint8_t, kJointCount> kNuiFromDc3;

// Per-joint confidence -> state, the thresholds the native port uses
// (Skeleton_Native.cpp ConfidenceFromScore).
JointState JointStateFromConfidence(float confidence);

// Decodes one v2 packet body (after the u32 length prefix). Layouts kDc3_20
// and kNui20 only (COCO17 carries no depth). Persons with a track id <= 0 are
// dropped (Kinect ids are > 0). timestamp seconds -> sensor_time_ms.
bool DecodeV2Packet(const uint8_t* body, size_t size, PoseFrame* out,
                    std::string* error);

// Stable person -> skeleton slot assignment, the native port's AssignSlots
// policy (GestureMgr_Native.cpp): a persisting id keeps its slot, a vanished
// id frees it, a new id takes the lowest free slot.
class SlotMap {
 public:
  // person_for_slot[s] = index into `persons`, or -1.
  void Assign(const std::vector<PosePerson>& persons,
              std::array<int, kSkeletonCount>* person_for_slot);
  void Reset() { slot_id_.fill(0); }
  uint32_t SlotTrackingId(uint32_t slot) const { return slot_id_[slot]; }

 private:
  std::array<uint32_t, kSkeletonCount> slot_id_{};
};

// Who is TRACKED (full joints) vs POSITION_ONLY (state, id and hip only).
enum class TrackedPolicy {
  // The SDK's: with NUI_SKELETON_TRACKING_FLAG_TITLE_SETS_TRACKED_SKELETONS
  // only the ids the title passed to NuiSkeletonSetTrackedSkeletons (none
  // until it does: NuiSkeletonTrackingEnable clears them); without it the
  // two nearest bodies, preferring those already tracked
  // (NuipNuiHandsDefaultTrackingPolicy).
  kFaithful,
  // Every person present is TRACKED (native-port parity).
  kAll,
};

struct TrackingSelection {
  bool title_sets_tracked = false;
  uint32_t title_ids[2] = {0, 0};
};

// Per-slot skeleton states for this frame. `previous` is last frame's result
// (for the default policy's hysteresis); it is updated in place.
void DecideSkeletonStates(TrackedPolicy policy,
                          const TrackingSelection& selection,
                          const PoseFrame& frame,
                          const std::array<int, kSkeletonCount>& person_for_slot,
                          std::array<SkeletonState, kSkeletonCount>* states);

// Writes the 0xAB0-byte big-endian NUI_SKELETON_FRAME. POSITION_ONLY slots
// carry state, id and hip position only; joints and joint states stay zero,
// as NuipConvertSTSkeletons leaves them.
void EncodeSkeletonFrame(const PoseFrame& frame,
                         const std::array<int, kSkeletonCount>& person_for_slot,
                         const std::array<SkeletonState, kSkeletonCount>& states,
                         uint8_t* out);

}  // namespace nui
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_NUI_FRAME_H_
