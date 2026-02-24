project_root = "../../../.."
include(project_root.."/tools/build")

test_suite("xenia-cpu-tests", project_root, ".", {
  links = {
    "capstone",
    "fmt",
    "mspack",
    "xenia-base",
    "xenia-core",
    "xenia-cpu",
    "xenia-vfs",

    -- TODO(benvanik): cut these dependencies?
    "xenia-kernel",
    "xenia-ui", -- needed by xenia-base
  },
  filtered_links = {
    {
      filter = 'architecture:x86_64',
      links = {
        "xenia-cpu-backend-x64",
      },
    }
  },
})
