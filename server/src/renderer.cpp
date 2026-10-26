#include "robv/server/renderer.h"

#include "robv/common/log.h"
#include "robv/shaders/lines.h"
#include "robv/shaders/points.h"
#include "robv/shaders/rgb_to_nv12.h"
#include "robv/vk/buffer.h"
#include "robv/vk/frame.h"
#include "robv/vk/image.h"
#include "robv/vk/shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

namespace robv::renderer
{
    namespace
    {
        // white bar along the bottom, length is the point count
        void draw_cloud_bar(planar_frame_t& frame, const point_cloud_frame_t* cloud)
        {
            if (!cloud) { return; }

            const u32 bar_height = std::max(2u, frame.height / 40) & ~1u;
            const f64 fill =
                std::clamp(std::log10(static_cast<f64>(point_cloud::point_count(*cloud)) + 1.0) / 7.0, 0.0, 1.0);
            const u32 bar_width = static_cast<u32>(fill * frame.width) & ~1u;

            u8* y = video::y_plane(frame);
            u8* uv = video::uv_plane(frame);
            for (u32 row = frame.height - bar_height; row < frame.height; ++row)
            {
                std::memset(y + static_cast<size_t>(row) * frame.y_stride, 235, bar_width);
            }
            for (u32 row = (frame.height - bar_height) / 2; row < frame.height / 2; ++row)
            {
                std::memset(uv + static_cast<size_t>(row) * frame.uv_stride, 128, bar_width);
            }
        }

        // null: colour bars scrolled by yaw, tinted by distance, so any intent visibly changes the stream
        void draw_null(const point_cloud_frame_t* cloud, const view_state_t& state, planar_frame_t& out)
        {
            const f32 phase = (state.camera.yaw + PI) / TAU;
            const f32 hue = std::log2(std::max(state.camera.distance, 0.1f)) * 0.1f +
                            static_cast<f32>(state.render.color_mode) * 0.25f;
            video::fill_test_pattern(out, phase, hue);
            draw_cloud_bar(out, cloud);
        }

        // keep in sync with lines.slang
        constexpr u32 grid_half_lines = 20;
        constexpr u32 grid_vertices = (grid_half_lines * 2 + 1) * 4;
        constexpr u32 axes_vertices = 6;

        constexpr VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;

        constexpr VkClearColorValue background = {
            {0.05f, 0.055f, 0.065f, 1.0f}
        };

        // keep in sync with points.slang
        struct points_constants_t
        {
            glm::mat4 view_projection{1.0f};
            cloud_gpu_layout_t layout;
            u32 width = 0;
            u32 row_stride = 0;
            u32 color_mode = 0;
            f32 point_size = 1.0f;
            f32 intensity_min = 0.0f;
            f32 intensity_max = 1.0f;
            f32 height_min = 0.0f;
            f32 height_max = 1.0f;
        };
        static_assert(sizeof(points_constants_t) == 120, "the Vulkan minimum for push constants is 128 bytes");

        // keep in sync with lines.slang
        struct lines_constants_t
        {
            glm::mat4 view_projection{1.0f};
            glm::vec2 grid_center{0.0f};
        };

        struct gpu_t
        {
            VkFormat depth_format = VK_FORMAT_UNDEFINED;
            f32 max_point_size = 1.0f;

            VkRenderPass render_pass = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            vk_image_t color;
            vk_image_t depth;
            vk_image_t y_plane;
            vk_image_t uv_plane;
            vk_buffer_t readback;

            VkDescriptorSetLayout points_set_layout = VK_NULL_HANDLE;
            VkDescriptorSetLayout nv12_set_layout = VK_NULL_HANDLE;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet points_set = VK_NULL_HANDLE;
            VkDescriptorSet nv12_set = VK_NULL_HANDLE;
            VkPipelineLayout draw_layout = VK_NULL_HANDLE;
            VkPipelineLayout nv12_layout = VK_NULL_HANDLE;
            VkPipeline points_pipeline = VK_NULL_HANDLE;
            VkPipeline lines_pipeline = VK_NULL_HANDLE;
            VkPipeline nv12_pipeline = VK_NULL_HANDLE;

