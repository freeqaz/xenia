project_root = "../../.."
include(project_root.."/tools/build")

-- Per-title modules (NOT upstream; FORK_CLEANUP_PLAN.md section 3.1). Linked by
-- xenia-headless only, which registers them in main(); the windowed xenia-app
-- links none of them. The registry they register into (title_hooks.cc) and the
-- cvar profile are compiled into xenia-core (../premake5.lua).

group("src")
project("xenia-titles-dc3")
  uuid("6b0f3e52-2d7a-4f41-9b8e-5c1a7d3e9f21")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "xenia-base",
  })
  files({
    "dc3/*.h",
    "dc3/*.cc",
    -- The decomp-layout hack pack: only applied to a DC3 image detected as
    -- the decomp layout. Kept, isolated here (plan Decisions, 2026-10-02).
    "dc3/decomp/*.h",
    "dc3/decomp/*.cc",
  })
  -- The DTA channel listens on a unix socket (sys/socket.h, sys/un.h); its
  -- only caller (dc3_title.cc) is #if XE_PLATFORM_LINUX too.
  filter("platforms:not Linux")
    removefiles({
      "dc3/dc3_dta_channel.cc",
    })
  filter({})

include("dc3/testing")
