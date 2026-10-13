// reports what this machine offers robv

#include "robv/common/log.h"
#include "robv/vk/context.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <format>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
    template <typename... args_t>
    void print(std::format_string<args_t...> fmt, args_t&&... args)
    { std::fputs(std::format(fmt, std::forward<args_t>(args)...).c_str(), stdout); }

    const char* yes_no(bool value) { return value ? "yes" : "no"; }

    bool file_exists(const char* path)
    {
        struct stat info{};
        return stat(path, &info) == 0;
    }

    bool can_dlopen(const char* name)
    {
        void* handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
        if (handle) { dlclose(handle); }
        return handle != nullptr;
    }

    std::string queue_flags_string(VkQueueFlags flags)
    {
        std::string out;
        if (flags & VK_QUEUE_GRAPHICS_BIT) { out += "graphics "; }
        if (flags & VK_QUEUE_COMPUTE_BIT) { out += "compute "; }
        if (flags & VK_QUEUE_TRANSFER_BIT) { out += "transfer "; }
        if (flags & VK_QUEUE_SPARSE_BINDING_BIT) { out += "sparse "; }
        if (flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) { out += "video-decode "; }
        if (flags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) { out += "video-encode "; }
        if (!out.empty()) { out.pop_back(); }
        return out;
    }

    template <typename props_t, typename name_fn_t>
    bool has_name(const std::vector<props_t>& list, const char* name, name_fn_t get_name)
    {
        for (const props_t& item : list)
        {
            if (std::strcmp(get_name(item), name) == 0) { return true; }
        }
        return false;
    }

    std::vector<VkExtensionProperties> instance_extensions()
    {
        u32 count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> out(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, out.data());
        return out;
    }

    std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device)
    {
        u32 count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> out(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, out.data());
        return out;
    }

    const char* extension_name(const VkExtensionProperties& p) { return p.extensionName; }
    const char* layer_name(const VkLayerProperties& p) { return p.layerName; }

    void print_instance(const robv::vk_context_t& ctx)
    {
        const std::vector<VkExtensionProperties> extensions = instance_extensions();

        u32 layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

        print("Vulkan instance\n");
        print("  loader version         {}\n", robv::vk::version_string(ctx.instance_version));
        print("  validation layer       {}\n", yes_no(has_name(layers, "VK_LAYER_KHRONOS_validation", layer_name)));

        // presentation needs VK_KHR_surface plus a platform surface, the server needs neither
        print("Presentation (instance)\n");
        for (const char* name :
             {"VK_KHR_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
              "VK_KHR_android_surface", "VK_KHR_display", "VK_EXT_headless_surface"})
        {
            print("  {:<30} {}\n", name, yes_no(has_name(extensions, name, extension_name)));
        }
    }

    // true when the device meets robv's floor: Vulkan 1.2 with timeline semaphores
    bool print_device(VkPhysicalDevice device, u32 index)
    {
        VkPhysicalDeviceDriverProperties driver{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &driver};
        vkGetPhysicalDeviceProperties2(device, &properties);
        const VkPhysicalDeviceProperties& p = properties.properties;

        VkPhysicalDeviceVulkan11Features features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                    .pNext = &features11};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
        vkGetPhysicalDeviceFeatures2(device, &features);

        const bool api_ok = p.apiVersion >= VK_API_VERSION_1_2;
        const bool usable = api_ok && features12.timelineSemaphore;

        print("\nDevice {}: {}\n", index, static_cast<const char*>(p.deviceName));
        print("  type                   {}\n", robv::vk::device_type_string(p.deviceType));
        print("  vendor:device          {:04x}:{:04x}\n", p.vendorID, p.deviceID);
        print("  Vulkan                 {} {}\n", robv::vk::version_string(p.apiVersion),
              api_ok ? "(ok, 1.2 required)" : "(TOO OLD, 1.2 required)");
        print("  driver                 {} {}\n", static_cast<const char*>(driver.driverName),
              static_cast<const char*>(driver.driverInfo));
        print("  timeline semaphores    {}\n", yes_no(features12.timelineSemaphore));
        print("  sampler YCbCr          {}\n", yes_no(features11.samplerYcbcrConversion));
        print("  max image 2D           {}\n", p.limits.maxImageDimension2D);
        print("  max compute invocations {}\n", p.limits.maxComputeWorkGroupInvocations);
        print("  timestamp period       {} ns\n", p.limits.timestampPeriod);

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
        print("  queue families\n");
        for (u32 i = 0; i < family_count; ++i)
        {
            print("    [{}] x{:<2} {} (timestamp bits {})\n", i, families[i].queueCount,
                  queue_flags_string(families[i].queueFlags), families[i].timestampValidBits);
        }

        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(device, &memory);
        print("  memory heaps\n");
        for (u32 i = 0; i < memory.memoryHeapCount; ++i)
        {
            const VkMemoryHeap& heap = memory.memoryHeaps[i];
            print("    [{}] {:>8} MiB {}\n", i, heap.size / (1024 * 1024),
                  heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "host");
        }

        // zero copy paths, dmabuf between Vulkan and NvBufSurface on the Jetson,
        // AHardwareBuffer between MediaCodec and Vulkan on Android
        const std::vector<VkExtensionProperties> extensions = device_extensions(device);
        print("  extensions\n");
        for (const char* name : {"VK_KHR_swapchain", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                                 "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd",
                                 "VK_ANDROID_external_memory_android_hardware_buffer", "VK_KHR_video_encode_queue",
                                 "VK_KHR_video_encode_h264", "VK_KHR_video_encode_h265"})
        {
            print("    {:<50} {}\n", name, yes_no(has_name(extensions, name, extension_name)));
        }

        print("  usable by robv         {}\n", yes_no(usable));
        return usable;
    }

    void print_encoder()
    {
        print("\nHardware video encoder\n");

        std::string tegra_release = "not a Jetson";
        if (std::ifstream file("/etc/nv_tegra_release"); file) { std::getline(file, tegra_release); }
        print("  /etc/nv_tegra_release  {}\n", tegra_release);
        print("  /dev/v4l2-nvenc        {}\n", yes_no(file_exists("/dev/v4l2-nvenc")));
        print("  libnvv4l2.so           {}\n", yes_no(can_dlopen("libnvv4l2.so")));
        print("  libnvbufsurface.so     {}\n", yes_no(can_dlopen("libnvbufsurface.so")));
        print("  compiled in            {}\n", ROBV_PROBE_ENCODERS);
    }
} // namespace

