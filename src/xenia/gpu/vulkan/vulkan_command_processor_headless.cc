/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Fork-only: headless (presenter-less) frame capture for VulkanCommandProcessor
// -- --dump_frames_path readback, capture-frame draw selection and the
// deferred draw record/replay. Kept out of vulkan_command_processor.cc so that
// file stays close to upstream; vulkan_command_processor.cc only calls in from
// SetupContext, ShutdownContext, IssueSwap and IssueDraw.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_texture_cache.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_util.h"

// Headless (no presenter) capture path. Every cvar below is inert when a
// presenter exists, and all default to upstream behaviour: a headless capture
// run opts in explicitly (tools/fork-regress/scenarios/S1V.sh).
DEFINE_bool(headless_skip_submission_wait, false,
            "Headless: when the CP is asked to await a specific submission, "
            "poll for completion instead of blocking on it, so the CP thread "
            "stays responsive to EVENT_WRITE_SHD / WAIT_REG_MEM (the DC3 and "
            "RB3DX capture flows deadlock without it at ~frame 12). UNSOUND "
            "in general: buffers and descriptor pools can be recycled while "
            "the GPU still reads them (corruption or VK_ERROR_DEVICE_LOST). "
            "Upstream behaviour is false.",
            "GPU");
DEFINE_bool(headless_capture_only_draws, false,
            "Headless: execute draws and EDRAM copies only on frames the "
            "--dump_frames_path capture logic marks as render frames, dropping "
            "the rest. A capture-throughput optimisation; false renders "
            "everything (upstream behaviour). --force_all_draws overrides it.",
            "GPU");
DEFINE_bool(headless_async_pipelines, false,
            "Headless: compile new graphics pipelines on background threads "
            "and skip draws whose pipeline is not ready yet, so a slow "
            "vkCreateGraphicsPipelines does not stall the CP thread. Drops "
            "geometry until the cache is warm. Upstream behaviour is false.",
            "GPU");
DEFINE_bool(headless_persist_render_state, true,
            "Headless deferred capture: keep EDRAM + host render targets "
            "across deferred-draw flushes (tear down on the first flush only). "
            "False tears down on every flush, which wipes tiles the title "
            "expects to persist (HUD-only / partial 3D resolves).",
            "GPU");
DEFINE_bool(headless_replay_depth_disable, false,
            "Headless deferred capture DIAGNOSTIC: clear "
            "RB_DEPTHCONTROL.z_enable for every replayed deferred draw. Breaks "
            "occlusion; experiment only.",
            "GPU");
DEFINE_bool(headless_inline_render, false,
            "Headless capture: execute draws + resolves inline every frame "
            "instead of deferring them to the swap, so the capture reads the "
            "frontbuffer the title resolved on its own timeline. Needs a warm "
            "pipeline cache to avoid the CP stall the deferral exists to "
            "dodge.",
            "GPU");
// Deprecated spellings of the three cvars above (renamed 2026-10: they are
// not DC3-specific). A non-default value is honoured with a warning when the
// new name is left at its default. Remove after one release.
DEFINE_bool(dc3_persist_render_state, true,
            "DEPRECATED: use --headless_persist_render_state.", "GPU");
DEFINE_bool(dc3_replay_depth_disable, false,
            "DEPRECATED: use --headless_replay_depth_disable.", "GPU");
DEFINE_bool(dc3_inline_render, false,
            "DEPRECATED: use --headless_inline_render.", "GPU");

