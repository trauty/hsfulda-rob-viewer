#pragma once

#include "robv/vk/context.h"
#include "robv/vk/frame.h"

#include <vector>

struct SDL_Window;

namespace robv
{
    inline constexpr u32 frames_in_flight = 2;

    struct presenter_frame_t
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_acquired = VK_NULL_HANDLE;
        // timeline value of the last submit, the slot is free once reached
        u64 submitted = 0;
    };

    // surface, swapchain and a clearing render pass
    // Android destroys the window in the background, so the surface comes and goes
    struct presenter_t
    {
        SDL_Window* window = nullptr;
        const vk_context_t* vk = nullptr;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR format{};
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D extent{};
        u32 min_image_count = 0;
        VkRenderPass render_pass = VK_NULL_HANDLE;

        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
        // per swapchain image, presentation can outlive the frame slot
        std::vector<VkSemaphore> render_done;

        presenter_frame_t frames[frames_in_flight];
        vk_timeline_t timeline;
        u32 frame_index = 0;
        u32 image_index = 0;
        bool needs_rebuild = false;
        // compositor rotates the swapchain (Android, device turned)
        // presentation then returns VK_SUBOPTIMAL_KHR forever
        // so the surface transform at build time separates a real change from the steady state
        bool compositor_rotates = false;
        VkSurfaceTransformFlagBitsKHR built_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        u64 presented = 0;

        // VK_GOOGLE_display_timing: when each present reached the screen
        // Android queues every present, MAILBOX too, so a once per vsync client keeps its backlog
        // the queue depth lets it skip a vsync, see presenter::frames_queued
        bool display_timing = false;
        u32 next_present_id = 1;
        u32 last_shown_id = 0;
        // CLOCK_MONOTONIC queue time of recent presents, by present id % 16
        u64 queued_ns[16] = {};
        f32 queue_to_screen_ms = 0.0f;
    };
} // namespace robv

namespace robv::presenter
{
    // needs an existing instance, surface made from it for window
    bool init(presenter_t& presenter, SDL_Window* window, const vk_context_t& vk, VkSurfaceKHR surface);
    void shutdown(presenter_t& presenter);

    // Android background and foreground: drops or recreates surface and swapchain
    void lose_surface(presenter_t& presenter);
    bool restore_surface(presenter_t& presenter, VkSurfaceKHR surface);

    // false when there is nothing to draw into (minimised, no surface, rebuilding)
    // on true, cmd records inside the render pass until end_frame
    bool begin_frame(presenter_t& presenter, const f32 clear_color[4], VkCommandBuffer& cmd);
    void end_frame(presenter_t& presenter);

    // call on resize, rebuilds at the next begin_frame
    void request_rebuild(presenter_t& presenter);

    // presented but not yet reported on screen, 0 without display timing
    u32 frames_queued(presenter_t& presenter);
} // namespace robv::presenter
