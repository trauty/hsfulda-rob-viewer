#include "robv/client/video_view.h"

#include "robv/shaders/video.h"
#include "robv/vk/shader.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace robv::video_view
{
    namespace
    {
        constexpr VkFormat nv12_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

        // conversion and sampler are fixed for the format, so they are made once and baked into the set layout
        bool create_conversion(video_view_t& v)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(v.vk->physical_device, nv12_format, &props);
            const VkFormatFeatureFlags features = props.optimalTilingFeatures;
            const VkFormatFeatureFlags needed =
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            const bool midpoint = features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
            const bool cosited = features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
            if ((features & needed) != needed || (!midpoint && !cosited))
            {
                ROBV_LOG_WARN("VIDEO", "NV12 cannot be sampled on this device, no video");
                return false;
            }

            // with no separate reconstruction filter the sampler's filters must match the chroma filter
            const VkFilter filter = features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT
                                        ? VK_FILTER_LINEAR
                                        : VK_FILTER_NEAREST;
            // rgb_to_nv12.slang averages 2x2 blocks, so chroma sits between the luma samples
            const VkChromaLocation location = midpoint ? VK_CHROMA_LOCATION_MIDPOINT : VK_CHROMA_LOCATION_COSITED_EVEN;

            const VkSamplerYcbcrConversionCreateInfo conversion_info{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                .format = nv12_format,
                .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .xChromaOffset = location,
                .yChromaOffset = location,
                .chromaFilter = filter,
                .forceExplicitReconstruction = VK_FALSE,
            };
            ROBV_VK_TRY(vkCreateSamplerYcbcrConversion(v.vk->device, &conversion_info, nullptr, &v.conversion));

            const VkSamplerYcbcrConversionInfo conversion_ref{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .conversion = v.conversion,
            };
            const VkSamplerCreateInfo sampler_info{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = &conversion_ref,
                .magFilter = filter,
                .minFilter = filter,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .maxLod = 0.0f,
                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            };
            ROBV_VK_TRY(vkCreateSampler(v.vk->device, &sampler_info, nullptr, &v.sampler));
            return true;
        }

        bool create_descriptors(video_view_t& v)
        {
            const VkDescriptorSetLayoutBinding binding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = &v.sampler,
            };
            const VkDescriptorSetLayoutCreateInfo layout_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 1,
                .pBindings = &binding,
            };
            ROBV_VK_TRY(vkCreateDescriptorSetLayout(v.vk->device, &layout_info, nullptr, &v.set_layout));

            // a multi planar image may take one descriptor per plane, so room for three
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
            const VkDescriptorPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &size,
            };
            ROBV_VK_TRY(vkCreateDescriptorPool(v.vk->device, &pool_info, nullptr, &v.pool));

            const VkDescriptorSetAllocateInfo set_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = v.pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &v.set_layout,
            };
            ROBV_VK_TRY(vkAllocateDescriptorSets(v.vk->device, &set_info, &v.set));
            return true;
        }

        // one pipeline per sampler: an immutable YCbCr sampler is part of the set layout
        bool create_pipeline(const vk_context_t& vk, VkRenderPass render_pass, VkDescriptorSetLayout set_layout,
                             VkPipelineLayout& layout_out, VkPipeline& pipeline_out)
        {
            const VkPipelineLayoutCreateInfo layout_info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &set_layout,
            };
            ROBV_VK_TRY(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout_out));

            const VkShaderModule module = vk::create_shader_module(vk, shaders::video);
            if (!module) { return false; }

            const VkPipelineShaderStageCreateInfo stages[2] = {
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                 .module = module,
                 .pName = "vertex_main"  },
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                 .module = module,
                 .pName = "fragment_main"},
            };
            const VkPipelineVertexInputStateCreateInfo vertex_input{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            const VkPipelineInputAssemblyStateCreateInfo input_assembly{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            };
            const VkPipelineViewportStateCreateInfo viewport{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .viewportCount = 1,
                .scissorCount = 1,
            };
            const VkPipelineRasterizationStateCreateInfo raster{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .cullMode = VK_CULL_MODE_NONE,
                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                .lineWidth = 1.0f,
            };
            const VkPipelineMultisampleStateCreateInfo multisample{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            };
            const VkPipelineColorBlendAttachmentState blend_attachment{
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                  VK_COLOR_COMPONENT_A_BIT,
            };
            const VkPipelineColorBlendStateCreateInfo blend{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &blend_attachment,
            };
            const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            const VkPipelineDynamicStateCreateInfo dynamic{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                .dynamicStateCount = 2,
                .pDynamicStates = dynamic_states,
            };
            const VkGraphicsPipelineCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .stageCount = 2,
                .pStages = stages,
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisample,
                .pColorBlendState = &blend,
                .pDynamicState = &dynamic,
                .layout = layout_out,
                .renderPass = render_pass,
                .subpass = 0,
            };
            const VkResult result =
                vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_out);
            vkDestroyShaderModule(vk.device, module, nullptr);
            if (result != VK_SUCCESS)
            {
                ROBV_LOG_ERROR("VIDEO", "vkCreateGraphicsPipelines failed: {}", vk::result_string(result));
                return false;
            }
            return true;
        }

        void destroy_image(video_view_t& v)
        {
            if (v.view) { vkDestroyImageView(v.vk->device, v.view, nullptr); }
            if (v.image) { vmaDestroyImage(v.vk->allocator, v.image, v.allocation); }
            v.view = VK_NULL_HANDLE;
            v.image = VK_NULL_HANDLE;
            v.allocation = VK_NULL_HANDLE;
            v.extent = {};
            v.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        // the view must carry the conversion too, so vk::create_image does not fit
        bool create_image(video_view_t& v, u32 width, u32 height)
        {
            // the set and the old image may still be in use by frames in flight
            vkDeviceWaitIdle(v.vk->device);
            destroy_image(v);

            const VkImageCreateInfo image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = nv12_format,
                .extent = {width, height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
            ROBV_VK_TRY(vmaCreateImage(v.vk->allocator, &image_info, &alloc_info, &v.image, &v.allocation, nullptr));

            const VkSamplerYcbcrConversionInfo conversion_ref{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .conversion = v.conversion,
            };
            const VkImageViewCreateInfo view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = &conversion_ref,
                .image = v.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = nv12_format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            ROBV_VK_TRY(vkCreateImageView(v.vk->device, &view_info, nullptr, &v.view));

            const VkDescriptorImageInfo image_ref{
                .imageView = v.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = v.set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_ref,
            };
            vkUpdateDescriptorSets(v.vk->device, 1, &write, 0, nullptr);

            v.extent = {width, height};
            ROBV_LOG_INFO("VIDEO", "NV12 image {}x{}", width, height);
            return true;
        }

        void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                     VkAccessFlags dst_access)
        {
            const VkImageMemoryBarrier b{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = src_access,
                .dstAccessMask = dst_access,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
    } // namespace

    namespace
    {
        // largest rectangle with the video's aspect, centred, the clear colour fills the bars
        void draw_quad(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                       VkExtent2D video, VkExtent2D target)
        {
            const f32 scale = std::min(static_cast<f32>(target.width) / static_cast<f32>(video.width),
                                       static_cast<f32>(target.height) / static_cast<f32>(video.height));
            const f32 width = static_cast<f32>(video.width) * scale;
            const f32 height = static_cast<f32>(video.height) * scale;
            const VkViewport viewport{
                .x = (static_cast<f32>(target.width) - width) * 0.5f,
                .y = (static_cast<f32>(target.height) - height) * 0.5f,
                .width = width,
                .height = height,
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            };
            const VkRect2D scissor{
                {0, 0},
                target
            };

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

#if defined(ROBV_PLATFORM_ANDROID)
        // zero copy: decoder output buffers (AHardwareBuffer) imported as Vulkan images
        // the driver knows the vendor layout (MediaTek outputs a tiled format a CPU copy cannot read as already
        // mentioned), so it supplies an external format, and the conversion and pipeline are built for that format the
        // decoder recycles a handful of buffers, each is imported once and kept until the decoder changes
        constexpr size_t max_external_images = 8;

        struct external_image_t
        {
            AHardwareBuffer* buffer = nullptr;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkExtent2D extent{};
        };

        struct external_t
        {
            u64 external_format = 0;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            std::vector<external_image_t> images;
            i32 current = -1;
            bool foreign_queue = false;
        };

        void destroy_external_images(const vk_context_t& vk, external_t& e)
        {
            for (external_image_t& image : e.images)
            {
                if (image.view) { vkDestroyImageView(vk.device, image.view, nullptr); }
                if (image.image) { vkDestroyImage(vk.device, image.image, nullptr); }
                if (image.memory) { vkFreeMemory(vk.device, image.memory, nullptr); }
            }
            e.images.clear();
            e.current = -1;
            if (e.pool) { vkResetDescriptorPool(vk.device, e.pool, 0); }
        }

        void destroy_external_path(const vk_context_t& vk, external_t& e)
        {
            destroy_external_images(vk, e);
            if (e.pipeline) { vkDestroyPipeline(vk.device, e.pipeline, nullptr); }
            if (e.pipeline_layout) { vkDestroyPipelineLayout(vk.device, e.pipeline_layout, nullptr); }
            if (e.pool) { vkDestroyDescriptorPool(vk.device, e.pool, nullptr); }
            if (e.set_layout) { vkDestroyDescriptorSetLayout(vk.device, e.set_layout, nullptr); }
            if (e.sampler) { vkDestroySampler(vk.device, e.sampler, nullptr); }
            if (e.conversion) { vkDestroySamplerYcbcrConversion(vk.device, e.conversion, nullptr); }
            const bool foreign = e.foreign_queue;
            e = {};
            e.foreign_queue = foreign;
        }

        // the stream is BT.709 limited range (rgb_to_nv12.slang), the driver's suggestion is a guess without a
        // dataspace
        bool build_external_path(const video_view_t& v, external_t& e,
                                 const VkAndroidHardwareBufferFormatPropertiesANDROID& format)
        {
            const vk_context_t& vk = *v.vk;
            const VkExternalFormatANDROID external{
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
                .externalFormat = format.format == VK_FORMAT_UNDEFINED ? format.externalFormat : 0,
            };
            const bool linear =
                format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
            const VkFilter filter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            const VkSamplerYcbcrConversionCreateInfo conversion_info{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                .pNext = &external,
                .format = format.format,
                .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
                .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
                .components = format.samplerYcbcrConversionComponents,
                .xChromaOffset = format.suggestedXChromaOffset,
                .yChromaOffset = format.suggestedYChromaOffset,
                .chromaFilter = filter,
                .forceExplicitReconstruction = VK_FALSE,
            };
            ROBV_VK_TRY(vkCreateSamplerYcbcrConversion(vk.device, &conversion_info, nullptr, &e.conversion));

            const VkSamplerYcbcrConversionInfo conversion_ref{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .conversion = e.conversion,
            };
            const VkSamplerCreateInfo sampler_info{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = &conversion_ref,
                .magFilter = filter,
                .minFilter = filter,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .maxLod = 0.0f,
                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            };
            ROBV_VK_TRY(vkCreateSampler(vk.device, &sampler_info, nullptr, &e.sampler));

            const VkDescriptorSetLayoutBinding binding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = &e.sampler,
            };
            const VkDescriptorSetLayoutCreateInfo layout_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 1,
                .pBindings = &binding,
            };
            ROBV_VK_TRY(vkCreateDescriptorSetLayout(vk.device, &layout_info, nullptr, &e.set_layout));

            // a multi planar image may take one descriptor per plane
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            static_cast<u32>(max_external_images * 3)};
            const VkDescriptorPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = static_cast<u32>(max_external_images),
                .poolSizeCount = 1,
                .pPoolSizes = &size,
            };
            ROBV_VK_TRY(vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &e.pool));

            if (!create_pipeline(vk, v.render_pass, e.set_layout, e.pipeline_layout, e.pipeline)) { return false; }
            e.external_format = format.externalFormat;
            e.format = format.format;
            ROBV_LOG_INFO("VIDEO", "zero copy video, external format {:#x}, vk format {}, {} chroma filter",
                          format.externalFormat, static_cast<i32>(format.format), linear ? "linear" : "nearest");
            return true;
        }

        bool import_buffer(const video_view_t& v, external_t& e, AHardwareBuffer* buffer,
                           const VkAndroidHardwareBufferPropertiesANDROID& props, external_image_t& out)
        {
            const vk_context_t& vk = *v.vk;
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(buffer, &desc);
            out.buffer = buffer;
            out.extent = {desc.width, desc.height};

            const VkExternalFormatANDROID external{
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
                .externalFormat = e.format == VK_FORMAT_UNDEFINED ? e.external_format : 0,
            };
            const VkExternalMemoryImageCreateInfo external_memory{
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .pNext = &external,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
            };
            const VkImageCreateInfo image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = &external_memory,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = e.format,
                .extent = {desc.width, desc.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            ROBV_VK_TRY(vkCreateImage(vk.device, &image_info, nullptr, &out.image));

            // the import holds its own reference to the buffer
            const VkImportAndroidHardwareBufferInfoANDROID import{
                .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
                .buffer = buffer,
            };
            const VkMemoryDedicatedAllocateInfo dedicated{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .pNext = &import,
                .image = out.image,
            };
            u32 type = 0;
            while (type < 32 && !(props.memoryTypeBits & (1u << type))) { ++type; }
            const VkMemoryAllocateInfo alloc_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated,
                .allocationSize = props.allocationSize,
                .memoryTypeIndex = type,
            };
            ROBV_VK_TRY(vkAllocateMemory(vk.device, &alloc_info, nullptr, &out.memory));
            ROBV_VK_TRY(vkBindImageMemory(vk.device, out.image, out.memory, 0));

            const VkSamplerYcbcrConversionInfo conversion_ref{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                .conversion = e.conversion,
            };
            const VkImageViewCreateInfo view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = &conversion_ref,
                .image = out.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = e.format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            ROBV_VK_TRY(vkCreateImageView(vk.device, &view_info, nullptr, &out.view));

            const VkDescriptorSetAllocateInfo set_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = e.pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &e.set_layout,
            };
            ROBV_VK_TRY(vkAllocateDescriptorSets(vk.device, &set_info, &out.set));
            const VkDescriptorImageInfo image_ref{
                .imageView = out.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = out.set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_ref,
            };
            vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
            return true;
        }

        void draw_external(video_view_t& v, VkCommandBuffer cmd, VkExtent2D target)
        {
            external_t* e = static_cast<external_t*>(v.external);
            if (!e || e->current < 0) { return; }
            const external_image_t& image = e->images[static_cast<size_t>(e->current)];
            draw_quad(cmd, e->pipeline, e->pipeline_layout, image.set, v.extent, target);
        }
