/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Kinect (NUI) HLE device: host unit tests.
 ******************************************************************************
 */

#include <cstring>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#include "xenia/base/memory.h"
#include "xenia/kernel/nui/nui_frame.h"
#include "xenia/kernel/nui/nui_pose_source.h"
#include "xenia/kernel/nui/nui_signature.h"

namespace xe::kernel::nui::test {
namespace {

uint32_t BE32(const uint8_t* frame, uint32_t off) {
  return xe::load_and_swap<uint32_t>(frame + off);
}
float BEF(const uint8_t* frame, uint32_t off) {
  return xe::load_and_swap<float>(frame + off);
}

void PutLE32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void PutLEF(std::vector<uint8_t>& v, float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  PutLE32(v, x);
}

// One DC3_20 v2 body, built the way synthetic_kinect.py _packet() does.
std::vector<uint8_t> Dc3Packet(uint32_t frame_id, double ts,
                               const std::vector<int32_t>& ids) {
  std::vector<uint8_t> b;
  PutLE32(b, kPoseProtocolMagic);
  PutLE32(b, frame_id);
  PutLE32(b, uint32_t(ids.size()));
  uint8_t d[8];
  std::memcpy(d, &ts, 8);
  b.insert(b.end(), d, d + 8);
  b.push_back(640 & 0xFF), b.push_back(640 >> 8);
  b.push_back(480 & 0xFF), b.push_back(480 >> 8);
  b.push_back(20);
  b.push_back(uint8_t(WireLayout::kDc3_20));
  b.push_back(0), b.push_back(0);
  for (int32_t id : ids) {
    PutLE32(b, uint32_t(id));
    for (int j = 0; j < 20; ++j) {
      PutLEF(b, float(j));           // x = game joint index
      PutLEF(b, 1.0f + j * 0.01f);   // y
      PutLEF(b, 2.5f);               // z
      PutLEF(b, j == 7 ? 0.5f : (j == 8 ? 0.1f : 1.0f));  // conf
    }
  }
  return b;
}

std::vector<PosePerson> Persons(const std::vector<uint32_t>& ids) {
  std::vector<PosePerson> v;
  for (uint32_t id : ids) {
    PosePerson p = StandingPerson(id, 0.0f, 2.5f);
    v.push_back(p);
  }
  return v;
}

}  // namespace

TEST_CASE("NUI frame layout constants", "[nui]") {
  REQUIRE(kFrameSize == 0xAB0);
  REQUIRE(kSkeletonStride == 0x1C0);
  REQUIRE(kSkeletonBase + 6 * kSkeletonStride == 0xAB0);
}

TEST_CASE("EncodeSkeletonFrame writes a big-endian 0xAB0 frame", "[nui]") {
  PoseFrame f;
  f.sensor_time_ms = 1234;
  f.frame_number = 37;
  f.persons = Persons({5, 9});
  std::array<int, kSkeletonCount> slots = {-1, 0, -1, 1, -1, -1};
  std::array<SkeletonState, kSkeletonCount> states;
  states.fill(SkeletonState::kNotTracked);
  states[1] = SkeletonState::kTracked;
  states[3] = SkeletonState::kPositionOnly;
  std::vector<uint8_t> out(kFrameSize + 16, 0xCD);
  EncodeSkeletonFrame(f, slots, states, out.data());
  // Exactly 0xAB0 bytes written.
  REQUIRE(out[kFrameSize] == 0xCD);
  REQUIRE(xe::load_and_swap<uint64_t>(out.data()) == 1234);
  REQUIRE(BE32(out.data(), 0x08) == 37);
  REQUIRE(BEF(out.data(), 0x14) == 1.0f);  // floor plane B
  REQUIRE(BEF(out.data(), 0x24) == 1.0f);  // normal to gravity y
  // Slot 0: empty.
  REQUIRE(BE32(out.data(), 0x30) == 0);
  // Slot 1 (0x30 + 0x1C0): TRACKED, id 5, joints + states + w = 1.
  const uint32_t s1 = 0x30 + 0x1C0;
  REQUIRE(BE32(out.data(), s1 + 0x0) == 2);
  REQUIRE(BE32(out.data(), s1 + 0x4) == 5);
  REQUIRE(BE32(out.data(), s1 + 0x8) == 0);  // enrollment index
  REQUIRE(BE32(out.data(), s1 + 0xC) == 0);  // user index
  REQUIRE(BEF(out.data(), s1 + 0x14) == 0.90f);  // hip y = Position y
  REQUIRE(BEF(out.data(), s1 + 0x1C) == 1.0f);
  REQUIRE(BEF(out.data(), s1 + 0x20 + 3 * 16 + 4) == 1.60f);  // head y
  REQUIRE(BEF(out.data(), s1 + 0x20 + 19 * 16 + 12) == 1.0f);  // joint w
  for (int j = 0; j < 20; ++j) {
    REQUIRE(BE32(out.data(), s1 + 0x160 + j * 4) == 2);
  }
  // Slot 3: POSITION_ONLY, id 9, hip only.
  const uint32_t s3 = 0x30 + 3 * 0x1C0;
  REQUIRE(BE32(out.data(), s3 + 0x0) == 1);
  REQUIRE(BE32(out.data(), s3 + 0x4) == 9);
  REQUIRE(BEF(out.data(), s3 + 0x18) == 2.5f);
  REQUIRE(BEF(out.data(), s3 + 0x20 + 3 * 16 + 4) == 0.0f);
  REQUIRE(BE32(out.data(), s3 + 0x160) == 0);
}

TEST_CASE("DC3_20 decodes into NUI order", "[nui]") {
  auto body = Dc3Packet(77, 12.3456, {5, 0, 9});
  PoseFrame f;
  std::string err;
  REQUIRE(DecodeV2Packet(body.data(), body.size(), &f, &err));
  REQUIRE(f.frame_number == 77);
  REQUIRE(f.sensor_time_ms == 12346);
  // id 0 is dropped (Kinect ids are > 0).
  REQUIRE(f.persons.size() == 2);
  REQUIRE(f.persons[0].tracking_id == 5);
  REQUIRE(f.persons[1].tracking_id == 9);
  // x carries the game joint index: NUI n <- game perm[n].
  const int perm[20] = {0, 1, 2,  3,  4,  5,  6,  7,  8,  9,
                        10, 11, 12, 13, 14, 18, 15, 16, 17, 19};
  for (int n = 0; n < 20; ++n) {
    REQUIRE(f.persons[0].joints[n].x == float(perm[n]));
  }
  // conf thresholds: game 7 (0.5) INFERRED, game 8 (0.1) NOT_TRACKED.
  REQUIRE(f.persons[0].joints[7].state == JointState::kInferred);
  REQUIRE(f.persons[0].joints[8].state == JointState::kNotTracked);
  REQUIRE(f.persons[0].joints[0].state == JointState::kTracked);
}

TEST_CASE("DecodeV2Packet rejects short, foreign and COCO packets", "[nui]") {
  PoseFrame f;
  std::string err;
  auto body = Dc3Packet(1, 1.0, {5});
  REQUIRE_FALSE(DecodeV2Packet(body.data(), 20, &f, &err));
  REQUIRE_FALSE(DecodeV2Packet(body.data(), body.size() - 4, &f, &err));
  body[25] = uint8_t(WireLayout::kCoco17);
  REQUIRE_FALSE(DecodeV2Packet(body.data(), body.size(), &f, &err));
  body[25] = uint8_t(WireLayout::kDc3_20);
  body[0] ^= 1;
  REQUIRE_FALSE(DecodeV2Packet(body.data(), body.size(), &f, &err));
}

TEST_CASE("SlotMap keeps persisting ids and fills the lowest free slot",
          "[nui]") {
  SlotMap m;
  std::array<int, kSkeletonCount> s;
  m.Assign(Persons({5, 9}), &s);
  REQUIRE(s[0] == 0);
  REQUIRE(s[1] == 1);
  REQUIRE(s[2] == -1);
  // 5 leaves; 9 keeps slot 1 although it is now person 0; 13 takes slot 0.
  m.Assign(Persons({9, 13}), &s);
  REQUIRE(s[1] == 0);
  REQUIRE(s[0] == 1);
  REQUIRE(m.SlotTrackingId(0) == 13);
  m.Assign(Persons({}), &s);
  for (int i = 0; i < 6; ++i) REQUIRE(s[i] == -1);
}

TEST_CASE("Faithful policy: title-set ids only, none before the title picks",
          "[nui]") {
  PoseFrame f;
  f.persons = Persons({5, 9});
  std::array<int, kSkeletonCount> slots = {0, 1, -1, -1, -1, -1};
  std::array<SkeletonState, kSkeletonCount> st{};
  TrackingSelection sel;
  sel.title_sets_tracked = true;
  DecideSkeletonStates(TrackedPolicy::kFaithful, sel, f, slots, &st);
  REQUIRE(st[0] == SkeletonState::kPositionOnly);
  REQUIRE(st[1] == SkeletonState::kPositionOnly);
  sel.title_ids[0] = 9;
  sel.title_ids[1] = 0xFFFFFFFF;
  DecideSkeletonStates(TrackedPolicy::kFaithful, sel, f, slots, &st);
  REQUIRE(st[0] == SkeletonState::kPositionOnly);
  REQUIRE(st[1] == SkeletonState::kTracked);
  REQUIRE(st[2] == SkeletonState::kNotTracked);
  // 'all' tracks everyone regardless.
  DecideSkeletonStates(TrackedPolicy::kAll, sel, f, slots, &st);
  REQUIRE(st[0] == SkeletonState::kTracked);
  REQUIRE(st[1] == SkeletonState::kTracked);
}

TEST_CASE("Default policy tracks the two nearest, with hysteresis", "[nui]") {
  PoseFrame f;
  f.persons.push_back(StandingPerson(5, 0.0f, 3.0f));
  f.persons.push_back(StandingPerson(9, 0.0f, 2.0f));
  f.persons.push_back(StandingPerson(13, 0.0f, 2.2f));
  std::array<int, kSkeletonCount> slots = {0, 1, 2, -1, -1, -1};
  std::array<SkeletonState, kSkeletonCount> st{};
  TrackingSelection sel;  // title does not choose
  DecideSkeletonStates(TrackedPolicy::kFaithful, sel, f, slots, &st);
  REQUIRE(st[0] == SkeletonState::kPositionOnly);
  REQUIRE(st[1] == SkeletonState::kTracked);
  REQUIRE(st[2] == SkeletonState::kTracked);
  // Person 5 steps to 2.35 m: closer than 13 (2.2 m from origin incl. hip
  // height) only by less than the 0.3 m bonus 13 holds while tracked.
  f.persons[0] = StandingPerson(5, 0.0f, 2.1f);
  DecideSkeletonStates(TrackedPolicy::kFaithful, sel, f, slots, &st);
  REQUIRE(st[0] == SkeletonState::kPositionOnly);
  REQUIRE(st[2] == SkeletonState::kTracked);
}

TEST_CASE("Constant source is synthetic_kinect STAND in NUI order", "[nui]") {
  auto src = CreateConstantPoseSource();
  REQUIRE(src->Open());
  PoseFrame f;
  REQUIRE(src->Sample(0, &f));
  REQUIRE(f.persons.size() == 1);
  REQUIRE(f.persons[0].tracking_id == 5);
  // NUI 15 FOOT_LEFT is game 18 (-0.12, 0.00); NUI 16 HIP_RIGHT is game 15.
  REQUIRE(f.persons[0].joints[15].x == -0.12f);
  REQUIRE(f.persons[0].joints[15].y == 0.00f);
  REQUIRE(f.persons[0].joints[16].y == 0.85f);
  REQUIRE(f.persons[0].joints[19].x == 0.12f);
  for (const auto& j : f.persons[0].joints) {
    REQUIRE(j.state == JointState::kTracked);
    REQUIRE(j.z == 2.5f);
  }
  auto empty = CreateEmptyPoseSource();
  REQUIRE(empty->Sample(0, &f));
  REQUIRE(f.persons.empty());
}

TEST_CASE("Signature matcher: masks, uniqueness and the XREF anchor",
          "[nui]") {
  // .text at 0x82000000: two thunks `li r3,0; b X` at 0x10 and 0x40 whose
  // targets differ; a body at 0x80 and one at 0xC0.
  std::vector<uint8_t> text(0x100, 0);
  auto put = [&](uint32_t off, uint32_t w) {
    xe::store_and_swap<uint32_t>(text.data() + off, w);
  };
  put(0x10, 0x38600000);                      // li r3, 0
  put(0x14, 0x48000000 | (0x80 - 0x14));      // b 0x80
  put(0x40, 0x38600000);
  put(0x44, 0x48000000 | (0xC0 - 0x44));      // b 0xC0
  put(0x80, 0x7C0802A6);                      // mflr r0
  put(0x84, 0x3D608312);                      // lis r11, X@ha
  put(0xC0, 0x7D8802A6);                      // mflr r12
  put(0xC4, 0x3D608200);
  const NuiSigWord thunk[] = {{0x38600000u, 0xFFFFFFFFu},
                              {0x48000000u, 0xFC000003u}};
  const NuiSigWord body_a[] = {{0x7C0802A6u, 0xFFFFFFFFu},
                               {0x3D600000u, 0xFFFF0000u}};
  NuiSdkSignature sig{"t", thunk, 2, -1, nullptr, 0, nullptr};
  uint32_t addr = 0;
  // Without the anchor the thunk is ambiguous (two matches).
  REQUIRE(FindSignature(text.data(), 0x82000000, 0x100, sig, &addr) == 2);
  // With it, only the thunk whose target is body_a matches.
  sig.follow_index = 1;
  sig.target = body_a;
  sig.target_count = 2;
  REQUIRE(FindSignature(text.data(), 0x82000000, 0x100, sig, &addr) == 1);
  REQUIRE(addr == 0x82000010);
  // The masked immediate does not matter; the unmasked opcode does.
  NuiSdkSignature body{"a", body_a, 2, -1, nullptr, 0, nullptr};
  REQUIRE(FindSignature(text.data(), 0x82000000, 0x100, body, &addr) == 1);
  REQUIRE(addr == 0x82000080);
  put(0x84, 0x3D60FFFF);
  REQUIRE(FindSignature(text.data(), 0x82000000, 0x100, body, &addr) == 1);
  put(0x84, 0x3D80FFFF);  // lis r12: different register, no match
  REQUIRE(FindSignature(text.data(), 0x82000000, 0x100, body, &addr) == 0);
}

}  // namespace xe::kernel::nui::test
