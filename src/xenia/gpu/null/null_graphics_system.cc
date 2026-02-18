/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2016 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "xenia/gpu/null/null_graphics_system.h"

#include "xenia/gpu/null//null_command_processor.h"
#include "xenia/xbox.h"

#ifndef XE_HEADLESS_BUILD
#include "xenia/ui/vulkan/vulkan_provider.h"
#endif

namespace xe {
namespace gpu {
namespace null {

NullGraphicsSystem::NullGraphicsSystem() {}

NullGraphicsSystem::~NullGraphicsSystem() {}

X_STATUS NullGraphicsSystem::Setup(cpu::Processor* processor,
                                   kernel::KernelState* kernel_state,
                                   ui::WindowedAppContext* app_context,
                                   bool with_presentation) {
#ifndef XE_HEADLESS_BUILD
  // For headless mode, we don't create a Vulkan provider.
  // The provider is only needed for presentation, which we don't do.
  // This allows the null backend to work without Vulkan/X11 dependencies.
  if (with_presentation) {
    // If presentation is requested, we still need Vulkan.
    // But for headless mode, presentation should be false.
    provider_ = xe::ui::vulkan::VulkanProvider::Create(false, with_presentation);
  }
#else
  (void)with_presentation;  // Suppress unused parameter warning
#endif
  return GraphicsSystem::Setup(processor, kernel_state, app_context,
                               with_presentation);
}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(
      new NullCommandProcessor(this, kernel_state_));
}

}  // namespace null
}  // namespace gpu
}  // namespace xe