#endif
    } // namespace

    bool init(video_view_t& view, const vk_context_t& vk, VkRenderPass render_pass)
    {
        video_view_t& v = view;
        v = {};
        v.vk = &vk;
        v.render_pass = render_pass;

        if (!vk.ycbcr_conversion)
        {
            ROBV_LOG_WARN("VIDEO", "samplerYcbcrConversion is not supported, no video");
            return true;
        }
        if (!create_conversion(v)) { return true; }
        if (!create_descriptors(v) || !create_pipeline(vk, render_pass, v.set_layout, v.pipeline_layout, v.pipeline))
        {
            return false;
        }

        v.available = true;
        return true;
    }

    void shutdown(video_view_t& view)
    {
        video_view_t& v = view;
        if (!v.vk) { return; }
        const VkDevice device = v.vk->device;

        destroy_image(v);
#if defined(ROBV_PLATFORM_ANDROID)
        if (external_t* e = static_cast<external_t*>(v.external))
        {
            destroy_external_path(*v.vk, *e);
            delete e;
        }
#endif
        for (vk_buffer_t& buffer : v.staging) { vk::destroy_buffer(*v.vk, buffer); }
        if (v.pipeline) { vkDestroyPipeline(device, v.pipeline, nullptr); }
        if (v.pipeline_layout) { vkDestroyPipelineLayout(device, v.pipeline_layout, nullptr); }
        if (v.pool) { vkDestroyDescriptorPool(device, v.pool, nullptr); }
        if (v.set_layout) { vkDestroyDescriptorSetLayout(device, v.set_layout, nullptr); }
        if (v.sampler) { vkDestroySampler(device, v.sampler, nullptr); }
        if (v.conversion) { vkDestroySamplerYcbcrConversion(device, v.conversion, nullptr); }
        v = {};
    }

    void upload(video_view_t& view, VkCommandBuffer cmd, u32 frame_slot, const planar_frame_t& frame)
    {
        video_view_t& v = view;
        if (!v.available || frame.data.empty() || frame.format != pixel_format_e::nv12) { return; }

        if ((v.extent.width != frame.width || v.extent.height != frame.height) &&
            !create_image(v, frame.width, frame.height))
        {
            destroy_image(v);
            return;
        }

        vk_buffer_t& staging = v.staging[frame_slot % frames_in_flight];
        if (staging.size < frame.data.size())
        {
            vk::destroy_buffer(*v.vk, staging);
            if (!vk::create_buffer(*v.vk, frame.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vk_memory_e::upload,
                                   staging))
            {
                return;
            }
        }
        std::memcpy(staging.mapped, frame.data.data(), frame.data.size());
        vmaFlushAllocation(v.vk->allocator, staging.allocation, 0, frame.data.size());

        // the previous frame may still sample the image, an execution dependency is enough for that
        const bool fresh = v.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        barrier(cmd, v.image, v.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

        // row lengths are in texels, a chroma texel is two bytes
        const VkBufferImageCopy planes[2] = {
            {.bufferOffset = 0,
             .bufferRowLength = frame.y_stride,
             .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
             .imageExtent = {frame.width, frame.height, 1}        },
            {.bufferOffset = static_cast<VkDeviceSize>(frame.y_stride) * frame.height,
             .bufferRowLength = frame.uv_stride / 2,
             .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
             .imageExtent = {frame.width / 2, frame.height / 2, 1}},
        };
        vkCmdCopyBufferToImage(cmd, staging.buffer, v.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, planes);

        barrier(cmd, v.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        v.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        v.showing_external = false;
        ++v.uploads;
    }

#if defined(ROBV_PLATFORM_ANDROID)
    bool show_hardware_buffer(video_view_t& view, VkCommandBuffer cmd, AHardwareBuffer* buffer, u32 width, u32 height)
    {
        video_view_t& v = view;
        if (!v.vk || !v.vk->ycbcr_conversion || !buffer) { return false; }
        const vk_context_t& vk = *v.vk;
        if (!v.external)
        {
            if (!vk::has_device_extension(vk, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME))
            {
                ROBV_LOG_ERROR("VIDEO", "{} is missing, no zero copy video",
                               VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
                return false;
            }
            external_t* e = new external_t;
            e->foreign_queue = vk::has_device_extension(vk, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            v.external = e;
        }
        external_t& e = *static_cast<external_t*>(v.external);

        i32 index = -1;
        for (size_t i = 0; i < e.images.size(); ++i)
        {
            if (e.images[i].buffer == buffer) { index = static_cast<i32>(i); }
        }

        if (index < 0)
        {
            VkAndroidHardwareBufferFormatPropertiesANDROID format{
                .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
            VkAndroidHardwareBufferPropertiesANDROID props{
                .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &format};
            if (vkGetAndroidHardwareBufferPropertiesANDROID(vk.device, buffer, &props) != VK_SUCCESS)
            {
                ROBV_LOG_ERROR("VIDEO", "vkGetAndroidHardwareBufferPropertiesANDROID failed");
                return false;
            }

            // a new format (stream size change) or a full cache: whatever still uses the old images must finish first
            const bool new_format =
                !e.pipeline || format.externalFormat != e.external_format || format.format != e.format;
            if (new_format || e.images.size() >= max_external_images)
            {
                vkDeviceWaitIdle(vk.device);
                if (new_format)
                {
                    destroy_external_path(vk, e);
                    if (!build_external_path(v, e, format)) { return false; }
                }
                else
                {
                    destroy_external_images(vk, e);
                }
            }

            external_image_t image;
            if (!import_buffer(v, e, buffer, props, image))
            {
                if (image.view) { vkDestroyImageView(vk.device, image.view, nullptr); }
                if (image.image) { vkDestroyImage(vk.device, image.image, nullptr); }
                if (image.memory) { vkFreeMemory(vk.device, image.memory, nullptr); }
                return false;
            }
            e.images.push_back(image);
            index = static_cast<i32>(e.images.size() - 1);
        }

        // the decoder wrote it outside Vulkan: acquire it from the foreign queue, old contents are the new picture
        const VkImageMemoryBarrier acquire{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = e.foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = e.foreign_queue ? vk.graphics.family : VK_QUEUE_FAMILY_IGNORED,
            .image = e.images[static_cast<size_t>(index)].image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &acquire);

        e.current = index;
        v.extent = {width, height};
        v.showing_external = true;
        ++v.uploads;
        return true;
    }

    void forget_hardware_buffers(video_view_t& view)
    {
        video_view_t& v = view;
        external_t* e = static_cast<external_t*>(v.external);
        if (!v.vk || !e) { return; }
        vkDeviceWaitIdle(v.vk->device);
        destroy_external_images(*v.vk, *e);
        v.showing_external = false;
    }
#endif

    void draw(video_view_t& view, VkCommandBuffer cmd, VkExtent2D target)
    {
        video_view_t& v = view;
        if (!v.available || target.width == 0 || target.height == 0) { return; }
#if defined(ROBV_PLATFORM_ANDROID)
        if (v.showing_external)
        {
            draw_external(v, cmd, target);
            return;
        }
#endif
        if (v.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) { return; }
        draw_quad(cmd, v.pipeline, v.pipeline_layout, v.set, v.extent, target);
    }
} // namespace robv::video_view
