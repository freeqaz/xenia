project_root = "../../../../.."
include(project_root.."/tools/build")

-- Was xenia-core-tests (src/xenia/testing), whose only test was the DC3 NUI
-- patch resolver test.
test_suite("xenia-titles-dc3-tests", project_root, ".", {
  links = {
    "fmt",
    "xenia-base",
    "xenia-cpu",
    "xenia-core-headless",
    "xenia-titles-dc3",
  },
})