            VkCommandPool cmd_pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vk_timeline_t timeline;

            vk_buffer_t points;
            std::shared_ptr<const std::vector<std::byte>> uploaded;
            bool drawable = false;
            cloud_gpu_layout_t layout;
            u32 width = 0;
            u32 row_stride = 0;
            u32 point_count = 0;
            f32 height_min = 0.0f;
            f32 height_max = 1.0f;
        };

        bool supports(const vk_context_t& vk, VkFormat format, VkFormatFeatureFlags needed)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(vk.physical_device, format, &props);
            return (props.optimalTilingFeatures & needed) == needed;
        }

        VkFormat pick_depth_format(const vk_context_t& vk)
        {
            for (const VkFormat format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM})
            {
                if (supports(vk, format, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) { return format; }
            }
            return VK_FORMAT_UNDEFINED;
        }

        bool create_targets(const vk_context_t& vk, gpu_t& g, u32 width, u32 height)
        {
            const VkExtent2D extent{width, height};
            const VkExtent2D chroma{width / 2, height / 2};
            return vk::create_image(vk,
                                    {.extent = extent,
                                     .format = color_format,
                                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT},
                                    g.color) &&
                   vk::create_image(vk,
                                    {.extent = extent,
                                     .format = g.depth_format,
                                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                     .aspect = VK_IMAGE_ASPECT_DEPTH_BIT},
                                    g.depth) &&
                   vk::create_image(vk,
                                    {.extent = extent,
                                     .format = VK_FORMAT_R8_UNORM,
                                     .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
                                    g.y_plane) &&
                   vk::create_image(vk,
                                    {.extent = chroma,
                                     .format = VK_FORMAT_R8G8_UNORM,
                                     .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
                                    g.uv_plane) &&
                   vk::create_buffer(vk, static_cast<VkDeviceSize>(width) * height * 3 / 2,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk_memory_e::readback, g.readback);
        }

        bool create_render_pass(const vk_context_t& vk, gpu_t& g, u32 width, u32 height)
        {
            const VkAttachmentDescription attachments[2] = {
                {
                 .format = color_format,
                 .samples = VK_SAMPLE_COUNT_1_BIT,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                 // the NV12 conversion reads it as a storage image
                    .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
                 },
                {
                 .format = g.depth_format,
                 .samples = VK_SAMPLE_COUNT_1_BIT,
                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                 .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                 .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 },
            };
            const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            const VkSubpassDescription subpass{
                .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                .colorAttachmentCount = 1,
                .pColorAttachments = &color_ref,
                .pDepthStencilAttachment = &depth_ref,
            };
            // the conversion that reads the colour image runs after the pass
            const VkSubpassDependency dependency{
                .srcSubpass = 0,
                .dstSubpass = VK_SUBPASS_EXTERNAL,
                .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            };
            const VkRenderPassCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                .attachmentCount = 2,
                .pAttachments = attachments,
                .subpassCount = 1,
                .pSubpasses = &subpass,
                .dependencyCount = 1,
                .pDependencies = &dependency,
            };
            ROBV_VK_TRY(vkCreateRenderPass(vk.device, &info, nullptr, &g.render_pass));

            const VkImageView views[2] = {g.color.view, g.depth.view};
            const VkFramebufferCreateInfo framebuffer_info{
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = g.render_pass,
                .attachmentCount = 2,
                .pAttachments = views,
                .width = width,
                .height = height,
                .layers = 1,
            };
            ROBV_VK_TRY(vkCreateFramebuffer(vk.device, &framebuffer_info, nullptr, &g.framebuffer));
            return true;
        }

        void write_points_set(const vk_context_t& vk, gpu_t& g)
        {
            const VkDescriptorBufferInfo buffer{g.points.buffer, 0, VK_WHOLE_SIZE};
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = g.points_set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffer,
            };
            vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
        }

