project_root = "../../.."
include(project_root.."/tools/build")

group("src")
project("xenia-gpu")
  uuid("0e8d3370-e4b1-4b05-a2e8-39ebbcdf9b17")
  kind("StaticLib")
  language("C++")
  links({
    "dxbc",
    "fmt",
    "glslang-spirv",
    "snappy",
    "xenia-base",
    "xenia-ui",
    "xxhash",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
  })
  local_platform_files()

-- Headless variant (no UI dependencies)
project("xenia-gpu-headless")
  uuid("0e8d3370-e4b1-4b05-a2e8-39ebbcdf9b18")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "snappy",
    "xenia-base",
    "xxhash",
  })
  defines({
    "XE_HEADLESS_BUILD",
  })
  files({
    "command_processor.cc",
    "draw_extent_estimator.cc",
    "draw_util.cc",
    "gpu_flags.cc",
    "graphics_system.cc",
    "packet_disassembler.cc",
    "primitive_processor.cc",
    "register_file.cc",
    "registers.cc",
    "render_target_cache.cc",
    "sampler_info.cc",
    "shader.cc",
    "shader_interpreter.cc",
    "shader_translator.cc",
    "shader_translator_disasm.cc",
    "shared_memory.cc",
    "trace_writer.cc",
  })

group("src")
project("xenia-gpu-shader-compiler")
  uuid("ad76d3e4-4c62-439b-a0f6-f83fcf0e83c5")
  kind("ConsoleApp")
  language("C++")
  links({
    "dxbc",
    "fmt",
    "glslang-spirv",
    "snappy",
    "xenia-base",
    "xenia-gpu",
    "xenia-ui",
    "xenia-ui-vulkan",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
  })
  files({
    "shader_compiler_main.cc",
    "../base/console_app_main_"..platform_suffix..".cc",
  })

  filter("platforms:Windows")
    -- Only create the .user file if it doesn't already exist.
    local user_file = project_root.."/build/xenia-gpu-shader-compiler.vcxproj.user"
    if not os.isfile(user_file) then
      debugdir(project_root)
      debugargs({
        "2>&1",
        "1>scratch/stdout-shader-compiler.txt",
      })
    end
