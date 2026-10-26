#include "robv/client/app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <string>
#include <unistd.h>

#if defined(ROBV_PLATFORM_ANDROID)
    #include <sys/system_properties.h>
#endif

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
        constexpr f32 orbit_per_height = PI;
        constexpr f32 zoom_per_wheel_step = 0.15f;

        constexpr u64 stream_log_period_ns = 10'000'000'000ull;

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

        bool streaming(const app_t& app) { return app.connection.state == connection_state_e::connected; }

        u64 next_due_ns(const app_t& app)
        {
            if (!streaming(app)) { return app.next_idle_ns; }
            return std::min(app.next_idle_ns, decoder::next_frame_ns(app.decoder));
        }

        bool frame_due(const app_t& app, u64 now)
        {
            if (app.in_background) { return false; }
            return app.config.continuous || app.pending_frames > 0 || now >= next_due_ns(app);
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

        // the frame's RTP timestamp is the server's render time, the pong clock offset puts it on our clock
        // 90 kHz like RTP, so the comparison survives its wrap
        void measure_video_age(app_t& app)
        {
            const connection_t& c = app.connection;
            if (!c.has_clock_offset) { return; }
            const u64 server_now_ns = static_cast<u64>(static_cast<i64>(net::clock_ns()) + c.clock_offset_ns);
            const u32 frame_ts = static_cast<u32>((app.decoded.pts_ns * 9 + 50'000) / 100'000);
            const i32 age_ticks = static_cast<i32>(rtp::timestamp(server_now_ns) - frame_ts);
            const f32 age_ms = static_cast<f32>(age_ticks) / 90.0f;
            app.video_age_ms = app.video_age_ms == 0.0f ? age_ms : 0.9f * app.video_age_ms + 0.1f * age_ms;
        }

        // images the presenter's frames no longer read go back to the decoder
        void retire_images(app_t& app)
        {
            const u64 completed = vk::completed_value(app.vk, app.presenter.timeline);
            std::erase_if(app.retiring_images,
                          [&](const app_t::retiring_image_t& r)
                          {
                              if (r.done_at > completed) { return false; }
                              decoder::release(app.decoder, r.image);
                              return true;
                          });
        }

        // before the decoder that owns them is reset or closed
        void release_images(app_t& app)
        {
            if (app.vk.device) { vkDeviceWaitIdle(app.vk.device); }
            for (const app_t::retiring_image_t& r : app.retiring_images) { decoder::release(app.decoder, r.image); }
            app.retiring_images.clear();
            decoder::release(app.decoder, app.shown_image);
            app.shown_image = nullptr;
            if (app.decoded_pending) { decoder::release(app.decoder, app.decoded.image); }
            app.decoded = {};
            app.decoded_pending = false;
#if defined(ROBV_PLATFORM_ANDROID)
            video_view::forget_hardware_buffers(app.video);
#endif
        }

        // shown to the other clients and in the server log
        std::string client_name()
        {
#if defined(ROBV_PLATFORM_ANDROID)
            char model[PROP_VALUE_MAX] = {};
            if (__system_property_get("ro.product.model", model) > 0) { return model; }
            return "Android";
#else
            char host[256] = {};
            if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) { return host; }
            return "robv-client";
#endif
        }

        void send(app_t& app, intent_kind_e kind, f32 dx, f32 dy, f32 scalar)
        { net::send_intent(app.net, {.kind = kind, .dx = dx, .dy = dy, .scalar = scalar}); }

        void send_scalar(app_t& app, intent_kind_e kind, f32 scalar) { send(app, kind, 0.0f, 0.0f, scalar); }

        // touches and clicks on the panel or an open popup belong to ImGui, not the camera
        bool over_ui(const app_t& app, f32 x, f32 y)
        {
            if (!app.imgui_ready) { return false; }
            if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) { return true; }
            return x >= app.panel_min[0] && x < app.panel_max[0] && y >= app.panel_min[1] && y < app.panel_max[1];
        }

        void window_size(const app_t& app, f32& w, f32& h)
        {
            int iw = 0;
            int ih = 0;
            SDL_GetWindowSize(app.window, &iw, &ih);
            w = static_cast<f32>(std::max(iw, 1));
            h = static_cast<f32>(std::max(ih, 1));
        }

        // drag right turns the scene with the finger, drag down looks from higher up
        void add_orbit(gesture_t& g, f32 dx_px, f32 dy_px, f32 height)
        {
            g.orbit_dx -= dx_px / height * orbit_per_height;
            g.orbit_dy += dy_px / height * orbit_per_height;
        }

        i32 find_finger(const gesture_t& g, u64 id)
        {
            for (u32 i = 0; i < g.finger_count; ++i)
            {
                if (g.fingers[i] == id) { return static_cast<i32>(i); }
            }
            return -1;
        }

        // one finger orbits, two pan with their midpoint and zoom with their spread
        void handle_finger(app_t& app, const SDL_TouchFingerEvent& e, SDL_EventType type)
        {
            gesture_t& g = app.gesture;
            f32 w = 0.0f;
            f32 h = 0.0f;
            window_size(app, w, h);
            const f32 x = e.x * w;
            const f32 y = e.y * h;
            const i32 index = find_finger(g, e.fingerID);

            if (type == SDL_EVENT_FINGER_DOWN)
            {
                if (index < 0 && g.finger_count < 2 && !over_ui(app, x, y))
                {
                    g.fingers[g.finger_count] = e.fingerID;
                    g.finger_x[g.finger_count] = x;
                    g.finger_y[g.finger_count] = y;
                    ++g.finger_count;
                }
                return;
            }
            if (index < 0) { return; }

            if (type == SDL_EVENT_FINGER_MOTION)
            {
                if (g.finger_count == 1) { add_orbit(g, x - g.finger_x[0], y - g.finger_y[0], h); }
                else
                {
                    const u32 other = index == 0 ? 1 : 0;
                    const f32 ox = g.finger_x[other];
                    const f32 oy = g.finger_y[other];
                    const f32 old_span = std::hypot(g.finger_x[index] - ox, g.finger_y[index] - oy);
                    const f32 new_span = std::hypot(x - ox, y - oy);
                    // the midpoint moves half as far as the finger that moved
                    g.pan_dx += 0.5f * (x - g.finger_x[index]) / h;
                    g.pan_dy += 0.5f * (y - g.finger_y[index]) / h;
                    if (old_span > 1.0f && new_span > 1.0f) { g.zoom += std::log(new_span / old_span); }
                }
                g.finger_x[index] = x;
                g.finger_y[index] = y;
                return;
            }

            // up or cancelled
            for (u32 i = static_cast<u32>(index); i + 1 < g.finger_count; ++i)
            {
                g.fingers[i] = g.fingers[i + 1];
                g.finger_x[i] = g.finger_x[i + 1];
                g.finger_y[i] = g.finger_y[i + 1];
            }
            --g.finger_count;
        }

        // touch also arrives as SDL_TOUCH_MOUSEID mouse events for ImGui, the camera takes it as fingers
        void handle_input(app_t& app, const SDL_Event& event)
        {
            gesture_t& g = app.gesture;
            switch (event.type)
            {
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
                handle_finger(app, event.tfinger, static_cast<SDL_EventType>(event.type));
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.which == SDL_TOUCH_MOUSEID || over_ui(app, event.button.x, event.button.y)) { break; }
                if (event.button.button == SDL_BUTTON_LEFT) { g.mouse_orbit = true; }
                else if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE)
                {
                    g.mouse_pan = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.which == SDL_TOUCH_MOUSEID) { break; }
                if (event.button.button == SDL_BUTTON_LEFT) { g.mouse_orbit = false; }
                else
                {
                    g.mouse_pan = false;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
            {
                if (event.motion.which == SDL_TOUCH_MOUSEID) { break; }
                f32 w = 0.0f;
                f32 h = 0.0f;
                window_size(app, w, h);
                if (g.mouse_orbit) { add_orbit(g, event.motion.xrel, event.motion.yrel, h); }
                else if (g.mouse_pan)
                {
                    g.pan_dx += event.motion.xrel / h;
                    g.pan_dy += event.motion.yrel / h;
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (event.wheel.which == SDL_TOUCH_MOUSEID || over_ui(app, event.wheel.mouse_x, event.wheel.mouse_y))
                {
                    break;
                }
                g.zoom += event.wheel.y * zoom_per_wheel_step;
                break;
            default: break;
            }
        }

        // what accumulated since the last frame, one intent per kind
        void send_gesture(app_t& app)
        {
            gesture_t& g = app.gesture;
            if (g.orbit_dx != 0.0f || g.orbit_dy != 0.0f)
            {
                send(app, intent_kind_e::orbit, g.orbit_dx, g.orbit_dy, 0.0f);
            }
            if (g.pan_dx != 0.0f || g.pan_dy != 0.0f) { send(app, intent_kind_e::pan, g.pan_dx, g.pan_dy, 0.0f); }
            if (g.zoom != 0.0f) { send_scalar(app, intent_kind_e::zoom, g.zoom); }
            g.orbit_dx = g.orbit_dy = g.pan_dx = g.pan_dy = g.zoom = 0.0f;
        }

        void draw_view_controls(app_t& app)
        {
            const view_state_t& v = app.connection.view;

            ImGui::SeparatorText("View");
            ImGui::Text("yaw %+.2f  pitch %+.2f  distance %.1f", static_cast<f64>(v.camera.yaw),
                        static_cast<f64>(v.camera.pitch), static_cast<f64>(v.camera.distance));
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) { send_scalar(app, intent_kind_e::reset_camera, 0.0f); }

            const char* color_modes[] = {"intensity", "height", "rgb", "flat"};
            i32 color_mode = static_cast<i32>(v.render.color_mode);
            if (ImGui::Combo("colour", &color_mode, color_modes, IM_ARRAYSIZE(color_modes)))
            {
                send_scalar(app, intent_kind_e::set_color_mode, static_cast<f32>(color_mode));
            }

            f32 point_size = v.render.point_size;
            if (ImGui::SliderFloat("point size", &point_size, 1.0f, 32.0f, "%.1f"))
            {
                send_scalar(app, intent_kind_e::set_point_size, point_size);
            }

            bool grid = v.render.show_grid;
            if (ImGui::Checkbox("grid", &grid)) { send_scalar(app, intent_kind_e::set_grid, grid ? 1.0f : 0.0f); }
            ImGui::SameLine();
            bool axes = v.render.show_axes;
            if (ImGui::Checkbox("axes", &axes)) { send_scalar(app, intent_kind_e::set_axes, axes ? 1.0f : 0.0f); }
            ImGui::SameLine();
            bool edl = v.render.eye_dome_lighting;
            if (ImGui::Checkbox("EDL", &edl))
            {
                send_scalar(app, intent_kind_e::set_eye_dome_lighting, edl ? 1.0f : 0.0f);
            }

            const source_info_msg_t& source = app.connection.source;
            ImGui::SeparatorText("Source");
            ImGui::TextWrapped("%s", source.source_uri.c_str());
            ImGui::Text("%llu points, %zu fields, topic %s", static_cast<unsigned long long>(source.point_count),
                        source.fields.size(), v.active_topic.empty() ? "-" : v.active_topic.c_str());
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
            ImGui::TextColored(state_color, "%s", net::state_name(c.state));
            ImGui::SameLine();
            if (c.state == connection_state_e::disconnected) { ImGui::TextDisabled("(%s)", c.status.c_str()); }
            else
            {
                ImGui::TextDisabled("(%s, %s)", c.server.c_str(), c.status.c_str());
            }

            const bool submitted = ImGui::InputText("server", app.server_text, sizeof(app.server_text),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
            const bool idle = c.state == connection_state_e::disconnected;
            const bool edited = !idle && c.server != app.server_text;
            if (idle || edited)
            {
                if (ImGui::Button("Connect") || submitted) { net::connect(app.net, app.server_text); }
            }
            if (!idle)
            {
                if (edited) { ImGui::SameLine(); }
                const char* stop = c.state == connection_state_e::connecting ? "Cancel" : "Disconnect";
                if (ImGui::Button(stop)) { net::disconnect(app.net); }
            }
            if (c.state == connection_state_e::connected)
            {
                ImGui::Text("client %u, rtt %.1f ms, tick %llu", c.client_id, static_cast<f64>(c.rtt_ms),
                            static_cast<unsigned long long>(c.view.tick));
            }
            if (c.has_view) { draw_view_controls(app); }

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
            ImGui::Text("decoder %s, %llu frames, %llu errors", decoder::kind_name(app.decoder.config.kind),
                        static_cast<unsigned long long>(app.decoder.frames_out),
                        static_cast<unsigned long long>(app.decoder.errors));
            if (c.state == connection_state_e::connected)
            {
                ImGui::Text("RTP %llu frames, %llu packets lost", static_cast<unsigned long long>(c.video_units),
                            static_cast<unsigned long long>(c.video_lost));
                if (app.video_age_ms != 0.0f)
                {
                    ImGui::Text("render to decode %.0f ms%s", static_cast<f64>(app.video_age_ms),
                                p.display_timing ? "" : ", display time not reported");
                    if (p.display_timing)
                    {
                        ImGui::SameLine();
                        ImGui::Text("+ %.0f ms to screen", static_cast<f64>(p.queue_to_screen_ms));
                    }
                }
            }
            if (app.video.available)
            {
                ImGui::Text("video %ux%u, %llu uploads", app.video.extent.width, app.video.extent.height,
                            static_cast<unsigned long long>(app.video.uploads));
            }
            else
            {
                ImGui::TextDisabled("no video: this GPU cannot sample NV12");
            }
            if (p.display_timing)
            {
                ImGui::Text("present -> screen %.1f ms, %u frames queued", static_cast<f64>(p.queue_to_screen_ms),
                            p.next_present_id - 1 - p.last_shown_id);
            }
            ImGui::Text("%.0f draws/s%s, last frame %.2f ms CPU, ui scale %.2f", static_cast<f64>(app.draws_per_second),
                        app.config.continuous ? " (continuous)" : " (on demand)", static_cast<f64>(app.frame_cpu_ms),
                        static_cast<f64>(app.ui_scale));

            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            app.panel_min[0] = pos.x;
            app.panel_min[1] = pos.y;
            app.panel_max[0] = pos.x + size.x;
            app.panel_max[1] = pos.y + size.y;
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

        if (!video_view::init(app.video, app.vk, app.presenter.render_pass))
        {
            fail(app, "Could not set up video display, see the log.");
            return false;
        }

        if (!init_imgui(app))
        {
            fail(app, "Could not initialise ImGui, see the log.");
            return false;
        }

        app.net_event = SDL_RegisterEvents(2);
        app.decoder_event = app.net_event != 0 ? app.net_event + 1 : 0;

        const decoder_config_t decoder_config{.kind = config.decoder, .wake_event = app.decoder_event};
        if (!decoder::open(app.decoder, decoder_config))
        {
            fail(app, std::string("The ") + decoder::kind_name(config.decoder) + " decoder is not available.");
            return false;
        }

        if (app.net_event == 0 || !net::start(app.net, client_name(), app.net_event))
        {
            fail(app, "Could not start the network thread, see the log.");
            return false;
        }
#if defined(ROBV_PLATFORM_ANDROID)
        const char* default_server = "192.168.1.10:9400";
#else
        const char* default_server = "127.0.0.1:9400";
#endif
        std::snprintf(app.server_text, sizeof(app.server_text), "%s",
                      config.server.empty() ? default_server : config.server.c_str());
        if (!config.server.empty()) { net::connect(app.net, config.server); }

        ROBV_LOG_INFO("APP", "ready in {:.0f} ms", static_cast<f64>(now_ns() - app.start_ns) * 1e-6);
        return true;
    }

    void shutdown(app_t& app)
    {
        SDL_RemoveEventWatch(on_lifecycle, &app);
        vsync::stop(app.vsync);
        net::stop(app.net);
        if (app.decoder.units_in > 0)
        {
            ROBV_LOG_INFO("APP", "video: {} units in, {} decoded, {} errors, {} shown", app.decoder.units_in,
                          app.decoder.decoded, app.decoder.errors, app.video.uploads);
        }
        if (app.vk.device) { vkDeviceWaitIdle(app.vk.device); }
        release_images(app);
        decoder::close(app.decoder);
        video_view::shutdown(app.video);
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
        if (app.net_event != 0 && event.type == app.net_event)
        {
            const u32 previous_client = app.connection.client_id;
            if (net::poll(app.net, app.connection)) { app.pending_frames = std::max(app.pending_frames, 1u); }

            if (app.connection.client_id != 0 && app.connection.client_id != previous_client)
            {
                if (app.connection.video.width > 0 && app.connection.video.height > 0)
                {
                    app.decoder.config.width = app.connection.video.width;
                    app.decoder.config.height = app.connection.video.height;
                }
                release_images(app);
                decoder::reset(app.decoder);
            }

            net::take_video(app.net, app.units);
            for (const rtp_unit_t& unit : app.units)
            {
                // 90 kHz RTP clock to nanoseconds
                const u64 pts_ns = static_cast<u64>(unit.timestamp) * 100'000 / 9;
                if (!decoder::submit(app.decoder, unit.data, pts_ns)) { net::request_keyframe(app.net); }
            }
            if (!app.units.empty()) { app.pending_frames = std::max(app.pending_frames, 1u); }
            return;
        }

        // the decoder finished something: pictures are passed on now, acquire takes them on the next frame
        if (app.decoder_event != 0 && event.type == app.decoder_event)
        {
            decoder::service(app.decoder);
            app.pending_frames = std::max(app.pending_frames, 1u);
            return;
        }

        if (app.imgui_ready) { ImGui_ImplSDL3_ProcessEvent(&event); }
        app.pending_frames = std::max(app.pending_frames, frames_after_input);
        handle_input(app, event);

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
            const u64 due = next_due_ns(app);
            const u64 until_ns = due > now ? due - now : 0;
            const Sint32 timeout_ms = static_cast<Sint32>(std::max<u64>(1, (until_ns + 999'999) / 1'000'000));
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, timeout_ms)) { handle_event(app, event); }
            now = now_ns();
        }

        if (!frame_due(app, now)) { return; }
        // only motion frames need the vsync grid, pacing the idle refresh keeps the vsync thread awake
        // video frames are motion: unpaced between touch batches they slip past the queue cap,
        // and Android queues every one, ~65 ms of backlog during a drag
        const bool video_due = streaming(app) && now >= decoder::next_frame_ns(app.decoder);
        const bool in_motion = app.config.continuous || app.pending_frames > 0 || video_due;
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

        send_gesture(app);
        retire_images(app);

        void* unshown = app.decoded_pending ? app.decoded.image : nullptr;
        if (streaming(app) && decoder::acquire(app.decoder, start, app.decoded))
        {
            if (unshown && unshown != app.decoded.image) { decoder::release(app.decoder, unshown); }
            app.decoded_pending = true;
            measure_video_age(app);
        }
        // polls display timing so the panel is current on idle frames too
        presenter::frames_queued(app.presenter);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_panel(app);
        ImGui::Render();

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        app.last_frame_presented = presenter::begin_frame(app.presenter, cmd);
        if (app.last_frame_presented)
        {
            void* replaced = nullptr;
            if (app.decoded_pending && app.decoded.image)
            {
                bool shown = false;
#if defined(ROBV_PLATFORM_ANDROID)
                shown = video_view::show_hardware_buffer(app.video, cmd,
                                                         static_cast<AHardwareBuffer*>(app.decoded.hardware_buffer),
                                                         app.decoded.width, app.decoded.height);
#endif
                if (shown)
                {
                    replaced = app.shown_image;
                    app.shown_image = app.decoded.image;
                }
                else
                {
                    decoder::release(app.decoder, app.decoded.image);
                }
                app.decoded.image = nullptr;
                app.decoded_pending = false;
            }
            else if (app.decoded_pending)
            {
                video_view::upload(app.video, cmd, app.presenter.frame_index, app.decoded.cpu);
                app.decoded_pending = false;
            }
            presenter::begin_pass(app.presenter, clear_color);
            if (streaming(app)) { video_view::draw(app.video, cmd, app.presenter.extent); }
            ImGui_ImplVulkan_SetMinImageCount(std::max(2u, app.presenter.min_image_count));
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            presenter::end_frame(app.presenter);

            if (replaced) { app.retiring_images.push_back({replaced, app.presenter.timeline.value}); }
            ++app.frames;
            ++app.rate_window_frames;
        }

        app.last_draw_ns = start;
        if (app.pending_frames > 0) { --app.pending_frames; }
        app.next_idle_ns = start + (ImGui::GetIO().WantTextInput ? text_input_period_ns : idle_period_ns);

        const u64 end = now_ns();
        app.frame_cpu_ms = static_cast<f32>(end - start) * 1e-6f;

        if (!streaming(app)) { app.stream_log_ns = 0; }
        else if (app.stream_log_ns == 0)
        {
            app.stream_log_ns = end;
            app.stream_log_frames = app.decoder.frames_out;
        }
        else if (end - app.stream_log_ns >= stream_log_period_ns)
        {
            const f64 seconds = static_cast<f64>(end - app.stream_log_ns) * 1e-9;
            ROBV_LOG_INFO("APP", "stream: {:.1f} fps shown, render to decode {:.0f} ms, + {:.0f} ms to screen, {} lost",
                          static_cast<f64>(app.decoder.frames_out - app.stream_log_frames) / seconds,
                          static_cast<f64>(app.video_age_ms), static_cast<f64>(app.presenter.queue_to_screen_ms),
                          app.connection.video_lost);
            app.stream_log_ns = end;
            app.stream_log_frames = app.decoder.frames_out;
        }
        if (end - app.rate_window_ns >= 1'000'000'000ull)
        {
            app.draws_per_second = static_cast<f32>(static_cast<f64>(app.rate_window_frames) * 1e9 /
                                                    static_cast<f64>(end - app.rate_window_ns));
            app.rate_window_ns = end;
            app.rate_window_frames = 0;
        }
    }
} // namespace robv::app
