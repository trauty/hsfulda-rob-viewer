#include "robv/client/app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <string>

namespace robv::app
{
    namespace
    {
        // dark slate, distinct from black to spot an empty frame
        constexpr f32 clear_color[4] = {0.07f, 0.09f, 0.12f, 1.0f};

        // ImGui needs a few frames after input to settle hover, active and release
        constexpr u32 frames_after_input = 3;
        // idle refresh for the panel numbers and the text cursor blink
        constexpr u64 idle_period_ns = 500'000'000ull;
        constexpr u64 text_input_period_ns = 250'000'000ull;

        // frames start 1 ms after a vsync, once the batched touches have arrived
        // a frame takes 2 to 3 ms (Tab A11+), SurfaceFlinger latches at about 7.5 ms
        // a start later than 4 ms waits for the next slot instead of risking the latch
        constexpr u64 vsync_offset_ns = 1'000'000ull;
        constexpr u64 vsync_late_ns = 3'000'000ull;
        // frames allowed to wait for the screen: one scanning out, one waiting for the latch
        // more is pure latency, a vsync is skipped instead
        constexpr u32 max_frames_queued = 3;
        // timing reports can lag or stop, so never skip more than this in a row
        constexpr u32 max_skipped_slots = 3;

        u64 now_ns() { return SDL_GetTicksNS(); }

        void fail(app_t& app, const std::string& message)
        {
            ROBV_LOG_FATAL("APP", "{}", message);
            // nobody reads logcat on a tablet, so the reason goes on screen too
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "robv", message.c_str(), app.window);
        }

        // best Vulkan version any device offers, to explain a failed device creation
        u32 best_device_api(const vk_context_t& vk)
        {
            u32 count = 0;
            vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(vk.instance, &count, devices.data());

            u32 best = 0;
            for (VkPhysicalDevice device : devices)
            {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                best = std::max(best, properties.apiVersion);
            }
            return best;
        }

        VkSurfaceKHR create_surface(app_t& app)
        {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(app.window, app.vk.instance, nullptr, &surface))
            {
                ROBV_LOG_ERROR("APP", "SDL_Vulkan_CreateSurface: {}", SDL_GetError());
                return VK_NULL_HANDLE;
            }
            return surface;
        }

        // Android can background the app before it has a window (locked screen, user switching away)
        // startup waits for the foreground instead of failing
        // the foreground event only reaches watchers, SDL queues SDL_EVENT_WINDOW_RESTORED right after
        // VK_NULL_HANDLE with app.quit set when closed while waiting
        VkSurfaceKHR wait_for_surface(app_t& app)
        {
            VkSurfaceKHR surface = create_surface(app);
#if defined(ROBV_PLATFORM_ANDROID)
            SDL_Event event;
            while (!surface && !app.quit && SDL_WaitEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT) { app.quit = true; }
                else if (event.type == SDL_EVENT_WINDOW_RESTORED || event.type == SDL_EVENT_WINDOW_SHOWN)
                {
                    ROBV_LOG_INFO("APP", "in the foreground, retrying the surface");
                    surface = create_surface(app);
                }
            }
