/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/null/null_command_processor.h"

#include "xenia/base/logging.h"

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
  static uint32_t swap_count = 0;
  swap_count++;
  if (swap_count <= 5 || (swap_count % 100) == 0) {
    XELOGI("NullGPU: IssueSwap #{} fb_ptr=0x{:08X} {}x{}", swap_count,
           frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  }
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                         uint32_t guest_address,
                                         const uint32_t* host_address,
                                         uint32_t dword_count) {
  static uint32_t shader_count = 0;
  shader_count++;
  if (shader_count <= 10 || (shader_count % 100) == 0) {
    XELOGI("NullGPU: LoadShader #{} type={} addr=0x{:08X} dwords={}",
           shader_count, static_cast<int>(shader_type), guest_address,
           dword_count);
  }
  return nullptr;
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                     uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  static uint32_t draw_count = 0;
  draw_count++;
  if (draw_count <= 5 || (draw_count % 1000) == 0) {
    XELOGI("NullGPU: IssueDraw #{} prim={} indices={}", draw_count,
           static_cast<int>(prim_type), index_count);
  }
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  static uint32_t copy_count = 0;
  copy_count++;
  if (copy_count <= 5 || (copy_count % 1000) == 0) {
    XELOGI("NullGPU: IssueCopy #{}", copy_count);
  }
  return true;
}

void NullCommandProcessor::InitializeTrace() {}

}  // namespace null
}  // namespace gpu
}  // namespace xe