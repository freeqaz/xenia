/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: the canonical pose and the guest skeleton frame.
 ******************************************************************************
 */

#include "xenia/kernel/nui/nui_frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace nui {

const std::array<uint8_t, kJointCount> kNuiFromDc3 = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 18, 15, 16, 17, 19};

JointState JointStateFromConfidence(float confidence) {
  if (!(confidence >= 0.3f)) {
    return JointState::kNotTracked;
  }
  if (confidence < 0.6f) {
    return JointState::kInferred;
  }
  return JointState::kTracked;
}

namespace {

uint32_t ReadLE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);  // the wire is little-endian, as is every host
  return v;
}
float ReadLEFloat(const uint8_t* p) {
  float v;
  std::memcpy(&v, p, 4);
  return v;
}

}  // namespace

bool DecodeV2Packet(const uint8_t* body, size_t size, PoseFrame* out,
                    std::string* error) {
  auto fail = [&](std::string msg) {
    if (error) {
      *error = std::move(msg);
    }
    return false;
  };
  if (size < kPoseV2HeaderSize) {
    return fail(fmt::format("packet of {} bytes is shorter than the v2 header",
                            size));
  }
  if (ReadLE32(body) != kPoseProtocolMagic) {
    return fail("not a v2 packet (magic)");
  }
  const uint32_t frame_id = ReadLE32(body + 4);
  const uint32_t num_persons = ReadLE32(body + 8);
  double timestamp_s;
  std::memcpy(&timestamp_s, body + 12, 8);
  const uint8_t num_landmarks = body[24];
  const auto layout = static_cast<WireLayout>(body[25]);
  if (layout != WireLayout::kDc3_20 && layout != WireLayout::kNui20) {
    return fail(fmt::format("layout {} carries no camera-space joints",
                            static_cast<int>(layout)));
  }
  if (num_landmarks != kJointCount) {
    return fail(fmt::format("layout {} with {} landmarks (want 20)",
                            static_cast<int>(layout), num_landmarks));
  }
  const size_t person_size = 4 + size_t(num_landmarks) * 16;
  if (num_persons > 64 ||
      size < kPoseV2HeaderSize + size_t(num_persons) * person_size) {
    return fail(fmt::format("{} persons do not fit in {} bytes", num_persons,
                            size));
  }
  out->frame_number = frame_id;
  out->sensor_time_ms =
      timestamp_s > 0.0 ? static_cast<uint64_t>(std::llround(timestamp_s * 1000.0))
                        : 0;
  out->persons.clear();
  const uint8_t* p = body + kPoseV2HeaderSize;
  for (uint32_t i = 0; i < num_persons; ++i, p += person_size) {
    const int32_t track_id = static_cast<int32_t>(ReadLE32(p));
    if (track_id <= 0 || out->persons.size() >= kSkeletonCount) {
      continue;
    }
    PosePerson person;
    person.tracking_id = static_cast<uint32_t>(track_id);
    for (uint32_t n = 0; n < kJointCount; ++n) {
      const uint32_t src = layout == WireLayout::kDc3_20 ? kNuiFromDc3[n] : n;
      const uint8_t* j = p + 4 + src * 16;
      person.joints[n].x = ReadLEFloat(j + 0);
      person.joints[n].y = ReadLEFloat(j + 4);
      person.joints[n].z = ReadLEFloat(j + 8);
      person.joints[n].state = JointStateFromConfidence(ReadLEFloat(j + 12));
    }
    out->persons.push_back(person);
  }
  return true;
}

void SlotMap::Assign(const std::vector<PosePerson>& persons,
                     std::array<int, kSkeletonCount>* person_for_slot) {
  person_for_slot->fill(-1);
  const size_t n = std::min<size_t>(persons.size(), kSkeletonCount);
  std::array<bool, kSkeletonCount> placed{};
  // 1. Persisting ids keep their slot.
  for (size_t k = 0; k < n; ++k) {
    for (uint32_t s = 0; s < kSkeletonCount; ++s) {
      if (slot_id_[s] && slot_id_[s] == persons[k].tracking_id) {
        (*person_for_slot)[s] = static_cast<int>(k);
        placed[k] = true;
        break;
      }
    }
  }
  // 2. Release slots whose owner left.
  for (uint32_t s = 0; s < kSkeletonCount; ++s) {
    if (slot_id_[s] && (*person_for_slot)[s] < 0) {
      slot_id_[s] = 0;
    }
  }
  // 3. New ids take the lowest free slot.
  for (size_t k = 0; k < n; ++k) {
    if (placed[k] || !persons[k].tracking_id) {
      continue;
    }
    for (uint32_t s = 0; s < kSkeletonCount; ++s) {
      if (!slot_id_[s] && (*person_for_slot)[s] < 0) {
        slot_id_[s] = persons[k].tracking_id;
        (*person_for_slot)[s] = static_cast<int>(k);
        break;
      }
    }
  }
}

