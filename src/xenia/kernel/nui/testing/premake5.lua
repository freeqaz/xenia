project_root = "../../../../.."
include(project_root.."/tools/build")

-- Host unit tests of the Kinect (NUI) HLE device's pure parts: frame
-- encoding, the DC3_20 -> NUI permutation, the slot policy, the tracking
-- policy and the signature matcher. Built from the sources directly so the
-- suite needs xenia-base only, not the kernel.
test_suite("xenia-kernel-nui-tests", project_root, ".", {
  links = {
    "fmt",
    "xenia-base",
  },
})
  filter({})
  files({
    "../nui_frame.cc",
    "../nui_pose_source.cc",
    "../nui_signature.cc",
  })
