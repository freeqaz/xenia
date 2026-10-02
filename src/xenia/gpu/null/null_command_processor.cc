/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/null/null_command_processor.h"

#include <cstdio>

#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {
namespace null {

NullCommandProcessor::NullCommandProcessor(NullGraphicsSystem* graphics_system,
                                           kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}
NullCommandProcessor::~NullCommandProcessor() = default;

void NullCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                    uint32_t length) {}

void NullCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {}

bool NullCommandProcessor::SetupContext() {
  return CommandProcessor::SetupContext();
}

void NullCommandProcessor::ShutdownContext() {
  return CommandProcessor::ShutdownContext();
}

void NullCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                     uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  // Fork: --dump_frames_path writes the raw guest frontbuffer as PPM. Nothing
  // renders on the null GPU, so this only shows what the guest itself wrote
  // there; the Vulkan backend has its own readback-based capture.
  if (cvars::dump_frames_path.empty() || !frontbuffer_ptr ||
      !frontbuffer_width || !frontbuffer_height) {
    return;
  }
  static uint32_t frame_number = 0;
  const uint8_t* fb_data = memory_->TranslatePhysical<const uint8_t*>(
      frontbuffer_ptr);
  char filename[256];
  std::snprintf(filename, sizeof(filename), "%s/frame_%04u.ppm",
                cvars::dump_frames_path.c_str(), frame_number);
  FILE* f = std::fopen(filename, "wb");
  if (f) {
    std::fprintf(f, "P6\n%u %u\n255\n", frontbuffer_width, frontbuffer_height);
    uint32_t pixel_count = frontbuffer_width * frontbuffer_height;
    for (uint32_t i = 0; i < pixel_count; ++i) {
      // Big-endian ARGB in guest memory.
      std::fwrite(fb_data + i * 4 + 1, 1, 3, f);
    }
    std::fclose(f);
    if (frame_number % 100 == 0) {
      XELOGI("Dumped frame {} to {} ({}x{}) phys=0x{:08X}", frame_number,
             filename, frontbuffer_width, frontbuffer_height,
             frontbuffer_ptr & 0x1FFFFFFF);
    }
  }
  ++frame_number;
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                         uint32_t guest_address,
                                         const uint32_t* host_address,
                                         uint32_t dword_count) {
  return nullptr;
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                     uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  return true;
}

bool NullCommandProcessor::IssueCopy() { return true; }

void NullCommandProcessor::InitializeTrace() {}

}  // namespace null
}  // namespace gpu
}  // namespace xe