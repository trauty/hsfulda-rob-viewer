#pragma once

#include "robv/client/presenter.h"
#include "robv/common/video.h"
#include "robv/vk/buffer.h"

#if defined(ROBV_PLATFORM_ANDROID)
    #include <android/hardware_buffer.h>
#endif

// shows decoded frames letterboxed before ImGui, sampled through a VkSamplerYcbcrConversion
// CPU frames (ffmpeg, null) go through a staging buffer into one NV12 image
// Android's decoder output is imported as is (zero copy), in whatever format the driver says it is (found this out the
// hard way lol)
namespace robv
{
    struct video_view_t
    {
        const vk_context_t* vk = nullptr;
        VkRenderPass render_pass = VK_NULL_HANDLE;
        bool available = false;
        // the newest picture came as a hardware buffer (Android), not through upload
        bool showing_external = false;
        void* external = nullptr;

        VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkExtent2D extent{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // a frame slots staging buffer is free once its previous submit finished, see presenter::begin_frame
        vk_buffer_t staging[frames_in_flight];
        u64 uploads = 0;
    };
} // namespace robv

namespace robv::video_view
{
    // false only on a broken device, missing YCbCr support leaves it unavailable and logs once
    bool init(video_view_t& view, const vk_context_t& vk, VkRenderPass render_pass);
    void shutdown(video_view_t& view);

    // outside the render pass, between presenter::begin_frame and presenter::begin_pass
    void upload(video_view_t& view, VkCommandBuffer cmd, u32 frame_slot, const planar_frame_t& frame);
#if defined(ROBV_PLATFORM_ANDROID)
    // zero copy: a decoders output buffer becomes the shown picture, outside the render pass like upload
    // the buffer must stay valid until the frames drawing it finished on the GPU
    // width and height are from picture, the buffer may be padded
    bool show_hardware_buffer(video_view_t& view, VkCommandBuffer cmd, AHardwareBuffer* buffer, u32 width, u32 height);
    void forget_hardware_buffers(video_view_t& view);
#endif

    void draw(video_view_t& view, VkCommandBuffer cmd, VkExtent2D target);
} // namespace robv::video_view
