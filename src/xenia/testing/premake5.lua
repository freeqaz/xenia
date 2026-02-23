project_root = "../../.."
include(project_root.."/tools/build")

test_suite("xenia-core-tests", project_root, ".", {
  links = {
    "fmt",
    "xenia-base",
    "xenia-cpu",
    "xenia-core-headless",
  },
})