namespace xe {
namespace gpu {
namespace vulkan {

namespace {
// A cvar renamed from a deprecated spelling: the old name is honoured only
// when it was changed from the shared default and the new one was not.
bool ResolveRenamedCvar(bool value, bool deprecated_value, bool default_value,
                        const char* name, const char* deprecated_name) {
  if (deprecated_value != default_value && value == default_value) {
    XELOGW("--{} is deprecated, use --{}", deprecated_name, name);
    return deprecated_value;
  }
  return value;
}
}  // namespace

void VulkanCommandProcessor::SetupHeadlessCapture() {
  headless_persist_render_state_ = ResolveRenamedCvar(
      cvars::headless_persist_render_state, cvars::dc3_persist_render_state,
      true, "headless_persist_render_state", "dc3_persist_render_state");
  headless_replay_depth_disable_ = ResolveRenamedCvar(
      cvars::headless_replay_depth_disable, cvars::dc3_replay_depth_disable,
      false, "headless_replay_depth_disable", "dc3_replay_depth_disable");
  headless_inline_render_ = ResolveRenamedCvar(
      cvars::headless_inline_render, cvars::dc3_inline_render, false,
      "headless_inline_render", "dc3_inline_render");
  if (cvars::force_all_draws) {
    // force_all_draws with deferred draws: draws are queued during PM4
    // processing (keeping CP responsive for sync events) and replayed
    // at VdSwap time. Without deferral, inline draws block the CP thread
    // at frame 12 causing deadlock with the game's sync mechanism.
    deferred_draws_enabled_ = true;
    XELOGI("Force all draws enabled — deferred draw mode");
  } else if (cvars::headless_async_pipelines) {
    // Compile new pipelines off the CP thread; draws whose pipeline is
    // still compiling are skipped (see ConfigurePipeline below).
    pipeline_cache_->SetHeadlessMode(true);
  }
  if (!cvars::dump_frames_path.empty()) {
    headless_frame_dump_ = true;
    headless_capture_interval_ =
        static_cast<uint32_t>(cvars::headless_capture_interval);
    XELOGI("Headless frame dump enabled: {} (capture interval: {})",
           cvars::dump_frames_path,
           headless_capture_interval_ ? headless_capture_interval_ : 0);

    // Pre-allocate readback resources for frame capture.
    const ui::vulkan::VulkanDevice* vk_dev = GetVulkanDevice();
    if (ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vk_dev, kReadbackBufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback,
            readback_staging_buffer_, readback_staging_memory_)) {
      const auto& dfn2 = vk_dev->functions();
      VkDevice dev = vk_dev->device();

      // Persistently map the staging buffer.
      dfn2.vkMapMemory(dev, readback_staging_memory_, 0, kReadbackBufferSize,
                       0, &readback_staging_mapping_);

      // Create resettable command pool + one persistent command buffer.
      VkCommandPoolCreateInfo pool_ci{};
      pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_ci.queueFamilyIndex =
          vk_dev->queue_family_graphics_compute();
      dfn2.vkCreateCommandPool(dev, &pool_ci, nullptr,
                               &readback_command_pool_);

      VkCommandBufferAllocateInfo cb_alloc{};
      cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cb_alloc.commandPool = readback_command_pool_;
      cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cb_alloc.commandBufferCount = 1;
      dfn2.vkAllocateCommandBuffers(dev, &cb_alloc,
                                    &readback_command_buffer_);

      XELOGI("Readback resources pre-allocated: {}x{} staging buffer",
             kReadbackMaxWidth, kReadbackMaxHeight);
    } else {
      XELOGE("Failed to pre-allocate readback staging buffer");
    }
  }
}

void VulkanCommandProcessor::ShutdownHeadlessCapture() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Clean up pre-allocated readback resources.
  if (readback_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, readback_fence_, nullptr);
    readback_fence_ = VK_NULL_HANDLE;
  }
  if (readback_command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, readback_command_pool_, nullptr);
    readback_command_pool_ = VK_NULL_HANDLE;
    readback_command_buffer_ = VK_NULL_HANDLE;
  }
  if (readback_staging_memory_ != VK_NULL_HANDLE) {
    if (readback_staging_mapping_) {
      dfn.vkUnmapMemory(device, readback_staging_memory_);
      readback_staging_mapping_ = nullptr;
    }
    dfn.vkFreeMemory(device, readback_staging_memory_, nullptr);
    readback_staging_memory_ = VK_NULL_HANDLE;
  }
  if (readback_staging_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, readback_staging_buffer_, nullptr);
    readback_staging_buffer_ = VK_NULL_HANDLE;
  }
}