void DecideSkeletonStates(TrackedPolicy policy,
                          const TrackingSelection& selection,
                          const PoseFrame& frame,
                          const std::array<int, kSkeletonCount>& person_for_slot,
                          std::array<SkeletonState, kSkeletonCount>* states) {
  std::array<SkeletonState, kSkeletonCount> previous = *states;
  std::array<SkeletonState, kSkeletonCount> next;
  next.fill(SkeletonState::kNotTracked);
  for (uint32_t s = 0; s < kSkeletonCount; ++s) {
    if (person_for_slot[s] >= 0) {
      next[s] = SkeletonState::kPositionOnly;
    }
  }
  if (policy == TrackedPolicy::kAll) {
    for (auto& st : next) {
      if (st == SkeletonState::kPositionOnly) {
        st = SkeletonState::kTracked;
      }
    }
  } else if (selection.title_sets_tracked) {
    for (uint32_t s = 0; s < kSkeletonCount; ++s) {
      if (person_for_slot[s] < 0) {
        continue;
      }
      const uint32_t id = frame.persons[person_for_slot[s]].tracking_id;
      if (id && (id == selection.title_ids[0] || id == selection.title_ids[1])) {
        next[s] = SkeletonState::kTracked;
      }
    }
  } else {
    // NuipNuiHandsDefaultTrackingPolicy: the two bodies nearest the sensor,
    // |hip| with 0.3 m off for a body that is already tracked.
    std::array<std::pair<float, uint32_t>, kSkeletonCount> order;
    size_t count = 0;
    for (uint32_t s = 0; s < kSkeletonCount; ++s) {
      if (person_for_slot[s] < 0) {
        continue;
      }
      const auto& hip = frame.persons[person_for_slot[s]].joints[0];
      float d = std::sqrt(hip.x * hip.x + hip.y * hip.y + hip.z * hip.z);
      if (previous[s] == SkeletonState::kTracked) {
        d -= 0.3f;
      }
      order[count++] = {d, s};
    }
    std::sort(order.begin(), order.begin() + count);
    for (size_t i = 0; i < count && i < 2; ++i) {
      next[order[i].second] = SkeletonState::kTracked;
    }
  }
  *states = next;
}

void EncodeSkeletonFrame(const PoseFrame& frame,
                         const std::array<int, kSkeletonCount>& person_for_slot,
                         const std::array<SkeletonState, kSkeletonCount>& states,
                         uint8_t* out) {
  std::memset(out, 0, kFrameSize);
  auto w32 = [out](uint32_t off, uint32_t v) {
    xe::store_and_swap<uint32_t>(out + off, v);
  };
  auto wf = [out](uint32_t off, float v) {
    xe::store_and_swap<float>(out + off, v);
  };
  xe::store_and_swap<uint64_t>(out + 0x00, frame.sensor_time_ms);
  w32(0x08, frame.frame_number);
  w32(0x0C, 0);  // dwFlags
  for (int i = 0; i < 4; ++i) {
    wf(0x10 + i * 4, frame.floor_clip_plane[i]);
    wf(0x20 + i * 4, frame.normal_to_gravity[i]);
  }
  for (uint32_t s = 0; s < kSkeletonCount; ++s) {
    if (person_for_slot[s] < 0 || states[s] == SkeletonState::kNotTracked) {
      continue;
    }
    const PosePerson& p = frame.persons[person_for_slot[s]];
    const uint32_t base = kSkeletonBase + s * kSkeletonStride;
    w32(base + kSkelTrackingState, static_cast<uint32_t>(states[s]));
    w32(base + kSkelTrackingId, p.tracking_id);
    // dwEnrollmentIndex and dwUserIndex stay 0: NuipConvertSTSkeletons
    // zeroes the array and never writes them.
    const PoseJoint& hip = p.joints[0];
    wf(base + kSkelPosition + 0, hip.x);
    wf(base + kSkelPosition + 4, hip.y);
    wf(base + kSkelPosition + 8, hip.z);
    wf(base + kSkelPosition + 12, 1.0f);
    if (states[s] != SkeletonState::kTracked) {
      continue;
    }
    for (uint32_t n = 0; n < kJointCount; ++n) {
      const PoseJoint& j = p.joints[n];
      const uint32_t off = base + kSkelJoints + n * 16;
      wf(off + 0, j.x);
      wf(off + 4, j.y);
      wf(off + 8, j.z);
      wf(off + 12, 1.0f);
      w32(base + kSkelJointStates + n * 4, static_cast<uint32_t>(j.state));
    }
    w32(base + kSkelQualityFlags, p.quality_flags);
  }
}

}  // namespace nui
}  // namespace kernel
}  // namespace xe