int main(int argc, char** argv)
{
    bool validation = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--validation") == 0) { validation = true; }
        else
        {
            std::fprintf(stderr, "usage: robv-probe [--validation]\n");
            return 2;
        }
    }

    robv::log::init("robv-probe");

    robv::vk_context_config_t config;
    config.app_name = "robv-probe";
    config.enable_validation = validation;

    robv::vk_context_t ctx;
    if (!robv::vk::init_instance(ctx, config))
    {
        print("No Vulkan instance, nothing else to probe\n");
        print_encoder();
        robv::log::shutdown();
        return 1;
    }

    print_instance(ctx);

    u32 count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(ctx.instance, &count, devices.data());

    u32 usable = 0;
    for (u32 i = 0; i < count; ++i) { usable += print_device(devices[i], i) ? 1 : 0; }
    if (count == 0) { print("\nNo Vulkan devices. Is a driver (ICD) installed?\n"); }

    // test: a headless device with queues and an allocator, as the server creates
    print("\nHeadless device (what robv-server creates)\n");
    const bool headless = usable > 0 && robv::vk::init_device(ctx, config, VK_NULL_HANDLE);
    print("  created                {}\n", yes_no(headless));
    if (headless)
    {
        print("  picked                 {}\n", static_cast<const char*>(ctx.properties.deviceName));
        print("  queues                 graphics {} compute {} transfer {}\n", ctx.graphics.family, ctx.compute.family,
              ctx.transfer.family);
    }

    print_encoder();

    robv::vk::shutdown(ctx);
    robv::log::shutdown();
    return headless ? 0 : 1;
}