void VulkanCommandProcessor::IssueSwapHeadless(uint32_t frontbuffer_ptr,
                                               uint32_t frontbuffer_width,
                                               uint32_t frontbuffer_height) {
  // ================================================================
  // Execute deferred draws from the render frame. All sync events
  // (EVENT_WRITE_SHD, WAIT_REG_MEM) were processed during the frame;
  // the flush happens here at swap time after sync is satisfied.
  // ================================================================
  if (!deferred_draws_.empty()) {
    FlushDeferredDraws();
  }

  // Headless mode: flush pending submissions.
  if (submission_open_) {
    EndSubmission(true);
  }

  // Reset render/deferred state after flush (before deciding next frame).
  // Inline-render mode keeps headless_render_frame_ true permanently so
  // resolves run every frame (the game's own frontbuffer is produced live).
  if (headless_render_frame_ && !cvars::force_all_draws &&
      !headless_inline_render_) {
    headless_render_frame_ = false;
    deferred_draws_enabled_ = false;
    pipeline_cache_->SetWarmupWait(false);
  }

  if (!headless_frame_dump_) {
    return;
  }

  headless_frame_count_++;

  bool should_capture =
      headless_capture_interval_ == 0 ||
      (headless_frame_count_ % headless_capture_interval_) == 0;

  if (cvars::force_all_draws || headless_inline_render_) {
    // force_all_draws: all draws execute but still via defer+replay.
    // headless_inline_render: execute draws+resolves INLINE every frame (NO
    //   defer) so the capture reads the game's own live-resolved frontbuffer.
    headless_render_frame_ = true;
    if (headless_inline_render_) {
      deferred_draws_enabled_ = false;
    }
    if (headless_frame_count_ <= 5 || headless_frame_count_ % 100 == 0 ||
        should_capture) {
      XELOGI("VdSwap #{}: ptr=0x{:08X} {}x{}{}", headless_frame_count_,
             frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
             should_capture ? " [CAPTURE]" : "");
    }
  } else {
    // Normal headless: enable render + deferred draws for the frame
    // BEFORE capture. Draws and copies are deferred so sync events
    // process immediately, keeping the game alive.
    bool next_is_capture = false;
    if (headless_capture_interval_ > 0) {
      next_is_capture =
          ((headless_frame_count_ + 1) % headless_capture_interval_) == 0;
    }

    if (next_is_capture) {
      headless_render_frame_ = true;
      deferred_draws_enabled_ = true;
      pipeline_cache_->SetWarmupWait(true);
      XELOGI("Enabled deferred draws for render frame (next is capture)");
    }
    // Note: render_frame already reset above for non-next-is-capture frames.

    if (headless_frame_count_ <= 5 || headless_frame_count_ % 100 == 0 ||
        should_capture || next_is_capture) {
      XELOGI("VdSwap #{}: ptr=0x{:08X} {}x{}{}{}", headless_frame_count_,
             frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
             next_is_capture ? " [RENDER+DEFER]" : "",
             should_capture ? " [CAPTURE]" : "");
    }
  }

  if (!should_capture) {
    return;
  }

  // ================================================================
  // PHASE 1: Submit readback work for this capture frame.
  // Deferred draws were flushed above. Submit GPU work for texture
  // load + image copy, then defer pixel read to Phase 2 (next swap).
  // ================================================================
  if (frontbuffer_ptr && frontbuffer_width && frontbuffer_height &&
      readback_staging_buffer_ != VK_NULL_HANDLE) {
    // Wait for GPU to finish deferred draws + copies.
    if (submission_open_) {
      EndSubmission(true);
    }
    AwaitAllQueueOperationsCompletion();

    // Dump raw GPU buffer bytes at frontbuffer address for debugging.
    // Gated: submits Vulkan work every capture frame just to print hex.
    if (cvars::headless_verbose_diagnostics) {
      auto* vsm = static_cast<VulkanSharedMemory*>(shared_memory_.get());
      const ui::vulkan::VulkanDevice* vd = GetVulkanDevice();
      const auto& dfn2 = vd->functions();
      dfn2.vkResetCommandPool(vd->device(), readback_command_pool_, 0);
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      dfn2.vkBeginCommandBuffer(readback_command_buffer_, &bi);
      VkBufferCopy bc{};
      bc.srcOffset = frontbuffer_ptr;
      bc.dstOffset = 0;
      bc.size = 256;
      dfn2.vkCmdCopyBuffer(readback_command_buffer_,
                           vsm->buffer(), readback_staging_buffer_, 1, &bc);
      dfn2.vkEndCommandBuffer(readback_command_buffer_);
      if (readback_fence_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        dfn2.vkCreateFence(vd->device(), &fci, nullptr, &readback_fence_);
      } else {
        dfn2.vkResetFences(vd->device(), 1, &readback_fence_);
      }
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &readback_command_buffer_;
      {
        auto qa = vd->AcquireQueue(vd->queue_family_graphics_compute(), 0);
        dfn2.vkQueueSubmit(qa.queue(), 1, &si, readback_fence_);
      }
      dfn2.vkWaitForFences(vd->device(), 1, &readback_fence_, VK_TRUE,
                           UINT64_MAX);
      uint8_t* bytes = static_cast<uint8_t*>(readback_staging_mapping_);
      XELOGI("GPU buffer raw @ 0x{:08X}: {:02X} {:02X} {:02X} {:02X}  "
             "{:02X} {:02X} {:02X} {:02X}  {:02X} {:02X} {:02X} {:02X}  "
             "{:02X} {:02X} {:02X} {:02X}",
             frontbuffer_ptr,
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
      XELOGI("GPU buffer raw @ 0x{:08X}+16: {:02X} {:02X} {:02X} {:02X}  "
             "{:02X} {:02X} {:02X} {:02X}  {:02X} {:02X} {:02X} {:02X}  "
             "{:02X} {:02X} {:02X} {:02X}",
             frontbuffer_ptr,
             bytes[16], bytes[17], bytes[18], bytes[19],
             bytes[20], bytes[21], bytes[22], bytes[23],
             bytes[24], bytes[25], bytes[26], bytes[27],
             bytes[28], bytes[29], bytes[30], bytes[31]);
    }

    // Load the swap texture from resolved EDRAM content.
    // NOTE: Do NOT call MemoryInvalidationCallback here -- the resolve
    // compute shader writes to the GPU buffer (shared_memory.buffer()),
    // and MarkRangeAsResolved marks those pages valid+gpu_written.
    // Invalidating would force re-upload from guest physical memory
    // (which is zeros), destroying the resolve data.
    if (!BeginSubmission(true)) {
      return;
    }
    uint32_t width_scaled, height_scaled;
    xenos::TextureFormat format;
    VkImageView swap_view = texture_cache_->RequestSwapTexture(
        width_scaled, height_scaled, format);
    EndSubmission(true);
    AwaitAllQueueOperationsCompletion();

    // Log fetch constant details for debugging channel order issues.
    {
      const auto& regs = *register_file_;
      auto fetch = regs.GetTextureFetch(0);
      XELOGI(
          "RequestSwapTexture: view={} {}x{} fmt={} "
          "endian={} guest_swizzle=0x{:03X} host_swizzle=0x{:03X}",
          swap_view != VK_NULL_HANDLE ? "valid" : "NULL", width_scaled,
          height_scaled, static_cast<uint32_t>(format),
          static_cast<uint32_t>(fetch.endianness), fetch.swizzle,
          texture_cache_->GetLastSwapHostSwizzle());
    }
    if (swap_view != VK_NULL_HANDLE) {
      VkImage swap_image = texture_cache_->GetLastSwapImage();
      if (swap_image != VK_NULL_HANDLE) {
        const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
        const auto& dfn = vulkan_device->functions();
        VkDevice device = vulkan_device->device();

        VkDeviceSize buffer_size =
            static_cast<VkDeviceSize>(width_scaled) * height_scaled * 4;

        if (buffer_size <= kReadbackBufferSize) {
          // Reset and record pre-allocated command buffer.
          dfn.vkResetCommandPool(device, readback_command_pool_, 0);

          VkCommandBufferBeginInfo begin_info{};
          begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
          begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
          dfn.vkBeginCommandBuffer(readback_command_buffer_, &begin_info);

          VkImageMemoryBarrier barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
          barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = swap_image;
          barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          barrier.subresourceRange.levelCount = 1;
          barrier.subresourceRange.layerCount = 1;
          dfn.vkCmdPipelineBarrier(
              readback_command_buffer_,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
              &barrier);

          VkBufferImageCopy region{};
          region.bufferRowLength = width_scaled;
          region.bufferImageHeight = height_scaled;
          region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.imageSubresource.layerCount = 1;
          region.imageExtent = {width_scaled, height_scaled, 1};
          dfn.vkCmdCopyImageToBuffer(
              readback_command_buffer_, swap_image,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              readback_staging_buffer_, 1, &region);

          barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          dfn.vkCmdPipelineBarrier(
              readback_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
              nullptr, 1, &barrier);

          dfn.vkEndCommandBuffer(readback_command_buffer_);

          // Submit readback with fence and wait synchronously.
          if (readback_fence_ == VK_NULL_HANDLE) {
            VkFenceCreateInfo fence_ci{};
            fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            dfn.vkCreateFence(device, &fence_ci, nullptr, &readback_fence_);
          } else {
            dfn.vkResetFences(device, 1, &readback_fence_);
          }

          VkSubmitInfo submit{};
          submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
          submit.commandBufferCount = 1;
          submit.pCommandBuffers = &readback_command_buffer_;
          {
            auto queue_acq = vulkan_device->AcquireQueue(
                vulkan_device->queue_family_graphics_compute(), 0);
            dfn.vkQueueSubmit(queue_acq.queue(), 1, &submit,
                              readback_fence_);
          }

          // Wait for GPU copy to complete and write PPM immediately.
          dfn.vkWaitForFences(device, 1, &readback_fence_, VK_TRUE,
                              UINT64_MAX);

          // Read from persistent mapping and write PPM.
          const uint8_t* pixels =
              static_cast<const uint8_t*>(readback_staging_mapping_);
          uint32_t total_pixels = width_scaled * height_scaled;

          // Determine channel byte mapping from the VkImageView's host
          // swizzle. The VkImage stores raw endian-swapped guest data in
          // R8G8B8A8 layout. The VkImageView swizzle remaps channels for
          // correct shader sampling, but vkCmdCopyImageToBuffer bypasses
          // the view. We apply the same swizzle on the CPU side.
          //
          // Host swizzle is 4x 3-bit indices packed into 12 bits:
          //   bits [2:0]  = output R reads from source component N
          //   bits [5:3]  = output G reads from source component N
          //   bits [8:6]  = output B reads from source component N
          //   bits [11:9] = output A reads from source component N
          // where source component 0=R(byte0), 1=G(byte1), 2=B(byte2),
          // 3=A(byte3), 4=0, 5=1
          uint32_t host_swizzle =
              texture_cache_->GetLastSwapHostSwizzle();
          uint32_t r_src = (host_swizzle >> 0) & 7;
          uint32_t g_src = (host_swizzle >> 3) & 7;
          uint32_t b_src = (host_swizzle >> 6) & 7;

          // Log the swizzle for debugging.
          XELOGI(
              "Frame {} readback: host_swizzle=0x{:03X} "
              "R<-{} G<-{} B<-{} A<-{}",
              headless_frame_count_, host_swizzle,
              r_src, g_src, b_src, (host_swizzle >> 9) & 7);

          // Build sRGB gamma correction lookup table.
          // Converts linear-space values to sRGB for human-viewable output.
          static uint8_t srgb_lut[256] = {};
          static bool srgb_lut_built = false;
          if (!srgb_lut_built) {
            for (int i = 0; i < 256; i++) {
              float linear = i / 255.0f;
              float srgb;
              if (linear <= 0.0031308f)
                srgb = linear * 12.92f;
              else
                srgb = 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
              srgb_lut[i] = static_cast<uint8_t>(
                  std::min(255.0f, srgb * 255.0f + 0.5f));
            }
            srgb_lut_built = true;
          }

          // Helper to read a channel value from the pixel, handling swizzle
          // constants (4=0, 5=1).
          auto read_channel = [&pixels](uint32_t pi,
                                        uint32_t src) -> uint8_t {
            if (src <= 3) return pixels[pi + src];
            if (src == 4) return 0;    // constant 0
            return 255;                // constant 1
          };

          uint32_t nonzero = 0;
          for (uint32_t i = 0; i < total_pixels * 4; i += 4) {
            uint8_t r = read_channel(i, r_src);
            uint8_t g = read_channel(i, g_src);
            uint8_t b = read_channel(i, b_src);
            if (r || g || b) nonzero++;
          }
          XELOGI("Frame {}: {}x{} fmt={} {}/{} non-zero pixels ({}%)",
                 headless_frame_count_, width_scaled, height_scaled,
                 static_cast<int>(format), nonzero, total_pixels,
                 total_pixels ? nonzero * 100 / total_pixels : 0);
          XELOGI("RSTAB: CAPTURE frame={} flush#={} frontbuffer=0x{:08X} "
                 "nonzero_pct={} verdict={}",
                 headless_frame_count_, deferred_flush_count_, frontbuffer_ptr,
                 total_pixels ? nonzero * 100 / total_pixels : 0,
                 (total_pixels && nonzero * 100 / total_pixels > 15)
                     ? "SCENE"
                     : "HUD_ONLY");

          // Write gamma-corrected PPM with swizzle-corrected channels.
          char ppm_path[512];
          std::snprintf(ppm_path, sizeof(ppm_path), "%s/frame_%04u.ppm",
                        cvars::dump_frames_path.c_str(),
                        headless_frame_count_);
          FILE* f = std::fopen(ppm_path, "wb");
          if (f) {
            std::fprintf(f, "P6\n%u %u\n255\n", width_scaled,
                         height_scaled);
            for (uint32_t pi = 0; pi < total_pixels * 4; pi += 4) {
              uint8_t rgb[3] = {
                  srgb_lut[read_channel(pi, r_src)],
                  srgb_lut[read_channel(pi, g_src)],
                  srgb_lut[read_channel(pi, b_src)],
              };
              std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
            XELOGI("Saved {}", ppm_path);
          }

          // Also save raw (no gamma, no swizzle) PPM for comparison.
          std::snprintf(ppm_path, sizeof(ppm_path),
                        "%s/frame_%04u_raw.ppm",
                        cvars::dump_frames_path.c_str(),
                        headless_frame_count_);
          f = std::fopen(ppm_path, "wb");
          if (f) {
            std::fprintf(f, "P6\n%u %u\n255\n", width_scaled,
                         height_scaled);
            for (uint32_t pi = 0; pi < total_pixels * 4; pi += 4) {
              std::fwrite(pixels + pi, 1, 3, f);
            }
            std::fclose(f);
          }
        }
      }
    }
  }

  // Schedule the NEXT render frame after capture completes.
  // Without this, only the first capture triggers RENDER+DEFER.
  // Skip in inline-render mode (no deferral; render frame is always on).
  if (headless_capture_interval_ > 0 && !headless_inline_render_) {
    bool next_is_capture =
        ((headless_frame_count_ + 1) % headless_capture_interval_) == 0;
    if (next_is_capture) {
      headless_render_frame_ = true;
      deferred_draws_enabled_ = true;
      pipeline_cache_->SetWarmupWait(true);
      XELOGI("Scheduled next RENDER+DEFER for frame {}",
             headless_frame_count_ + 1);
    }
  }
}

bool VulkanCommandProcessor::InterceptHeadlessDraw(
    xenos::EdramMode edram_mode, xenos::PrimitiveType prim_type,
    uint32_t index_count, IndexBufferInfo* index_buffer_info,
    bool major_mode_explicit, bool& result) {
  if (cvars::headless_verbose_diagnostics) {
    // Diagnostic: count all draws (even skipped ones).
    static uint32_t total_draw_count = 0;
    static uint32_t total_swap_at_last_draw = 0;
    total_draw_count++;
    if (headless_frame_count_ != total_swap_at_last_draw) {
      if (total_draw_count <= 200 || total_draw_count % 500 == 0) {
        XELOGI(
            "IssueDraw #{} at swap_count={} prim={} idx_count={} "
            "render_frame={}",
            total_draw_count, headless_frame_count_,
            static_cast<int>(prim_type), index_count, headless_render_frame_);
      }
      total_swap_at_last_draw = headless_frame_count_;
    }
  }

  // Diagnostic: log ALL draws during warmup (both copy and non-copy)
  if (cvars::headless_verbose_diagnostics &&
      !graphics_system_->presenter() && headless_render_frame_) {
    static uint32_t all_draw_log_count = 0;
    all_draw_log_count++;
    if (all_draw_log_count <= 20 || all_draw_log_count % 200 == 0) {
      XELOGI("WARMUP ALL-DRAW #{}: edram_mode={} (0=colorDepth, 1=colorOnly, 2=depthOnly, 3=copy)",
             all_draw_log_count, static_cast<int>(edram_mode));
    }
  }

  if (edram_mode == xenos::EdramMode::kCopy) {
    // HEADLESS: Skip copies when not rendering — no EDRAM content to resolve.
    // Gated on --headless_capture_only_draws alongside the non-copy skip
    // below; dropping copies while still issuing draws would make
    // --headless_capture_only_draws=false render into EDRAM and never resolve.
    if (cvars::headless_capture_only_draws &&
        !graphics_system_->presenter() && !headless_render_frame_ &&
        !cvars::force_all_draws) {
      result = true;
      return true;
    }
    // Defer copies along with draws during deferred rendering. Flushing
    // draws at copy time would execute Vulkan draws mid-frame, permanently
    // killing VdSwap before the capture frame arrives.
    if (deferred_draws_enabled_) {
      DeferredDrawState state;
      state.register_values.assign(
          register_file_->values,
          register_file_->values + RegisterFile::kRegisterCount);
      state.vertex_shader = active_vertex_shader();
      state.pixel_shader = active_pixel_shader();
      state.prim_type = prim_type;
      state.index_count = index_count;
      state.is_indexed = (index_buffer_info != nullptr);
      state.is_copy = true;
      state.major_mode_explicit = major_mode_explicit;
      if (index_buffer_info) {
        state.index_buffer_info = *index_buffer_info;
      }
      // Save resolve vertex data from guest memory — it may be overwritten
      // by subsequent frames before we replay.
      xenos::xe_gpu_vertex_fetch_t vfetch =
          register_file_->GetVertexFetch(0);
      state.resolve_vertex_addr = 0;
      std::memset(state.resolve_vertex_data, 0,
                  sizeof(state.resolve_vertex_data));
      if (vfetch.type == xenos::FetchConstantType::kVertex &&
          vfetch.size == 3 * 2 && vfetch.address) {
        state.resolve_vertex_addr = vfetch.address;
        const void* guest_ptr = memory_->TranslatePhysical(
            vfetch.address * sizeof(uint32_t));
        std::memcpy(state.resolve_vertex_data, guest_ptr,
                    sizeof(state.resolve_vertex_data));
      }
      deferred_draws_.push_back(std::move(state));
      result = true;
      return true;
    }
    // Non-deferred: flush pending draws; the caller then issues the copy.
    if (!deferred_draws_.empty()) {
      FlushDeferredDraws();
    }
    return false;
  }

  // HEADLESS: Skip non-copy draws unless this is a capture frame or
  // force_all_draws is enabled.
  //
  // !presenter() alone is true for ANY windowless run, so ungated this drops
  // every draw of every headless title — trace dumps included. Require
  // --headless_capture_only_draws (default false = upstream behaviour; capture
  // runs opt in).
  if (cvars::headless_capture_only_draws &&
      !graphics_system_->presenter() && !headless_render_frame_ &&
      !cvars::force_all_draws) {
    result = true;
    return true;
  }

  // DEFERRED DRAWS: Save register state and skip the draw. Draws will be
  // executed later at XE_SWAP time, after sync events have been processed.
  // This prevents CP deadlock caused by slow draws blocking sync events.
  if (deferred_draws_enabled_) {
    DeferredDrawState state;
    state.register_values.assign(
        register_file_->values,
        register_file_->values + RegisterFile::kRegisterCount);
    state.vertex_shader = active_vertex_shader();
    state.pixel_shader = active_pixel_shader();
    state.prim_type = prim_type;
    state.index_count = index_count;
    state.is_indexed = (index_buffer_info != nullptr);
    state.major_mode_explicit = major_mode_explicit;
    if (index_buffer_info) {
      state.index_buffer_info = *index_buffer_info;
    }
    deferred_draws_.push_back(std::move(state));
    if (cvars::headless_verbose_diagnostics) {
      static uint32_t defer_log_count = 0;
      defer_log_count++;
      if (defer_log_count <= 10 || defer_log_count % 100 == 0) {
        XELOGI("Deferred draw #{} (prim={} idx_count={})", defer_log_count,
               static_cast<int>(prim_type), index_count);
      }
    }
    result = true;
    return true;
  }

  // Diagnostic: log when warmup draws start (only first few per frame)
  if (cvars::headless_verbose_diagnostics) {
    static uint32_t warmup_draw_log_count = 0;
    if (!graphics_system_->presenter() && headless_render_frame_) {
      warmup_draw_log_count++;
      if (warmup_draw_log_count <= 10 || warmup_draw_log_count % 100 == 0) {
        XELOGI("WARMUP DRAW #{}: edram_mode={} headless_render_frame_={}",
               warmup_draw_log_count, static_cast<int>(edram_mode),
               headless_render_frame_);
      }
    }
  }

  return false;
}

void VulkanCommandProcessor::FlushDeferredDraws() {
  if (deferred_draws_.empty()) return;

  uint32_t draw_count = static_cast<uint32_t>(deferred_draws_.size());
  XELOGI("FlushDeferredDraws: executing {} deferred draws", draw_count);

  auto flush_start = std::chrono::steady_clock::now();

  // Save current register state and shaders to restore after.
  std::vector<uint32_t> saved_regs(
      register_file_->values,
      register_file_->values + RegisterFile::kRegisterCount);
  Shader* saved_vs = active_vertex_shader_;
  Shader* saved_ps = active_pixel_shader_;

  // Temporarily disable deferred mode so the draws actually execute.
  deferred_draws_enabled_ = false;

  // Suppress mprotect-based page watches during deferred draw execution.
  // Without this, RequestTextures → MakeRangeValid sets up page watches that
  // persist after the flush. Game threads then trigger SIGSEGVs on every write
  // to watched pages, and the signal handler's global lock contention kills
  // the game thread's timing (VdSwap stops being called 2 frames later).
  shared_memory_->set_suppress_memory_watches(true);

  ++deferred_flush_count_;

  // Clear all host render targets and EDRAM state so deferred draws start
  // from a clean slate. Without this, host render targets retain stale
  // content from the previous render frame (potentially hundreds of frames
  // ago), causing ghosting artifacts where old screen content bleeds through
  // during transitions.
  //
  // BUT doing this on EVERY flush corrupts the 3D-scene resolve: DC3 relies on
  // EDRAM/RT content persisting across frames for parts of the scene that are
  // not re-rendered every frame (RB_COPY_DEST_BASE=0x1E830000 resolves run every
  // flush but read zeroed tiles after the wipe -> HUD-only / 50%-plateau frames).
  // So when headless_persist_render_state is set, only tear down on the FIRST
  // flush; afterwards EDRAM + host RTs persist and the resolve sees complete
  // tiles.
  if (render_target_cache_ &&
      (!headless_persist_render_state_ || deferred_flush_count_ == 1)) {
    // Wait for any in-flight GPU work that references render targets.
    if (submission_open_) {
      EndSubmission(true);
    }
    AwaitAllQueueOperationsCompletion();

    // First destroy all Vulkan framebuffers and render passes that hold
    // references to render target image views. Without this, ResetState()
    // destroys the VkImage/VkImageView objects but leaves dangling
    // VkFramebuffer references in the cache, causing VK_ERROR_DEVICE_LOST
    // when those framebuffers are reused on subsequent flushes.
    render_target_cache_->ClearCache();

    // Now destroy all host render targets and reset EDRAM ownership tracking.
    // They'll be recreated fresh on demand during draw replay.
    render_target_cache_->ResetState();

    // Invalidate accumulated render target pointers which now dangle after
    // DestroyAllRenderTargets deleted the RenderTarget objects.
    render_target_cache_->BeginFrame();

    // Also fill the EDRAM buffer with zeros via a one-shot command buffer.
    // This ensures that any ownership transfers or EDRAM reads during the
    // deferred draws see clean data instead of stale content.
    VkBuffer edram_buf = render_target_cache_->edram_buffer();
    if (edram_buf != VK_NULL_HANDLE && readback_command_buffer_ != VK_NULL_HANDLE) {
      const ui::vulkan::VulkanDevice* vd = GetVulkanDevice();
      const auto& dfn = vd->functions();
      dfn.vkResetCommandPool(vd->device(), readback_command_pool_, 0);
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      dfn.vkBeginCommandBuffer(readback_command_buffer_, &bi);

      // Barrier: transition EDRAM buffer for transfer writes.
      VkBufferMemoryBarrier buf_barrier{};
      buf_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      buf_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_SHADER_WRITE_BIT;
      buf_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      buf_barrier.buffer = edram_buf;
      buf_barrier.offset = 0;
      buf_barrier.size = VK_WHOLE_SIZE;
      dfn.vkCmdPipelineBarrier(
          readback_command_buffer_,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &buf_barrier, 0,
          nullptr);

      dfn.vkCmdFillBuffer(readback_command_buffer_, edram_buf, 0,
                          render_target_cache_->edram_buffer_size(), 0);

      // Barrier: transition back for shader reads.
      buf_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      buf_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_SHADER_WRITE_BIT;
      dfn.vkCmdPipelineBarrier(
          readback_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 1, &buf_barrier, 0, nullptr);

      dfn.vkEndCommandBuffer(readback_command_buffer_);

      if (readback_fence_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        dfn.vkCreateFence(vd->device(), &fci, nullptr, &readback_fence_);
      } else {
        dfn.vkResetFences(vd->device(), 1, &readback_fence_);
      }
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &readback_command_buffer_;
      {
        auto qa = vd->AcquireQueue(vd->queue_family_graphics_compute(), 0);
        dfn.vkQueueSubmit(qa.queue(), 1, &si, readback_fence_);
      }
      dfn.vkWaitForFences(vd->device(), 1, &readback_fence_, VK_TRUE,
                          UINT64_MAX);
    }

    XELOGI("FlushDeferredDraws: cleared EDRAM and host render targets");
  }

  // Execute all deferred draws normally (no debug limits).

  uint32_t success_count = 0;
  uint32_t copy_count = 0;
  for (uint32_t i = 0; i < draw_count; ++i) {
    auto& state = deferred_draws_[i];

    // Restore register file to the state at the time this draw was deferred.
    std::memcpy(register_file_->values, state.register_values.data(),
                RegisterFile::kRegisterCount * sizeof(uint32_t));

    // Invalidate Vulkan-side caches that track register state.
    // The raw memcpy bypasses WriteRegister(), which normally marks constant
    // buffers dirty and notifies the texture cache of fetch constant changes.
    // Without this, draws may render with stale constant data or textures.
    current_constant_buffers_up_to_date_ = 0;
    if (texture_cache_) {
      texture_cache_->ResetTextureBindingsInSync();
    }

    bool ok;
    if (state.is_copy) {
      // Ensure draws before this copy are fully completed on GPU.
      // The resolve reads from EDRAM (render targets) written by draws;
      // without a flush, the EDRAM content may not be visible yet.
      if (submission_open_) {
        EndSubmission(true);
      }
      AwaitAllQueueOperationsCompletion();
      // Restore saved resolve vertex data to guest memory before the copy.
      // Guest memory may have been overwritten since deferral.
      if (state.resolve_vertex_addr) {
        void* guest_ptr = memory_->TranslatePhysical(
            state.resolve_vertex_addr * sizeof(uint32_t));
        std::memcpy(guest_ptr, state.resolve_vertex_data,
                    sizeof(state.resolve_vertex_data));
      }
      // Validate copy state before executing — skip operations that would
      // assert in GetResolveInfo due to invalid register state.
      auto rb_copy_ctl =
          register_file_->Get<reg::RB_COPY_CONTROL>();
      auto copy_cmd = rb_copy_ctl.copy_command;
      xenos::xe_gpu_vertex_fetch_t vfetch =
          register_file_->GetVertexFetch(0);
      bool copy_valid =
          (copy_cmd == xenos::CopyCommand::kRaw ||
           copy_cmd == xenos::CopyCommand::kConvert) &&
          vfetch.type == xenos::FetchConstantType::kVertex &&
          vfetch.size == 3 * 2;
      if (!copy_valid) {
        if (cvars::headless_verbose_diagnostics && (i < 10 || copy_count == 0)) {
          XELOGI("  deferred copy {}/{}: SKIPPED (cmd={} fetch_type={} "
                 "fetch_size={})",
                 i + 1, draw_count, static_cast<uint32_t>(copy_cmd),
                 static_cast<uint32_t>(vfetch.type), vfetch.size);
        }
        continue;
      }
      // Log resolve details
      if (cvars::headless_verbose_diagnostics) {
        auto rb_copy_dest = register_file_->Get<reg::RB_COPY_DEST_INFO>();
        auto rb_copy_addr = register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE];
        auto rb_surface_info = register_file_->Get<reg::RB_SURFACE_INFO>();
        auto rb_color_info = register_file_->values[XE_GPU_REG_RB_COLOR_INFO];
        auto rb_color1_info = register_file_->values[XE_GPU_REG_RB_COLOR1_INFO];
        auto rb_depth_info = register_file_->values[XE_GPU_REG_RB_DEPTH_INFO];
        auto rb_copy_ctl_full = register_file_->values[XE_GPU_REG_RB_COPY_CONTROL];
        XELOGI("  RESOLVE {}/{}: cmd={} dest_addr=0x{:08X} dest_endian={} "
               "dest_format={} surface_pitch={} color_info=0x{:08X} "
               "color1_info=0x{:08X} depth_info=0x{:08X} copy_ctl=0x{:08X}",
               i + 1, draw_count, static_cast<uint32_t>(copy_cmd),
               rb_copy_addr,
               static_cast<uint32_t>(rb_copy_dest.copy_dest_endian),
               static_cast<uint32_t>(rb_copy_dest.copy_dest_format),
               rb_surface_info.surface_pitch,
               rb_color_info, rb_color1_info, rb_depth_info, rb_copy_ctl_full);
      }
      ok = IssueCopy();
      copy_count++;
      if (cvars::headless_verbose_diagnostics) {
        XELOGI("RSTAB: REPLAY_COPY flush#{} copy#{} dest_base=0x{:08X} ok={}",
               deferred_flush_count_, copy_count,
               register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE], ok);
      }
    } else {
      active_vertex_shader_ = state.vertex_shader;
      active_pixel_shader_ = state.pixel_shader;
      if (headless_replay_depth_disable_) {
        // DIAGNOSTIC: clear z_enable (bit 1) so the depth test always passes.
        register_file_->values[XE_GPU_REG_RB_DEPTHCONTROL] &= ~uint32_t(0x2);
      }
      // RSTAB2: bound color RT EDRAM base (tiles) per substantial-geometry draw,
      // so a per-flush histogram can show whether scene-RT-binding draws are
      // present on burst flushes and ~absent on HUD-only flushes (rank-2
      // guest-side) vs invariant (rank-1 ownership-at-resolve). Gated to
      // geometry draws + a sample to keep the hot replay loop fast.
      if (cvars::headless_verbose_diagnostics &&
          (state.index_count >= 64 || (i % 32) == 0)) {
        auto rstab2_si = register_file_->Get<reg::RB_SURFACE_INFO>();
        XELOGI("RSTAB2: REPLAY_DRAW flush#{} draw#{} color_base_tiles={} "
               "surface_pitch={} prim={} idx={}",
               deferred_flush_count_, i + 1,
               register_file_->values[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF,
               rstab2_si.surface_pitch,
               static_cast<uint32_t>(state.prim_type), state.index_count);
      }
      // Execute the draw with the restored state.
      ok = IssueDraw(
          state.prim_type, state.index_count,
          state.is_indexed ? &state.index_buffer_info : nullptr,
          state.major_mode_explicit);
    }
    if (ok) success_count++;

    if (cvars::headless_verbose_diagnostics && (i < 5 || i % 50 == 0)) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - flush_start).count();
      XELOGI("  deferred {}{}/{}: ok={} ({}ms elapsed)",
             state.is_copy ? "copy " : "draw ", i + 1, draw_count,
             ok, elapsed_ms);
    }
  }

  // Restore original register state and shaders.
  std::memcpy(register_file_->values, saved_regs.data(),
              RegisterFile::kRegisterCount * sizeof(uint32_t));
  active_vertex_shader_ = saved_vs;
  active_pixel_shader_ = saved_ps;

  // Invalidate caches after the final register restore — the memcpy bypasses
  // WriteRegister() side effects. Without this, RequestSwapTexture() may use
  // stale texture bindings from the last deferred draw's register state.
  current_constant_buffers_up_to_date_ = 0;
  if (texture_cache_) {
    texture_cache_->ResetTextureBindingsInSync();
  }

  // Re-enable deferred mode and restore memory watches.
  shared_memory_->set_suppress_memory_watches(false);
  deferred_draws_enabled_ = true;

  auto flush_end = std::chrono::steady_clock::now();
  auto flush_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      flush_end - flush_start).count();
  XELOGI("FlushDeferredDraws: {} succeeded/{} total ({} copies) in {}ms",
         success_count, draw_count, copy_count, flush_ms);

  deferred_draws_.clear();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