        bool create_descriptors(const vk_context_t& vk, gpu_t& g)
        {
            const VkDescriptorSetLayoutBinding points_binding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            };
            const VkDescriptorSetLayoutCreateInfo points_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 1,
                .pBindings = &points_binding,
            };
            ROBV_VK_TRY(vkCreateDescriptorSetLayout(vk.device, &points_info, nullptr, &g.points_set_layout));

            VkDescriptorSetLayoutBinding nv12_bindings[3];
            for (u32 i = 0; i < 3; ++i)
            {
                nv12_bindings[i] = {
                    .binding = i,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                };
            }
            const VkDescriptorSetLayoutCreateInfo nv12_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 3,
                .pBindings = nv12_bindings,
            };
            ROBV_VK_TRY(vkCreateDescriptorSetLayout(vk.device, &nv12_info, nullptr, &g.nv12_set_layout));

            const VkDescriptorPoolSize sizes[2] = {
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  3},
            };
            const VkDescriptorPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 2,
                .poolSizeCount = 2,
                .pPoolSizes = sizes,
            };
            ROBV_VK_TRY(vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &g.pool));

            const VkDescriptorSetLayout layouts[2] = {g.points_set_layout, g.nv12_set_layout};
            VkDescriptorSet sets[2] = {};
            const VkDescriptorSetAllocateInfo set_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = g.pool,
                .descriptorSetCount = 2,
                .pSetLayouts = layouts,
            };
            ROBV_VK_TRY(vkAllocateDescriptorSets(vk.device, &set_info, sets));
            g.points_set = sets[0];
            g.nv12_set = sets[1];

            // a placeholder until the first cloud, so the set is always valid
            if (!vk::create_buffer(vk, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk_memory_e::upload, g.points))
            {
                return false;
            }
            write_points_set(vk, g);

            const VkDescriptorImageInfo images[3] = {
                {VK_NULL_HANDLE, g.color.view,    VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, g.y_plane.view,  VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, g.uv_plane.view, VK_IMAGE_LAYOUT_GENERAL},
            };
            VkWriteDescriptorSet writes[3];
            for (u32 i = 0; i < 3; ++i)
            {
                writes[i] = {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = g.nv12_set,
                    .dstBinding = i,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &images[i],
                };
            }
            vkUpdateDescriptorSets(vk.device, 3, writes, 0, nullptr);
            return true;
        }

        VkPipeline create_draw_pipeline(const vk_context_t& vk, const gpu_t& g, std::span<const u32> spirv,
                                        VkPrimitiveTopology topology)
        {
            const VkShaderModule module = vk::create_shader_module(vk, spirv);
            if (!module) { return VK_NULL_HANDLE; }

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
                .topology = topology,
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
            const VkPipelineDepthStencilStateCreateInfo depth{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                .depthTestEnable = VK_TRUE,
                .depthWriteEnable = VK_TRUE,
                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
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
                .pDepthStencilState = &depth,
                .pColorBlendState = &blend,
                .pDynamicState = &dynamic,
                .layout = g.draw_layout,
                .renderPass = g.render_pass,
                .subpass = 0,
            };
            VkPipeline pipeline = VK_NULL_HANDLE;
            const VkResult result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
            vkDestroyShaderModule(vk.device, module, nullptr);
            if (result != VK_SUCCESS)
            {
                ROBV_LOG_ERROR("RENDER", "vkCreateGraphicsPipelines failed: {}", vk::result_string(result));
                return VK_NULL_HANDLE;
            }
            return pipeline;
        }

        bool create_pipelines(const vk_context_t& vk, gpu_t& g)
        {
            // points and lines share one layout, the push range covers the larger of the two
            const VkPushConstantRange draw_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(points_constants_t)};
            const VkPipelineLayoutCreateInfo draw_info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &g.points_set_layout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &draw_range,
            };
            ROBV_VK_TRY(vkCreatePipelineLayout(vk.device, &draw_info, nullptr, &g.draw_layout));

            const VkPushConstantRange nv12_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(u32)};
            const VkPipelineLayoutCreateInfo nv12_info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &g.nv12_set_layout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &nv12_range,
            };
            ROBV_VK_TRY(vkCreatePipelineLayout(vk.device, &nv12_info, nullptr, &g.nv12_layout));

            g.points_pipeline = create_draw_pipeline(vk, g, shaders::points, VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
            g.lines_pipeline = create_draw_pipeline(vk, g, shaders::lines, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
            if (!g.points_pipeline || !g.lines_pipeline) { return false; }

            const VkShaderModule module = vk::create_shader_module(vk, shaders::rgb_to_nv12);
            if (!module) { return false; }
            const VkComputePipelineCreateInfo compute_info{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                          .module = module,
                          .pName = "main"},
                .layout = g.nv12_layout,
            };
            const VkResult result =
                vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &compute_info, nullptr, &g.nv12_pipeline);
            vkDestroyShaderModule(vk.device, module, nullptr);
            ROBV_VK_TRY(result);
            return true;
        }

        bool create_commands(const vk_context_t& vk, gpu_t& g)
        {
            const VkCommandPoolCreateInfo pool_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                .queueFamilyIndex = vk.graphics.family,
            };
            ROBV_VK_TRY(vkCreateCommandPool(vk.device, &pool_info, nullptr, &g.cmd_pool));
            const VkCommandBufferAllocateInfo cmd_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = g.cmd_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            ROBV_VK_TRY(vkAllocateCommandBuffers(vk.device, &cmd_info, &g.cmd));
            return vk::create_timeline(vk, g.timeline);
        }

        void destroy_gpu(const vk_context_t& vk, gpu_t* g)
        {
            if (!g) { return; }
            const VkDevice device = vk.device;
            vkDeviceWaitIdle(device);

            vk::destroy_timeline(vk, g->timeline);
            if (g->cmd_pool) { vkDestroyCommandPool(device, g->cmd_pool, nullptr); }
            for (VkPipeline pipeline : {g->points_pipeline, g->lines_pipeline, g->nv12_pipeline})
            {
                if (pipeline) { vkDestroyPipeline(device, pipeline, nullptr); }
            }
            for (VkPipelineLayout layout : {g->draw_layout, g->nv12_layout})
            {
                if (layout) { vkDestroyPipelineLayout(device, layout, nullptr); }
            }
            if (g->pool) { vkDestroyDescriptorPool(device, g->pool, nullptr); }
            for (VkDescriptorSetLayout layout : {g->points_set_layout, g->nv12_set_layout})
            {
                if (layout) { vkDestroyDescriptorSetLayout(device, layout, nullptr); }
            }
            if (g->framebuffer) { vkDestroyFramebuffer(device, g->framebuffer, nullptr); }
            if (g->render_pass) { vkDestroyRenderPass(device, g->render_pass, nullptr); }
            vk::destroy_buffer(vk, g->points);
            vk::destroy_buffer(vk, g->readback);
            for (vk_image_t* image : {&g->color, &g->depth, &g->y_plane, &g->uv_plane})
            {
                vk::destroy_image(vk, *image);
            }
            delete g;
        }

        bool init_vulkan(renderer_t& renderer)
        {
            const vk_context_t& vk = *renderer.vk;
            const renderer_config_t& config = renderer.config;

            gpu_t* g = new gpu_t;
            renderer.backend = g;

            g->depth_format = pick_depth_format(vk);
            if (g->depth_format == VK_FORMAT_UNDEFINED)
            {
                ROBV_LOG_ERROR("RENDER", "no depth format this device can render to");
                return false;
            }
            const VkFormatFeatureFlags storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
            if (!supports(vk, color_format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | storage) ||
                !supports(vk, VK_FORMAT_R8_UNORM, storage | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
                !supports(vk, VK_FORMAT_R8G8_UNORM, storage | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
            {
                ROBV_LOG_ERROR("RENDER", "r8 and rg8 storage images are not supported, the NV12 conversion needs them");
                return false;
            }

            const VkPhysicalDeviceLimits& limits = vk.properties.limits;
            g->max_point_size = vk.large_points ? limits.pointSizeRange[1] : 1.0f;

            if (!create_targets(vk, *g, config.width, config.height) ||
                !create_render_pass(vk, *g, config.width, config.height) || !create_descriptors(vk, *g) ||
                !create_pipelines(vk, *g) || !create_commands(vk, *g))
            {
                return false;
            }
            ROBV_LOG_INFO("RENDER", "vulkan renderer {}x{} on {}, depth format {}, points up to {:.0f} px",
                          config.width, config.height, vk.properties.deviceName, static_cast<i32>(g->depth_format),
                          static_cast<f64>(g->max_point_size));
            return true;
        }

        // z range for the height colours, the one CPU pass over a cloud, once per new blob
        void measure_height(const point_cloud_frame_t& cloud, gpu_t& g)
        {
            const std::byte* data = cloud.data->data();
            f32 low = INFINITY;
            f32 high = -INFINITY;
            for (u32 row = 0; row < cloud.height; ++row)
            {
                for (u32 col = 0; col < cloud.width; ++col)
                {
                    f32 z = 0.0f;
                    const size_t offset = static_cast<size_t>(row) * cloud.row_stride +
                                          static_cast<size_t>(col) * cloud.point_stride + g.layout.offset_xyz + 8;
                    std::memcpy(&z, data + offset, sizeof(z));
                    if (!std::isfinite(z)) { continue; }
                    low = std::min(low, z);
                    high = std::max(high, z);
                }
            }
            if (!(low <= high))
            {
                low = 0.0f;
                high = 1.0f;
            }
            g.height_min = low;
            g.height_max = std::max(high, low + 0.01f);
        }

        void upload_cloud(renderer_t& renderer, gpu_t& g, const point_cloud_frame_t* cloud)
        {
            if (!cloud || !cloud->data || cloud->data == g.uploaded) { return; }
            g.uploaded = cloud->data;
            g.drawable = false;

            const vk_context_t& vk = *renderer.vk;
            const cloud_layout_t layout = point_cloud::resolve_layout(cloud->fields);
            if (!point_cloud::make_gpu_layout(cloud->fields, layout, cloud->point_stride, g.layout))
            {
                ROBV_LOG_WARN("RENDER", "cloud from '{}' has no float32 x y z the GPU can read, not drawn",
                              cloud->frame_id);
                return;
            }

            // the shader reads 32 bit words, a padded row must keep them aligned
            if (cloud->row_stride % 4 != 0)
            {
                ROBV_LOG_WARN("RENDER", "cloud from '{}' has a row stride of {} bytes, not a multiple of 4, not drawn",
                              cloud->frame_id, cloud->row_stride);
                return;
            }

            const std::vector<std::byte>& blob = *cloud->data;
            const VkDeviceSize needed = static_cast<VkDeviceSize>(cloud->row_stride) * cloud->height;
            if (blob.size() < needed || needed == 0) { return; }
            if (needed > vk.properties.limits.maxStorageBufferRange)
            {
                ROBV_LOG_WARN("RENDER", "cloud of {} MiB exceeds this GPU's {} MiB storage buffer limit, not drawn",
                              needed >> 20, vk.properties.limits.maxStorageBufferRange >> 20);
                return;
            }

            if (g.points.size < needed)
            {
                vk::destroy_buffer(vk, g.points);
                if (!vk::create_buffer(vk, needed, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk_memory_e::upload, g.points))
                {
                    return;
                }
                write_points_set(vk, g);
            }
            std::memcpy(g.points.mapped, blob.data(), needed);
            vmaFlushAllocation(vk.allocator, g.points.allocation, 0, needed);

            g.width = cloud->width;
            g.row_stride = cloud->row_stride;
            g.point_count = cloud->width * cloud->height;
            measure_height(*cloud, g);
            g.drawable = true;
            ++renderer.uploads;
        }

        void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                           VkAccessFlags dst_access)
        {
            const VkImageMemoryBarrier barrier{
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
            vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        void record_scene(const renderer_t& renderer, const gpu_t& g, VkCommandBuffer cmd, const view_state_t& state)
        {
            const u32 width = renderer.config.width;
            const u32 height = renderer.config.height;

            const VkClearValue clears[2] = {{.color = background}, {.depthStencil = {1.0f, 0}}};
            const VkRenderPassBeginInfo pass_info{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = g.render_pass,
                .framebuffer = g.framebuffer,
                .renderArea = {{0, 0}, {width, height}},
                .clearValueCount = 2,
                .pClearValues = clears,
            };
            vkCmdBeginRenderPass(cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

            const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), 0.0f, 1.0f};
            const VkRect2D scissor{
                {0,     0     },
                {width, height}
            };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            const camera_matrices_t camera =
                camera_matrices(state.camera, static_cast<f32>(width) / static_cast<f32>(height));
            const glm::mat4 view_projection = camera.projection * camera.view;

            if (state.render.show_grid || state.render.show_axes)
            {
                const lines_constants_t constants{
                    .view_projection = view_projection,
                    .grid_center = {std::round(state.camera.target.x), std::round(state.camera.target.y)},
                };
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.lines_pipeline);
                vkCmdPushConstants(cmd, g.draw_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
                if (state.render.show_grid) { vkCmdDraw(cmd, grid_vertices, 1, 0, 0); }
                if (state.render.show_axes) { vkCmdDraw(cmd, axes_vertices, 1, grid_vertices, 0); }
            }

            if (g.drawable)
            {
                const points_constants_t constants{
                    .view_projection = view_projection,
                    .layout = g.layout,
                    .width = g.width,
                    .row_stride = g.row_stride,
                    .color_mode = static_cast<u32>(state.render.color_mode),
                    .point_size = std::clamp(state.render.point_size, 1.0f, g.max_point_size),
                    .intensity_min = state.render.intensity_min,
                    .intensity_max = state.render.intensity_max,
                    .height_min = g.height_min,
                    .height_max = g.height_max,
                };
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.points_pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.draw_layout, 0, 1, &g.points_set, 0,
                                        nullptr);
                vkCmdPushConstants(cmd, g.draw_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
                vkCmdDraw(cmd, g.point_count, 1, 0, 0);
            }

            vkCmdEndRenderPass(cmd);
        }

        // RGB to NV12 planes on the GPU, then both planes into the readback buffer
        void record_conversion(const renderer_t& renderer, const gpu_t& g, VkCommandBuffer cmd)
        {
            const u32 width = renderer.config.width;
            const u32 height = renderer.config.height;

            // the previous frame finished on the host side, so the old contents can go
            for (VkImage plane : {g.y_plane.image, g.uv_plane.image})
            {
                image_barrier(cmd, plane, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_SHADER_WRITE_BIT);
            }

            const u32 extent[2] = {width, height};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.nv12_pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.nv12_layout, 0, 1, &g.nv12_set, 0, nullptr);
            vkCmdPushConstants(cmd, g.nv12_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(extent), extent);
            // one thread per 2x2 block, 8x8 threads per group
            vkCmdDispatch(cmd, (width / 2 + 7) / 8, (height / 2 + 7) / 8, 1);

            for (VkImage plane : {g.y_plane.image, g.uv_plane.image})
            {
                image_barrier(cmd, plane, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            }

            const VkBufferImageCopy y_copy{
                .bufferOffset = 0,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {width, height, 1},
            };
            const VkBufferImageCopy uv_copy{
                .bufferOffset = static_cast<VkDeviceSize>(width) * height,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {width / 2, height / 2, 1},
            };
            vkCmdCopyImageToBuffer(cmd, g.y_plane.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.buffer, 1,
                                   &y_copy);
            vkCmdCopyImageToBuffer(cmd, g.uv_plane.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.buffer, 1,
                                   &uv_copy);

            const VkBufferMemoryBarrier host_barrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = g.readback.buffer,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &host_barrier, 0, nullptr);
        }

        // submit and wait: the frame goes to the encoder in this tick, no frame of latency is added
        // the readback is the one GPU to CPU copy per frame, zero copy into the encoder comes with the Jetson
        bool draw_vulkan(renderer_t& renderer, const point_cloud_frame_t* cloud, const view_state_t& state,
                         planar_frame_t& out)
        {
            const vk_context_t& vk = *renderer.vk;
            gpu_t& g = *static_cast<gpu_t*>(renderer.backend);
            upload_cloud(renderer, g, cloud);

            vkResetCommandPool(vk.device, g.cmd_pool, 0);
            const VkCommandBufferBeginInfo begin_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            vkBeginCommandBuffer(g.cmd, &begin_info);
            record_scene(renderer, g, g.cmd, state);
            record_conversion(renderer, g, g.cmd);
            vkEndCommandBuffer(g.cmd);

            const u64 value = vk::next_value(g.timeline);
            const VkTimelineSemaphoreSubmitInfo timeline_info{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &value,
            };
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline_info,
                .commandBufferCount = 1,
                .pCommandBuffers = &g.cmd,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &g.timeline.semaphore,
            };
            ROBV_VK_TRY(vkQueueSubmit(vk.graphics.queue, 1, &submit, VK_NULL_HANDLE));
            if (!vk::wait(vk, g.timeline, value)) { return false; }

            const u32 width = renderer.config.width;
            const u32 height = renderer.config.height;
            const size_t y_size = static_cast<size_t>(width) * height;
            vmaInvalidateAllocation(vk.allocator, g.readback.allocation, 0, VK_WHOLE_SIZE);
            const u8* mapped = static_cast<const u8*>(g.readback.mapped);
            // allocate_nv12 packs rows, so each plane is one copy
            std::memcpy(video::y_plane(out), mapped, y_size);
            std::memcpy(video::uv_plane(out), mapped + y_size, y_size / 2);
            return true;
        }
    } // namespace

    bool init(renderer_t& renderer, const renderer_config_t& config, const vk_context_t& vk)
    {
        renderer = {};
        renderer.config = config;
        // NV12 subsamples 2x2
        renderer.config.width = (config.width + 1) & ~1u;
        renderer.config.height = (config.height + 1) & ~1u;
        renderer.vk = &vk;

        switch (config.kind)
        {
        case renderer_kind_e::null:
            ROBV_LOG_INFO("RENDER", "null renderer {}x{}, colour bars driven by the view state", renderer.config.width,
                          renderer.config.height);
            return true;
        case renderer_kind_e::vulkan: return init_vulkan(renderer);
        }
        return false;
    }

    void shutdown(renderer_t& renderer)
    {
        if (renderer.config.kind == renderer_kind_e::vulkan && renderer.vk)
        {
            destroy_gpu(*renderer.vk, static_cast<gpu_t*>(renderer.backend));
        }
        renderer = {};
    }

    bool draw(renderer_t& renderer, const point_cloud_frame_t* cloud, const view_state_t& state, planar_frame_t& out)
    {
        if (out.width != renderer.config.width || out.height != renderer.config.height || out.data.empty())
        {
            video::allocate_nv12(out, renderer.config.width, renderer.config.height);
        }

        bool ok = true;
        switch (renderer.config.kind)
        {
        case renderer_kind_e::null: draw_null(cloud, state, out); break;
        case renderer_kind_e::vulkan: ok = draw_vulkan(renderer, cloud, state, out); break;
        }

        out.tick = state.tick;
        ++renderer.frames;
        return ok;
    }

    camera_matrices_t camera_matrices(const camera_t& camera, f32 aspect)
    {
        const f32 cp = std::cos(camera.pitch);
        const glm::vec3 target(camera.target.x, camera.target.y, camera.target.z);
        const glm::vec3 offset(cp * std::cos(camera.yaw), cp * std::sin(camera.yaw), std::sin(camera.pitch));

        camera_matrices_t out;
        out.eye = target + offset * camera.distance;
        out.view = glm::lookAt(out.eye, target, glm::vec3(0.0f, 0.0f, 1.0f));
        out.projection = glm::perspective(camera.fov_y, aspect, camera.near_plane, camera.far_plane);
        out.projection[1][1] *= -1.0f;
        return out;
    }

    bool parse_kind(std::string_view text, renderer_kind_e& out)
    {
        if (text == "null") { out = renderer_kind_e::null; }
        else if (text == "vulkan") { out = renderer_kind_e::vulkan; }
        else
        {
            return false;
        }
        return true;
    }

    const char* kind_name(renderer_kind_e kind)
    {
        switch (kind)
        {
        case renderer_kind_e::null: return "null";
        case renderer_kind_e::vulkan: return "vulkan";
        }
        return "unknown";
    }
} // namespace robv::renderer
