project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-kernel")
  uuid("ae185c4a-1c4f-4503-9892-328e549e871a")
  kind("StaticLib")
  language("C++")
  links({
    "aes_128",
    "fmt",
    "xenia-apu",
    "xenia-base",
    "xenia-cpu",
    "xenia-hid",
    "xenia-vfs",
  })
  defines({
  })
  recursive_platform_files()
  files({
    "debug_visualizers.natvis",
  })

-- Headless variant of kernel (no UI dependencies)
project("xenia-kernel-headless")
  uuid("ae185c4a-1c4f-4503-9892-328e549e871b")
  kind("StaticLib")
  language("C++")
  links({
    "aes_128",
    "fmt",
    "xenia-apu",
    "xenia-apu-nop",
    "xenia-base",
    "xenia-cpu",
    "xenia-hid",
    "xenia-hid-nop",
    "xenia-vfs",
  })
  defines({
    "XE_HEADLESS_BUILD",
  })
  recursive_platform_files()
  files({
    "debug_visualizers.natvis",
  })
