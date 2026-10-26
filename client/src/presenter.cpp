#include "robv/client/presenter.h"

#include <SDL3/SDL_video.h>
#include <algorithm>
#include <ctime>
#include <string>

namespace robv::presenter
{
    namespace
    {
        const char* present_mode_name(VkPresentModeKHR mode)
        {
            switch (mode)
            {
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
            case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
            case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
            case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR: return "shared-demand";
            case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR: return "shared-continuous";
            default: return "other";
            }
        }

        // UNORM, not SRGB: ImGui's colours and the decoded video are already gamma encoded
        VkSurfaceFormatKHR pick_format(const vk_context_t& vk, VkSurfaceKHR surface)
        {
            u32 count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, surface, &count, formats.data());

            for (const VkFormat wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
            {
                for (const VkSurfaceFormatKHR& format : formats)
                {
                    if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    {
                        return format;
                    }
                }
            }
            return formats.empty() ? VkSurfaceFormatKHR{} : formats.front();
        }

        // MAILBOX if available: the display takes the newest image, so a touch shows next refresh
        // FIFO queues every image, ~65 ms of drag lag with Mali's 7 image swapchain
        // MAILBOX never blocks, so the app paces itself (app::iterate)
        // FIFO is the fallback, always available
        VkPresentModeKHR pick_present_mode(const vk_context_t& vk, VkSurfaceKHR surface)
        {
            u32 count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, surface, &count, modes.data());

            std::string names;
            for (const VkPresentModeKHR mode : modes) { names += std::string(present_mode_name(mode)) + " "; }
            ROBV_LOG_INFO("PRESENT", "present modes: {}", names);

            const bool mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
            return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
        }

        VkCompositeAlphaFlagBitsKHR pick_composite_alpha(VkCompositeAlphaFlagsKHR supported)
        {
            for (const VkCompositeAlphaFlagBitsKHR bit :
                 {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                  VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            {
                if (supported & bit) { return bit; }
            }
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }

        // clock display timing reports in on Android and Linux
        u64 monotonic_ns()
        {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
        }

        void poll_display_timing(presenter_t& p)
        {
            if (!p.display_timing || !p.swapchain) { return; }

            VkPastPresentationTimingGOOGLE timings[16];
            u32 count = 16;
            const VkResult result = vkGetPastPresentationTimingGOOGLE(p.vk->device, p.swapchain, &count, timings);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) { return; }

            for (u32 i = 0; i < count; ++i)
            {
                const VkPastPresentationTimingGOOGLE& t = timings[i];
                if (t.presentID <= p.last_shown_id) { continue; }
                p.last_shown_id = t.presentID;
                const u64 queued = p.queued_ns[t.presentID % 16];
                if (queued != 0 && t.actualPresentTime > queued)
                {
                    p.queue_to_screen_ms = static_cast<f32>(static_cast<f64>(t.actualPresentTime - queued) * 1e-6);
                }
            }
        }

        bool create_render_pass(presenter_t& p)
        {
            const VkAttachmentDescription color{
                .format = p.format.format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            };
            const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            const VkSubpassDescription subpass{
                .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                .colorAttachmentCount = 1,
                .pColorAttachments = &color_ref,
            };
            // the layout transition waits for the acquire semaphore, which the submit waits on at this stage
            const VkSubpassDependency dependency{
                .srcSubpass = VK_SUBPASS_EXTERNAL,
                .dstSubpass = 0,
                .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            };
            const VkRenderPassCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &color,
                .subpassCount = 1,
                .pSubpasses = &subpass,
                .dependencyCount = 1,
                .pDependencies = &dependency,
            };
            ROBV_VK_TRY(vkCreateRenderPass(p.vk->device, &info, nullptr, &p.render_pass));
            return true;
        }

        void destroy_swapchain_images(presenter_t& p)
        {
            const VkDevice device = p.vk->device;
            for (VkFramebuffer framebuffer : p.framebuffers) { vkDestroyFramebuffer(device, framebuffer, nullptr); }
            for (VkImageView view : p.views) { vkDestroyImageView(device, view, nullptr); }
            for (VkSemaphore semaphore : p.render_done) { vkDestroySemaphore(device, semaphore, nullptr); }
            p.framebuffers.clear();
            p.views.clear();
            p.render_done.clear();
            p.images.clear();
        }

