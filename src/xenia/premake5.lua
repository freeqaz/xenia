project_root = "../.."
include(project_root.."/tools/build")

group("src")
project("xenia-core")
  uuid("970f7892-f19a-4bf5-8795-478c51757bec")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "xenia-base",
  })
  defines({
  })
  files({"*.h", "*.cc"})

-- Headless variant (no UI dependencies)
project("xenia-core-headless")
  uuid("970f7892-f19a-4bf5-8795-478c51757bed")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "xenia-base",
  })
  defines({
    "XE_HEADLESS_BUILD",
  })
  files({"*.h", "*.cc"})