#endif
            return surface;
        }

        // SDL hands lifecycle events to watchers inside SDL_PollEvent, never queues them
        // on Android that is the app's own thread, so Vulkan calls are safe
        // the old surface must go before the native window, a new one is needed on resume
        bool SDLCALL on_lifecycle(void* userdata, SDL_Event* event)
        {
            app_t& app = *static_cast<app_t*>(userdata);
            switch (event->type)
            {
            case SDL_EVENT_TERMINATING: app.quit = true; break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                app.in_background = true;
                presenter::lose_surface(app.presenter);
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND:
                app.in_background = false;
                app.pending_frames = frames_after_input;
                // not presenting yet, startup makes its own surface
                if (!app.presenter.vk) { break; }
                // recreated even if one exists, a surface from just before a pause is stale
                presenter::lose_surface(app.presenter);
                if (const VkSurfaceKHR surface = create_surface(app);
                    !surface || !presenter::restore_surface(app.presenter, surface))
                {
                    app.quit = true;
                }
                break;
            default: break;
            }
            return true;
        }

        void pace_to_vsync(app_t& app)
        {
            vsync::keep_alive(app.vsync);
            const u64 now = vsync::now_ns();
            const u64 period = vsync::period_ns(app.vsync);
            u64 slot = vsync::slot_at(app.vsync, now, vsync_offset_ns);
            // no grid before the first callback, that frame goes unpaced
            if (slot == 0 || period == 0) { return; }

            if (slot <= app.last_slot_ns || now - slot > vsync_late_ns) { slot += period; }
            while (slot <= app.last_slot_ns) { slot += period; }
            vsync::sleep_until(slot);

            for (u32 skipped = 0;
                 skipped < max_skipped_slots && presenter::frames_queued(app.presenter) > max_frames_queued; ++skipped)
            {
                slot += period;
                vsync::sleep_until(slot);
            }
            app.last_slot_ns = slot;
        }

        u64 refresh_period_ns(const app_t& app)
        {
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(app.window));
            const f32 hz = mode && mode->refresh_rate > 0.0f ? mode->refresh_rate : 60.0f;
            return static_cast<u64>(1e9 / static_cast<f64>(hz));
        }

        bool frame_due(const app_t& app, u64 now)
        {
            if (app.in_background) { return false; }
            return app.config.continuous || app.pending_frames > 0 || now >= app.next_idle_ns;
        }

        void check_vk_result(VkResult result)
        {
            if (result != VK_SUCCESS) { ROBV_LOG_ERROR("IMGUI", "Vulkan call failed: {}", vk::result_string(result)); }
        }

        bool init_imgui(app_t& app)
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            // no ini file: no writable working directory on Android, no layout worth saving yet
            io.IniFilename = nullptr;

            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            app.ui_scale = std::max(1.0f, SDL_GetWindowDisplayScale(app.window));
            style.ScaleAllSizes(app.ui_scale);
            style.FontScaleDpi = app.ui_scale;

            if (!ImGui_ImplSDL3_InitForVulkan(app.window)) { return false; }

            ImGui_ImplVulkan_InitInfo info{};
            info.ApiVersion = app.vk_config.api_version;
            info.Instance = app.vk.instance;
            info.PhysicalDevice = app.vk.physical_device;
            info.Device = app.vk.device;
            info.QueueFamily = app.vk.graphics.family;
            info.Queue = app.vk.graphics.queue;
            info.DescriptorPoolSize = 8;
            info.MinImageCount = std::max(2u, app.presenter.min_image_count);
            info.ImageCount = std::max(info.MinImageCount, static_cast<u32>(app.presenter.images.size()));
            info.PipelineInfoMain.RenderPass = app.presenter.render_pass;
            info.PipelineInfoMain.Subpass = 0;
            info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            info.CheckVkResultFn = check_vk_result;
            if (!ImGui_ImplVulkan_Init(&info)) { return false; }

            app.imgui_ready = true;
            return true;
        }

        const char* connection_name(connection_state_e state)
        {
            switch (state)
            {
            case connection_state_e::disconnected: return "disconnected";
            case connection_state_e::connecting: return "connecting";
            case connection_state_e::connected: return "connected";
            }
            return "unknown";
        }

        void draw_panel(app_t& app)
        {
            const vk_context_t& vk = app.vk;
            const presenter_t& p = app.presenter;
            connection_t& c = app.connection;

            // inside the safe area: clear of display cutouts and visible system bars
            SDL_Rect safe{};
            SDL_GetWindowSafeArea(app.window, &safe);
            const f32 margin = 16.0f * app.ui_scale;
            ImGui::SetNextWindowPos(ImVec2(static_cast<f32>(safe.x) + margin, static_cast<f32>(safe.y) + margin),
                                    ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(420.0f * app.ui_scale, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("robv " ROBV_VERSION);

            ImGui::SeparatorText("Connection");
            const ImVec4 state_color = c.state == connection_state_e::connected ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                                                                : ImVec4(0.9f, 0.6f, 0.3f, 1.0f);
            ImGui::TextColored(state_color, "%s", connection_name(c.state));
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", c.status.c_str());
            char server[128];
            std::snprintf(server, sizeof(server), "%s", c.server.c_str());
            if (ImGui::InputText("server", server, sizeof(server))) { c.server = server; }
            ImGui::BeginDisabled();
            ImGui::Button("Connect");
            ImGui::EndDisabled();
            if (c.state == connection_state_e::connected)
            {
                ImGui::Text("client %u, rtt %.1f ms, tick %llu", c.client_id, static_cast<f64>(c.rtt_ms),
                            static_cast<unsigned long long>(c.view.tick));
            }

            ImGui::SeparatorText("Device");
            ImGui::Text("%s (%s)", vk.properties.deviceName, vk::device_type_string(vk.properties.deviceType));
            ImGui::Text("Vulkan %s, %s", vk::version_string(vk.api_version).c_str(), vk.driver.driverInfo);
            ImGui::Text("queues  graphics %u  compute %u  transfer %u", vk.graphics.family, vk.compute.family,
                        vk.transfer.family);
            ImGui::Text("swapchain %ux%u, %zu images, format %d", p.extent.width, p.extent.height, p.images.size(),
                        static_cast<i32>(p.format.format));
#if defined(ROBV_PLATFORM_ANDROID)
            ImGui::Text("zero copy decode: %s",
                        vk::has_device_extension(vk, "VK_ANDROID_external_memory_android_hardware_buffer") ? "available"
                                                                                                           : "missing");
#endif

            ImGui::SeparatorText("Stream");
            ImGui::Text("decoder %s, %llu frames", decoder::kind_name(app.decoder.config.kind),
                        static_cast<unsigned long long>(app.decoder.frames_out));
            if (p.display_timing)
            {
                ImGui::Text("present -> screen %.1f ms, %u frames queued", static_cast<f64>(p.queue_to_screen_ms),
                            p.next_present_id - 1 - p.last_shown_id);
            }
            ImGui::Text("%.0f draws/s%s, last frame %.2f ms CPU, ui scale %.2f", static_cast<f64>(app.draws_per_second),
                        app.config.continuous ? " (continuous)" : " (on demand)", static_cast<f64>(app.frame_cpu_ms),
                        static_cast<f64>(app.ui_scale));

            ImGui::End();
        }
    } // namespace

    bool init(app_t& app, const app_config_t& config)
    {
        app.config = config;
        app.start_ns = now_ns();

        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fail(app, std::string("SDL_Init failed: ") + SDL_GetError());
            return false;
        }
        SDL_AddEventWatch(on_lifecycle, &app);
#if defined(ROBV_PLATFORM_ANDROID)
        // SDL 3.4 on Android: SDL_WaitEvent(Timeout) requeues SDL_EVENT_POLL_SENTINEL every pass,
        // waking the lifecycle semaphore it waits on, so the wait spins a core
        // without the sentinel SDL_PollEvent just drains the queue, all the client needs
        SDL_SetEventEnabled(SDL_EVENT_POLL_SENTINEL, false);
#endif

        SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(ROBV_PLATFORM_ANDROID)
        // immersive: Android 15+ draws edge to edge, the bars would cover the picture
        // a swipe from the edge brings them back
        window_flags |= SDL_WINDOW_FULLSCREEN;
#endif
        app.window =
            SDL_CreateWindow("robv", static_cast<int>(config.width), static_cast<int>(config.height), window_flags);
        if (!app.window)
        {
            fail(app, std::string("SDL_CreateWindow failed: ") + SDL_GetError());
            return false;
        }

        // SDL already opened the Vulkan loader for the window, volk reuses it
        app.vk_config.app_name = "robv-client";
        app.vk_config.need_presentation = true;
        app.vk_config.enable_validation = config.validation;
        app.vk_config.loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        Uint32 extension_count = 0;
        const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        app.vk_config.extra_instance_extensions.assign(extensions, extensions + extension_count);
#if defined(ROBV_PLATFORM_ANDROID)
        // zero copy MediaCodec path, shown in the panel until the decoder uses it
        app.vk_config.optional_device_extensions = {"VK_ANDROID_external_memory_android_hardware_buffer",
                                                    "VK_EXT_queue_family_foreign", "VK_GOOGLE_display_timing"};
#endif

        if (!app.vk_config.loader || !vk::init_instance(app.vk, app.vk_config))
        {
            fail(app, "Could not create a Vulkan instance. robv needs a Vulkan 1.2 driver.");
            return false;
        }

        const VkSurfaceKHR surface = wait_for_surface(app);
        if (!surface)
        {
            if (!app.quit) { fail(app, std::string("Could not create a Vulkan surface: ") + SDL_GetError()); }
            return false;
        }

        if (!vk::init_device(app.vk, app.vk_config, surface))
        {
            const u32 best = best_device_api(app.vk);
            vkDestroySurfaceKHR(app.vk.instance, surface, nullptr);
            fail(app, "No usable GPU. robv needs Vulkan 1.2 with timeline semaphores, this device offers Vulkan " +
                          vk::version_string(best) + ".");
            return false;
        }

        if (!presenter::init(app.presenter, app.window, app.vk, surface))
        {
            fail(app, "Could not set up presentation, see the log.");
            return false;
        }

        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(app.window));
        vsync::start(app.vsync, mode ? mode->refresh_rate : 0.0f);

        if (!init_imgui(app))
        {
            fail(app, "Could not initialise ImGui, see the log.");
            return false;
        }

        const decoder_config_t decoder_config{.kind = config.decoder};
        if (!decoder::open(app.decoder, decoder_config))
        {
            fail(app, std::string("The ") + decoder::kind_name(config.decoder) + " decoder is not available.");
            return false;
        }

        ROBV_LOG_INFO("APP", "ready in {:.0f} ms", static_cast<f64>(now_ns() - app.start_ns) * 1e-6);
        return true;
    }

    void shutdown(app_t& app)
    {
        SDL_RemoveEventWatch(on_lifecycle, &app);
        vsync::stop(app.vsync);
        if (app.vk.device) { vkDeviceWaitIdle(app.vk.device); }
        decoder::close(app.decoder);
        if (app.imgui_ready)
        {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            app.imgui_ready = false;
        }
        presenter::shutdown(app.presenter);
        vk::shutdown(app.vk);
        if (app.window) { SDL_DestroyWindow(app.window); }
        app.window = nullptr;
        SDL_Quit();
    }

    void handle_event(app_t& app, const SDL_Event& event)
    {
        if (app.imgui_ready) { ImGui_ImplSDL3_ProcessEvent(&event); }
        app.pending_frames = std::max(app.pending_frames, frames_after_input);

        switch (event.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: app.quit = true; break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: presenter::request_rebuild(app.presenter); break;
        default: break;
        }
    }

    void wait(app_t& app)
    {
        u64 now = now_ns();
        if (!frame_due(app, now))
        {
            const u64 until_ns = app.next_idle_ns > now ? app.next_idle_ns - now : 0;
            const Sint32 timeout_ms = static_cast<Sint32>(std::max<u64>(1, (until_ns + 999'999) / 1'000'000));
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, timeout_ms)) { handle_event(app, event); }
            now = now_ns();
        }

        if (!frame_due(app, now)) { return; }
        // only motion frames need the vsync grid, pacing the idle refresh keeps the vsync thread awake
        const bool in_motion = app.config.continuous || app.pending_frames > 0;
        if (vsync::is_running(app.vsync))
        {
            if (in_motion) { pace_to_vsync(app); }
            return;
        }

        // no vsync source: FIFO already blocks in acquire
        // MAILBOX and frames that presented nothing never block, so they are held here
        // relative to the last frame, which drifts against the display, hence vsync is preferred
        const bool fifo_paces = app.presenter.present_mode == VK_PRESENT_MODE_FIFO_KHR && app.last_frame_presented;
        if (!fifo_paces)
        {
            const u64 period = refresh_period_ns(app);
            const u64 since = now - app.last_draw_ns;
            if (since < period) { SDL_DelayNS(period - since); }
        }
    }

    void iterate(app_t& app)
    {
        const u64 start = now_ns();
        if (!frame_due(app, start)) { return; }

        decoder::acquire(app.decoder, start, app.decoded);
        // polls display timing so the panel is current on idle frames too
        presenter::frames_queued(app.presenter);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_panel(app);
        ImGui::Render();

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        app.last_frame_presented = presenter::begin_frame(app.presenter, clear_color, cmd);
        if (app.last_frame_presented)
        {
            ImGui_ImplVulkan_SetMinImageCount(std::max(2u, app.presenter.min_image_count));
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            presenter::end_frame(app.presenter);
            ++app.frames;
            ++app.rate_window_frames;
        }

        app.last_draw_ns = start;
        if (app.pending_frames > 0) { --app.pending_frames; }
        app.next_idle_ns = start + (ImGui::GetIO().WantTextInput ? text_input_period_ns : idle_period_ns);

        const u64 end = now_ns();
        app.frame_cpu_ms = static_cast<f32>(end - start) * 1e-6f;
        if (end - app.rate_window_ns >= 1'000'000'000ull)
        {
            app.draws_per_second = static_cast<f32>(static_cast<f64>(app.rate_window_frames) * 1e9 /
                                                    static_cast<f64>(end - app.rate_window_ns));
            app.rate_window_ns = end;
            app.rate_window_frames = 0;
        }
    }
} // namespace robv::app