        void destroy_swapchain(presenter_t& p)
        {
            destroy_swapchain_images(p);
            if (p.swapchain) { vkDestroySwapchainKHR(p.vk->device, p.swapchain, nullptr); }
            p.swapchain = VK_NULL_HANDLE;
            p.extent = {};
        }

        // after VK_SUBOPTIMAL_KHR: rebuild always, unless the compositor rotates, then only if the surface changed
        // Android reports a turn as a resize event before the surface updates, so the event alone can see the old size
        bool suboptimal_needs_rebuild(const presenter_t& p)
        {
            if (!p.compositor_rotates) { return true; }

            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p.vk->physical_device, p.surface, &caps) != VK_SUCCESS)
            {
                return true;
            }
            const bool size_known = caps.currentExtent.width != UINT32_MAX;
            const bool resized = size_known && (caps.currentExtent.width != p.extent.width ||
                                                caps.currentExtent.height != p.extent.height);
            return resized || caps.currentTransform != p.built_transform;
        }

        // false is not fatal: a zero sized (minimised) window has no swapchain for now
        bool build_swapchain(presenter_t& p)
        {
            const vk_context_t& vk = *p.vk;
            vkDeviceWaitIdle(vk.device);

            VkSurfaceCapabilitiesKHR caps{};
            ROBV_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, p.surface, &caps));

            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX)
            {
                int w = 0;
                int h = 0;
                SDL_GetWindowSizeInPixels(p.window, &w, &h);
                extent.width = std::clamp(static_cast<u32>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
                extent.height = std::clamp(static_cast<u32>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
            }
            if (extent.width == 0 || extent.height == 0)
            {
                destroy_swapchain(p);
                return false;
            }

            u32 image_count = std::max(caps.minImageCount + 1, 2u);
            if (caps.maxImageCount > 0) { image_count = std::min(image_count, caps.maxImageCount); }

            // identity transform, so the compositor rotates and all coordinates here stay unrotated
            // pre rotating would save that cost, but ImGui's Vulkan backend cannot draw rotated
            // revisit when robv draws the video quad itself
            const VkSurfaceTransformFlagBitsKHR transform =
                caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                 : caps.currentTransform;

            const VkSwapchainKHR old = p.swapchain;
            const VkSwapchainCreateInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                .surface = p.surface,
                .minImageCount = image_count,
                .imageFormat = p.format.format,
                .imageColorSpace = p.format.colorSpace,
                .imageExtent = extent,
                .imageArrayLayers = 1,
                .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .preTransform = transform,
                .compositeAlpha = pick_composite_alpha(caps.supportedCompositeAlpha),
                .presentMode = p.present_mode,
                .clipped = VK_TRUE,
                .oldSwapchain = old,
            };
            destroy_swapchain_images(p);
            const VkResult result = vkCreateSwapchainKHR(vk.device, &info, nullptr, &p.swapchain);
            if (old) { vkDestroySwapchainKHR(vk.device, old, nullptr); }
            if (result != VK_SUCCESS)
            {
                p.swapchain = VK_NULL_HANDLE;
                ROBV_LOG_ERROR("PRESENT", "vkCreateSwapchainKHR failed: {}", vk::result_string(result));
                return false;
            }
            p.extent = extent;
            p.min_image_count = image_count;

            u32 count = 0;
            vkGetSwapchainImagesKHR(vk.device, p.swapchain, &count, nullptr);
            p.images.resize(count);
            vkGetSwapchainImagesKHR(vk.device, p.swapchain, &count, p.images.data());

            p.views.resize(count, VK_NULL_HANDLE);
            p.framebuffers.resize(count, VK_NULL_HANDLE);
            p.render_done.resize(count, VK_NULL_HANDLE);
            for (u32 i = 0; i < count; ++i)
            {
                const VkImageViewCreateInfo view_info{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = p.images[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = p.format.format,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
                ROBV_VK_TRY(vkCreateImageView(vk.device, &view_info, nullptr, &p.views[i]));

                const VkFramebufferCreateInfo framebuffer_info{
                    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                    .renderPass = p.render_pass,
                    .attachmentCount = 1,
                    .pAttachments = &p.views[i],
                    .width = extent.width,
                    .height = extent.height,
                    .layers = 1,
                };
                ROBV_VK_TRY(vkCreateFramebuffer(vk.device, &framebuffer_info, nullptr, &p.framebuffers[i]));

                const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                ROBV_VK_TRY(vkCreateSemaphore(vk.device, &semaphore_info, nullptr, &p.render_done[i]));
            }

            p.needs_rebuild = false;
            p.last_shown_id = p.next_present_id - 1;
            p.compositor_rotates = transform != caps.currentTransform;
            p.built_transform = caps.currentTransform;
            ROBV_LOG_INFO("PRESENT", "swapchain {}x{}, {} images{}", extent.width, extent.height, count,
                          p.compositor_rotates ? ", rotated by the compositor" : "");
            return true;
        }
    } // namespace

    bool init(presenter_t& presenter, SDL_Window* window, const vk_context_t& vk, VkSurfaceKHR surface)
    {
        presenter_t& p = presenter;
        p.window = window;
        p.vk = &vk;
        p.surface = surface;
        p.format = pick_format(vk, surface);
        p.present_mode = pick_present_mode(vk, surface);
        if (p.format.format == VK_FORMAT_UNDEFINED)
        {
            ROBV_LOG_ERROR("PRESENT", "the surface reports no formats");
            return false;
        }

        p.display_timing = vk::has_device_extension(vk, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        if (!create_render_pass(p) || !vk::create_timeline(vk, p.timeline)) { return false; }

        for (presenter_frame_t& frame : p.frames)
        {
            const VkCommandPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                .queueFamilyIndex = vk.graphics.family,
            };
            ROBV_VK_TRY(vkCreateCommandPool(vk.device, &pool_info, nullptr, &frame.pool));

            const VkCommandBufferAllocateInfo cmd_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = frame.pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            ROBV_VK_TRY(vkAllocateCommandBuffers(vk.device, &cmd_info, &frame.cmd));

            const VkSemaphoreCreateInfo semaphore_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            ROBV_VK_TRY(vkCreateSemaphore(vk.device, &semaphore_info, nullptr, &frame.image_acquired));
        }

        // a minimised start is fine, the swapchain follows once the window has a size
        build_swapchain(p);
        ROBV_LOG_INFO("PRESENT", "surface format {}, present mode {}", static_cast<i32>(p.format.format),
                      present_mode_name(p.present_mode));
        return true;
    }

    void shutdown(presenter_t& presenter)
    {
        presenter_t& p = presenter;
        if (!p.vk) { return; }
        const VkDevice device = p.vk->device;

        vkDeviceWaitIdle(device);
        destroy_swapchain(p);
        for (presenter_frame_t& frame : p.frames)
        {
            if (frame.image_acquired) { vkDestroySemaphore(device, frame.image_acquired, nullptr); }
            if (frame.pool) { vkDestroyCommandPool(device, frame.pool, nullptr); }
            frame = {};
        }
        vk::destroy_timeline(*p.vk, p.timeline);
        if (p.render_pass) { vkDestroyRenderPass(device, p.render_pass, nullptr); }
        if (p.surface) { vkDestroySurfaceKHR(p.vk->instance, p.surface, nullptr); }
        p = {};
    }

    void lose_surface(presenter_t& presenter)
    {
        presenter_t& p = presenter;
        if (!p.surface) { return; }
        vkDeviceWaitIdle(p.vk->device);
        destroy_swapchain(p);
        vkDestroySurfaceKHR(p.vk->instance, p.surface, nullptr);
        p.surface = VK_NULL_HANDLE;
        ROBV_LOG_INFO("PRESENT", "surface lost");
    }

    bool restore_surface(presenter_t& presenter, VkSurfaceKHR surface)
    {
        presenter_t& p = presenter;
        p.surface = surface;

        VkBool32 can_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(p.vk->physical_device, p.vk->graphics.family, surface, &can_present);
        const VkSurfaceFormatKHR format = pick_format(*p.vk, surface);
        // render pass and ImGui pipeline are built for one format, new windows have matched so far
        if (!can_present || format.format != p.format.format)
        {
            ROBV_LOG_ERROR("PRESENT", "the new surface is incompatible (present {}, format {} vs {})", can_present,
                           static_cast<i32>(format.format), static_cast<i32>(p.format.format));
            return false;
        }
        p.needs_rebuild = true;
        ROBV_LOG_INFO("PRESENT", "surface restored");
        return true;
    }

    bool begin_frame(presenter_t& presenter, VkCommandBuffer& cmd)
    {
        presenter_t& p = presenter;
        if (!p.surface) { return false; }
        if ((p.needs_rebuild || !p.swapchain) && !build_swapchain(p)) { return false; }

        presenter_frame_t& frame = p.frames[p.frame_index];
        vk::wait(*p.vk, p.timeline, frame.submitted);

        const VkResult acquired = vkAcquireNextImageKHR(p.vk->device, p.swapchain, UINT64_MAX, frame.image_acquired,
                                                        VK_NULL_HANDLE, &p.image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            p.needs_rebuild = true;
            return false;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            ROBV_LOG_ERROR("PRESENT", "vkAcquireNextImageKHR failed: {}", vk::result_string(acquired));
            p.needs_rebuild = true;
            return false;
        }
        // suboptimal still presents this frame, the rebuild follows
        if (acquired == VK_SUBOPTIMAL_KHR && suboptimal_needs_rebuild(p)) { p.needs_rebuild = true; }

        vkResetCommandPool(p.vk->device, frame.pool, 0);
        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(frame.cmd, &begin_info);

        cmd = frame.cmd;
        return true;
    }

    void begin_pass(presenter_t& presenter, const f32 clear_color[4])
    {
        presenter_t& p = presenter;
        presenter_frame_t& frame = p.frames[p.frame_index];

        VkClearValue clear{};
        for (u32 i = 0; i < 4; ++i) { clear.color.float32[i] = clear_color[i]; }
        const VkRenderPassBeginInfo pass_info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = p.render_pass,
            .framebuffer = p.framebuffers[p.image_index],
            .renderArea = {{0, 0}, p.extent},
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(frame.cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
    }

    void end_frame(presenter_t& presenter)
    {
        presenter_t& p = presenter;
        presenter_frame_t& frame = p.frames[p.frame_index];

        vkCmdEndRenderPass(frame.cmd);
        vkEndCommandBuffer(frame.cmd);

        // binary semaphores for the swapchain, the timeline for the CPU, one submit signals both
        frame.submitted = vk::next_value(p.timeline);
        const VkSemaphore signals[2] = {p.render_done[p.image_index], p.timeline.semaphore};
        const u64 signal_values[2] = {0, frame.submitted};
        const VkTimelineSemaphoreSubmitInfo timeline_info{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = 2,
            .pSignalSemaphoreValues = signal_values,
        };
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline_info,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &frame.image_acquired,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &frame.cmd,
            .signalSemaphoreCount = 2,
            .pSignalSemaphores = signals,
        };
        const VkResult submitted = vkQueueSubmit(p.vk->graphics.queue, 1, &submit, VK_NULL_HANDLE);
        if (submitted != VK_SUCCESS)
        {
            ROBV_LOG_ERROR("PRESENT", "vkQueueSubmit failed: {}", vk::result_string(submitted));
        }

        const VkPresentTimeGOOGLE present_time{.presentID = p.next_present_id, .desiredPresentTime = 0};
        const VkPresentTimesInfoGOOGLE present_times{
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
            .swapchainCount = 1,
            .pTimes = &present_time,
        };
        if (p.display_timing) { p.queued_ns[p.next_present_id % 16] = monotonic_ns(); }
        const VkPresentInfoKHR present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = p.display_timing ? &present_times : nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &p.render_done[p.image_index],
            .swapchainCount = 1,
            .pSwapchains = &p.swapchain,
            .pImageIndices = &p.image_index,
        };
        const VkResult presented = vkQueuePresentKHR(p.vk->graphics.queue, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || (presented == VK_SUBOPTIMAL_KHR && suboptimal_needs_rebuild(p)))
        {
            p.needs_rebuild = true;
        }
        else if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        {
            ROBV_LOG_ERROR("PRESENT", "vkQueuePresentKHR failed: {}", vk::result_string(presented));
        }

        ++p.presented;
        if (p.display_timing) { ++p.next_present_id; }
        p.frame_index = (p.frame_index + 1) % frames_in_flight;
    }

    void request_rebuild(presenter_t& presenter) { presenter.needs_rebuild = true; }

    u32 frames_queued(presenter_t& presenter)
    {
        poll_display_timing(presenter);
        if (!presenter.display_timing) { return 0; }
        return presenter.next_present_id - 1 - presenter.last_shown_id;
    }
} // namespace robv::presenter
