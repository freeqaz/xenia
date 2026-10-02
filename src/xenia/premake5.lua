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
  -- The per-title hook registry and (empty) cvar profile are core: the
  -- emulator calls them for every title. The title modules themselves are in
  -- titles/premake5.lua and are linked by xenia-headless only.
  files({
    "titles/title_hooks.h",
    "titles/title_hooks.cc",
    "titles/title_ids.h",
    "titles/title_profile.h",
    "titles/title_profile.cc",
  })

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
  -- The per-title hook registry and (empty) cvar profile are core: the
  -- emulator calls them for every title. The title modules themselves are in
  -- titles/premake5.lua and are linked by xenia-headless only.
  files({
    "titles/title_hooks.h",
    "titles/title_hooks.cc",
    "titles/title_ids.h",
    "titles/title_profile.h",
    "titles/title_profile.cc",
  })

include("titles")
