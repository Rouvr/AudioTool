// ============================================================================
// AudioTool (AT) — Native Windows Graphical User Interface (DX11 + ImGui)
// ============================================================================

#include "at_version.h"
#include "common/AppPathsAndRegistry.h"
#include "engine/AudioEngine.h"
#include "gui/CanvasRenderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <commdlg.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

// Forward declare Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device*           g_pd3dDevice = nullptr;
ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
IDXGISwapChain*         g_pSwapChain = nullptr;
UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::string OpenProfileFileDialog(HWND hWnd, bool is_save) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"AudioTool Profiles (*.atprof)\0*.atprof\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"atprof";
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | (is_save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    std::wstring init_dir = at::AppPathsAndRegistry::get_profiles_dir().wstring();
    ofn.lpstrInitialDir = init_dir.c_str();

    BOOL ok = is_save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return "";

    char utf8_path[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, filename, -1, utf8_path, sizeof(utf8_path), nullptr, nullptr);
    return utf8_path;
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    at::AppPathsAndRegistry::harden_dll_search_order();

    // 1. Initialize Audio Engine
    at::AudioEngine engine;
    at::EngineConfig cfg = at::AudioEngine::load_config_file();
    std::string err;
    if (!engine.initialize(cfg, &err)) {
        MessageBoxA(nullptr, ("Engine initialization failed: " + err).c_str(), "AudioTool Error", MB_ICONERROR);
        return 1;
    }

    // Auto-create initial default profile
    at::ProfileGraph* cur_profile = engine.create_profile("Main Setup");

    // Spawn default nodes: Virtual Input -> Volume Mixer -> Output
    if (cur_profile) {
        uint32_t vin  = cur_profile->spawn_node("at.builtin.audio.virtual_input", "Microphone");
        uint32_t vol  = cur_profile->spawn_node("at.builtin.audio.volume", "Volume Mixer");
        uint32_t vout = cur_profile->spawn_node("at.builtin.audio.output", "Speakers / Cable");
        uint32_t tmr  = cur_profile->spawn_node("at.builtin.control.timer", "Periodic Pulse");

        if (vin)  cur_profile->set_node_ui(vin,  { 60.0f,  80.0f, true});
        if (vol)  cur_profile->set_node_ui(vol,  {430.0f,  60.0f, true});
        if (vout) cur_profile->set_node_ui(vout, {830.0f,  80.0f, true});
        if (tmr)  cur_profile->set_node_ui(tmr,  { 60.0f, 340.0f, true});

        if (vin && vol)  cur_profile->connect_audio(vin, vol);
        if (vol && vout) cur_profile->connect_audio(vol, vout);
    }

    // Start real-time audio thread
    engine.start_realtime_loop();

    // 2. Win32 Application Window setup
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"AudioToolMainWindowClass", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Rouvr AudioTool", WS_OVERLAPPEDWINDOW, 80, 60, 1360, 820, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 3. ImGui Setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string ini_path = (at::AppPathsAndRegistry::get_local_data_root() / "imgui.ini").string();
    io.IniFilename = ini_path.c_str();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding  = 3.0f;
    style.GrabRounding   = 3.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    at::CanvasRenderer canvas_renderer;

    uint32_t active_profile_id = cur_profile ? cur_profile->id() : 1;
    uint32_t pending_select_profile_id = active_profile_id;
    bool show_settings_modal = false;
    bool show_app_router_window = false;
    std::vector<at::AudioAppSession> cached_audio_apps;
    double last_apps_refresh_time = -10.0;
    std::string router_status_msg = "Ready";
    char app_filter_buf[128] = {};
    char new_preset_name_buf[128] = {};
    int selected_preset_idx = 0;

    // 4. Main Event Loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Global Keyboard Shortcuts (when not typing in a text box)
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                auto* p = engine.create_profile("New Profile");
                if (p) {
                    active_profile_id = p->id();
                    pending_select_profile_id = p->id();
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
                std::string fpath = OpenProfileFileDialog(hwnd, false);
                if (!fpath.empty()) {
                    std::string load_err;
                    auto* p = engine.load_profile(fpath, &load_err);
                    if (p) {
                        active_profile_id = p->id();
                        pending_select_profile_id = p->id();
                    } else {
                        MessageBoxA(hwnd, load_err.c_str(), "Failed to Load Profile", MB_ICONWARNING);
                    }
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                auto* p = engine.find_profile(active_profile_id);
                if (p) {
                    std::string fpath = io.KeyShift ? "" : p->metadata().file_path;
                    if (fpath.empty()) fpath = OpenProfileFileDialog(hwnd, true);
                    if (!fpath.empty()) {
                        std::string save_err;
                        if (!engine.save_profile(active_profile_id, fpath, &save_err)) {
                            MessageBoxA(hwnd, save_err.c_str(), "Failed to Save", MB_ICONWARNING);
                        }
                    }
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                show_app_router_window = !show_app_router_window;
            }
            if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                if (auto* p = engine.find_profile(active_profile_id)) {
                    at::ProfileMetadata m = p->metadata();
                    std::string next_view = (m.active_plane_view == "AUDIO") ? "CONTROL" : "AUDIO";
                    p->set_canvas_state(m.canvas_pan_x, m.canvas_pan_y, next_view, m.canvas_zoom);
                }
            }
        }

        // Top Fullscreen Host Window
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                      ImGuiWindowFlags_MenuBar;

        ImGui::Begin("AudioToolMainHost", nullptr, host_flags);
        ImGui::PopStyleVar(3);

        // Top Menu Bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Profile Tab", "Ctrl+N")) {
                    auto* p = engine.create_profile("New Profile");
                    if (p) {
                        active_profile_id = p->id();
                        pending_select_profile_id = p->id();
                    }
                }
                if (ImGui::MenuItem("Load Profile...", "Ctrl+O")) {
                    std::string fpath = OpenProfileFileDialog(hwnd, false);
                    if (!fpath.empty()) {
                        std::string load_err;
                        auto* p = engine.load_profile(fpath, &load_err);
                        if (p) {
                            active_profile_id = p->id();
                            pending_select_profile_id = p->id();
                        } else {
                            MessageBoxA(hwnd, load_err.c_str(), "Failed to Load Profile", MB_ICONWARNING);
                        }
                    }
                }
                if (ImGui::MenuItem("Save Profile", "Ctrl+S")) {
                    auto* p = engine.find_profile(active_profile_id);
                    if (p) {
                        std::string fpath = p->metadata().file_path;
                        if (fpath.empty()) fpath = OpenProfileFileDialog(hwnd, true);
                        if (!fpath.empty()) {
                            std::string save_err;
                            if (!engine.save_profile(active_profile_id, fpath, &save_err)) {
                                MessageBoxA(hwnd, save_err.c_str(), "Failed to Save", MB_ICONWARNING);
                            }
                        }
                    }
                }
                if (ImGui::MenuItem("Save Profile As...", "Ctrl+Shift+S")) {
                    std::string fpath = OpenProfileFileDialog(hwnd, true);
                    if (!fpath.empty()) {
                        std::string save_err;
                        if (!engine.save_profile(active_profile_id, fpath, &save_err)) {
                            MessageBoxA(hwnd, save_err.c_str(), "Failed to Save", MB_ICONWARNING);
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    done = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Engine")) {
                if (ImGui::MenuItem("Engine Settings...")) {
                    show_settings_modal = true;
                }
                if (ImGui::MenuItem("Refresh Audio Devices")) {
                    canvas_renderer.invalidate_device_cache();
                }
                if (ImGui::MenuItem("Rescan Plugins Folder")) {
                    std::vector<std::string> errs;
                    size_t loaded = engine.rescan_plugins(&errs);
                    canvas_renderer.invalidate_device_cache();
                    std::string rescan_msg = "Plugins reloaded: " + std::to_string(loaded);
                    MessageBoxA(hwnd, rescan_msg.c_str(), "Plugin Scan", MB_ICONINFORMATION);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Audio Router")) {
                if (ImGui::MenuItem("App Audio Output Editor...", "Ctrl+R")) {
                    show_app_router_window = true;
                }
                if (ImGui::MenuItem("Revert All to Original Routing")) {
                    engine.audio_router().revert_all_to_original();
                    router_status_msg = "Reverted all applications to original system output.";
                }
                ImGui::EndMenu();
            }

            ImGui::SameLine(0.0f, 16.0f);
            if (ImGui::SmallButton(" App Audio Router ")) {
                show_app_router_window = true;
            }

            // Right-aligned view switcher: [ Audio Stream View | Control / Data View ]
            float menu_w = ImGui::GetWindowWidth();
            auto* cur_prof = engine.find_profile(active_profile_id);
            if (cur_prof) {
                at::ProfileMetadata m = cur_prof->metadata();
                bool is_audio = (m.active_plane_view == "AUDIO");

                ImGui::SetCursorPosX(std::max(240.0f, menu_w - 330.0f));
                ImGui::TextUnformatted("Plane (Tab):");
                ImGui::SameLine();
                if (ImGui::RadioButton("Audio Stream", is_audio)) {
                    cur_prof->set_canvas_state(m.canvas_pan_x, m.canvas_pan_y, "AUDIO", m.canvas_zoom);
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Control / Data", !is_audio)) {
                    cur_prof->set_canvas_state(m.canvas_pan_x, m.canvas_pan_y, "CONTROL", m.canvas_zoom);
                }
            }

            ImGui::EndMenuBar();
        }

        // Multi-Profile Tab Bar
        auto prof_ids = engine.profile_ids();
        if (prof_ids.empty()) {
            auto* p = engine.create_profile("Profile 1");
            if (p) {
                active_profile_id = p->id();
                pending_select_profile_id = p->id();
            }
            prof_ids = engine.profile_ids();
        }

        uint32_t tab_to_close = 0;
        if (ImGui::BeginTabBar("ProfileTabs", ImGuiTabBarFlags_AutoSelectNewTabs)) {
            for (uint32_t pid : prof_ids) {
                auto* prof = engine.find_profile(pid);
                if (!prof) continue;

                bool is_prof_active = prof->is_active();
                std::string prefix = is_prof_active ? "[ON] " : "[PAUSED] ";
                std::string tab_label = prefix + prof->metadata().name + "##tab_" + std::to_string(pid);
                bool open = true;

                ImGuiTabItemFlags tab_flags = 0;
                if (pending_select_profile_id == pid) {
                    tab_flags |= ImGuiTabItemFlags_SetSelected;
                }

                if (ImGui::BeginTabItem(tab_label.c_str(), &open, tab_flags)) {
                    if (pending_select_profile_id == 0 || pending_select_profile_id == pid) {
                        active_profile_id = pid;
                    }
                    ImGui::EndTabItem();
                }

                // Right-click context menu on tab to toggle profile active/paused state
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(is_prof_active ? "Pause Profile Processing" : "Resume Profile Processing")) {
                        prof->set_active(!is_prof_active);
                    }
                    ImGui::EndPopup();
                }

                if (!open) {
                    tab_to_close = pid;
                }
            }
            pending_select_profile_id = 0;

            if (ImGui::TabItemButton("+##add_tab")) {
                auto* p = engine.create_profile("New Profile");
                if (p) {
                    active_profile_id = p->id();
                    pending_select_profile_id = p->id();
                }
            }
            ImGui::EndTabBar();
        }

        if (tab_to_close != 0) {
            engine.close_profile(tab_to_close);
            auto remaining = engine.profile_ids();
            if (!remaining.empty()) {
                active_profile_id = remaining.front();
                pending_select_profile_id = active_profile_id;
            }
        }

        // Main Visual Editing Canvas (reserves 28 px at bottom for status bar)
        constexpr float kStatusBarHeight = 28.0f;
        auto* active_prof = engine.find_profile(active_profile_id);
        if (active_prof) {
            canvas_renderer.render(*active_prof, engine, kStatusBarHeight);
        }

        // Bottom Status Bar
        at::EngineStats stats = engine.stats();
        ImDrawList* host_dl = ImGui::GetWindowDrawList();
        ImVec2 win_pos = ImGui::GetWindowPos();
        ImVec2 win_sz  = ImGui::GetWindowSize();
        ImVec2 sb_min(win_pos.x, win_pos.y + win_sz.y - kStatusBarHeight);
        ImVec2 sb_max(win_pos.x + win_sz.x, win_pos.y + win_sz.y);
        host_dl->AddRectFilled(sb_min, sb_max, IM_COL32(18, 20, 24, 255));
        host_dl->AddLine(sb_min, ImVec2(sb_max.x, sb_min.y), IM_COL32(48, 52, 62, 255), 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(sb_min.x + 12.0f, sb_min.y + 5.0f));
        ImGui::Text("Engine: %s | Sample Rate: %u Hz | Buffer: %u smp (%.1f ms) | DSP Load: %.1f%% | Active Devices: %zu | Build: %s",
                    stats.running ? "Running" : "Stopped",
                    engine.config().sample_rate,
                    engine.config().buffer_size_frames,
                    (1000.0 * engine.config().buffer_size_frames) / std::max(1u, engine.config().sample_rate),
                    stats.dsp_load * 100.0f,
                    stats.open_devices,
                    AT_BUILD_ID);

        // Engine Settings Modal Dialog
        if (show_settings_modal) {
            ImGui::OpenPopup("Engine Settings");
            show_settings_modal = false;
        }
        if (ImGui::BeginPopupModal("Engine Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static int sel_sr = static_cast<int>(engine.config().sample_rate);
            static int sel_bs = static_cast<int>(engine.config().buffer_size_frames);
            static int sel_mc = static_cast<int>(engine.config().max_connections_per_port);

            if (ImGui::IsWindowAppearing()) {
                sel_sr = static_cast<int>(engine.config().sample_rate);
                sel_bs = static_cast<int>(engine.config().buffer_size_frames);
                sel_mc = static_cast<int>(engine.config().max_connections_per_port);
            }

            ImGui::Text("Runtime Audio Parameters (safe live reload):");
            ImGui::Separator();

            int sr_options[] = { 44100, 48000, 88200, 96000 };
            const char* sr_labels[] = { "44,100 Hz", "48,000 Hz", "88,200 Hz", "96,000 Hz" };
            int cur_sr_idx = (sel_sr == 44100) ? 0 : ((sel_sr == 48000) ? 1 : ((sel_sr == 88200) ? 2 : 3));
            if (ImGui::Combo("Sample Rate", &cur_sr_idx, sr_labels, 4)) {
                sel_sr = sr_options[cur_sr_idx];
            }

            int bs_options[] = { 64, 128, 256, 512, 1024 };
            const char* bs_labels[] = { "64 samples (1.3 ms)", "128 samples (2.7 ms)", "256 samples (5.3 ms)", "512 samples (10.7 ms)", "1024 samples (21.3 ms)" };
            int cur_bs_idx = (sel_bs <= 64) ? 0 : ((sel_bs <= 128) ? 1 : ((sel_bs <= 256) ? 2 : ((sel_bs <= 512) ? 3 : 4)));
            if (ImGui::Combo("Buffer Size", &cur_bs_idx, bs_labels, 5)) {
                sel_bs = bs_options[cur_bs_idx];
            }

            ImGui::SliderInt("Max Connections/Port", &sel_mc, 1, 16);

            if (ImGui::Button("Apply & Reload Engine", ImVec2(160, 0))) {
                at::EngineConfig new_cfg;
                new_cfg.sample_rate = static_cast<uint32_t>(sel_sr);
                new_cfg.buffer_size_frames = static_cast<uint32_t>(sel_bs);
                new_cfg.max_connections_per_port = static_cast<uint32_t>(sel_mc);
                engine.reload_config(new_cfg);
                at::AudioEngine::save_config_file(new_cfg);
                canvas_renderer.invalidate_device_cache();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Application Audio Output Router Popup Window
        if (show_app_router_window) {
            ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Application Audio Output Router", &show_app_router_window)) {
                double cur_time = ImGui::GetTime();
                auto all_devs = engine.device_manager().enumerate_devices();
                std::vector<at::AudioDeviceInfo> out_render_devices;
                for (const auto& d : all_devs) {
                    if (d.is_output_render) out_render_devices.push_back(d);
                }

                if (cached_audio_apps.empty() || (cur_time - last_apps_refresh_time > 2.5)) {
                    cached_audio_apps = engine.audio_router().enumerate_apps(all_devs);
                    last_apps_refresh_time = cur_time;
                }

                auto* active_profile_ptr = engine.find_profile(active_profile_id);

                // Top action toolbar
                if (ImGui::Button("Refresh Apps")) {
                    cached_audio_apps = engine.audio_router().enumerate_apps(all_devs);
                    last_apps_refresh_time = cur_time;
                    router_status_msg = "Application list refreshed.";
                }
                ImGui::SameLine();
                if (ImGui::Button("Save to Profile")) {
                    if (active_profile_ptr) {
                        for (const auto& a : cached_audio_apps) {
                            if (!a.current_device_id.empty()) {
                                active_profile_ptr->set_app_routing_entry(a.process_name, a.current_device_id);
                            }
                        }
                        std::string p_path = active_profile_ptr->metadata().file_path;
                        if (!p_path.empty()) {
                            std::string s_err;
                            if (engine.save_profile(active_profile_id, p_path, &s_err)) {
                                router_status_msg = "Routing saved to profile file: " + p_path;
                            } else {
                                router_status_msg = "Failed to save profile: " + s_err;
                            }
                        } else {
                            router_status_msg = "Routing saved to active profile tab.";
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Apply Profile Routing")) {
                    if (active_profile_ptr) {
                        auto prof_routing = active_profile_ptr->app_routing();
                        engine.audio_router().apply_routing_map(prof_routing);
                        cached_audio_apps = engine.audio_router().enumerate_apps(all_devs);
                        router_status_msg = "Applied " + std::to_string(prof_routing.size()) + " routing rules from profile.";
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset All to Original")) {
                    engine.audio_router().revert_all_to_original();
                    cached_audio_apps = engine.audio_router().enumerate_apps(all_devs);
                    router_status_msg = "Reverted all applications to original system output.";
                }

                ImGui::Separator();

                // Standalone Routing Presets row
                auto preset_names = engine.audio_router().list_presets();
                ImGui::TextUnformatted("Preset:");
                ImGui::SameLine();
                std::string current_preset_label = (selected_preset_idx >= 0 && selected_preset_idx < static_cast<int>(preset_names.size()))
                                                   ? preset_names[selected_preset_idx]
                                                   : "<Select Preset...>";
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::BeginCombo("##PresetCombo", current_preset_label.c_str())) {
                    for (int i = 0; i < static_cast<int>(preset_names.size()); ++i) {
                        bool is_sel = (i == selected_preset_idx);
                        if (ImGui::Selectable(preset_names[i].c_str(), is_sel)) {
                            selected_preset_idx = i;
                        }
                        if (is_sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::SameLine();
                if (ImGui::Button("Apply Preset")) {
                    if (selected_preset_idx >= 0 && selected_preset_idx < static_cast<int>(preset_names.size())) {
                        std::map<std::string, std::string> mapping;
                        std::string load_err;
                        if (engine.audio_router().load_preset(preset_names[selected_preset_idx], &mapping, &load_err)) {
                            engine.audio_router().apply_routing_map(mapping);
                            if (active_profile_ptr) {
                                active_profile_ptr->set_app_routing(mapping);
                            }
                            cached_audio_apps = engine.audio_router().enumerate_apps(all_devs);
                            router_status_msg = "Applied preset '" + preset_names[selected_preset_idx] + "'.";
                        } else {
                            router_status_msg = "Failed to load preset: " + load_err;
                        }
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Save Preset As...")) {
                    ImGui::OpenPopup("Save Preset Dialog");
                    new_preset_name_buf[0] = '\0';
                }

                ImGui::SameLine();
                if (ImGui::Button("Delete Preset")) {
                    if (selected_preset_idx >= 0 && selected_preset_idx < static_cast<int>(preset_names.size())) {
                        std::string del_err;
                        if (engine.audio_router().delete_preset(preset_names[selected_preset_idx], &del_err)) {
                            router_status_msg = "Deleted preset '" + preset_names[selected_preset_idx] + "'.";
                            selected_preset_idx = 0;
                        } else {
                            router_status_msg = "Failed to delete preset: " + del_err;
                        }
                    }
                }

                // Modal for Save Preset As...
                if (ImGui::BeginPopupModal("Save Preset Dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Enter a name for the routing preset:");
                    ImGui::InputText("##PresetNameInput", new_preset_name_buf, sizeof(new_preset_name_buf));
                    if (ImGui::Button("Save", ImVec2(100, 0))) {
                        std::string p_name = new_preset_name_buf;
                        if (!p_name.empty()) {
                            std::map<std::string, std::string> current_mapping;
                            for (const auto& a : cached_audio_apps) {
                                if (!a.current_device_id.empty()) {
                                    current_mapping[a.process_name] = a.current_device_id;
                                }
                            }
                            std::string s_err;
                            if (engine.audio_router().save_preset(p_name, current_mapping, &s_err)) {
                                router_status_msg = "Saved preset '" + p_name + "'.";
                                ImGui::CloseCurrentPopup();
                            } else {
                                router_status_msg = "Error saving preset: " + s_err;
                            }
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::SameLine();
                ImGui::TextUnformatted("| Filter:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                ImGui::InputTextWithHint("##FilterInput", "Search apps...", app_filter_buf, sizeof(app_filter_buf));

                ImGui::Separator();

                // Table of Audio Applications
                float table_h = ImGui::GetContentRegionAvail().y - 42.0f;
                if (ImGui::BeginTable("AppsRoutingTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, table_h))) {
                    ImGui::TableSetupColumn("Application", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 65.0f);
                    ImGui::TableSetupColumn("Audio Output Playback Device", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 85.0f);
                    ImGui::TableHeadersRow();

                    std::string filter_str = app_filter_buf;
                    std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                    int row_idx = 0;
                    for (auto& app : cached_audio_apps) {
                        std::string proc_lower = app.process_name;
                        std::string disp_lower = app.display_name;
                        std::transform(proc_lower.begin(), proc_lower.end(), proc_lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        std::transform(disp_lower.begin(), disp_lower.end(), disp_lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                        if (!filter_str.empty()) {
                            if (proc_lower.find(filter_str) == std::string::npos &&
                                disp_lower.find(filter_str) == std::string::npos) {
                                continue;
                            }
                        }

                        ImGui::TableNextRow();
                        ImGui::PushID(row_idx++);

                        // Col 0: App Name
                        ImGui::TableSetColumnIndex(0);
                        if (!app.display_name.empty() && app.display_name != app.process_name) {
                            ImGui::Text("%s (%s)", app.display_name.c_str(), app.process_name.c_str());
                        } else {
                            ImGui::Text("%s", app.process_name.c_str());
                        }

                        // Col 1: PID
                        ImGui::TableSetColumnIndex(1);
                        if (app.pid != 0) {
                            ImGui::Text("%u", app.pid);
                        } else {
                            ImGui::TextDisabled("-");
                        }

                        // Col 2: Output Device Dropdown
                        ImGui::TableSetColumnIndex(2);
                        std::string combo_preview = "Default (Windows System Output)";
                        for (const auto& dev : out_render_devices) {
                            if (dev.endpoint_id == app.current_device_id) {
                                combo_preview = dev.friendly_name;
                                break;
                            }
                        }

                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::BeginCombo("##DevCombo", combo_preview.c_str())) {
                            bool is_def_selected = app.current_device_id.empty() || app.current_device_id == "(default)";
                            if (ImGui::Selectable("Default (Windows System Output)", is_def_selected)) {
                                app.current_device_id = "";
                                app.current_device_name = "Default";
                                engine.audio_router().set_app_output_device(app.pid, app.process_name, "");
                                if (active_profile_ptr) {
                                    active_profile_ptr->set_app_routing_entry(app.process_name, "");
                                }
                                router_status_msg = "Set " + app.process_name + " -> Default Output";
                            }
                            if (is_def_selected) ImGui::SetItemDefaultFocus();

                            for (const auto& dev : out_render_devices) {
                                bool is_sel = (dev.endpoint_id == app.current_device_id);
                                if (ImGui::Selectable(dev.friendly_name.c_str(), is_sel)) {
                                    app.current_device_id = dev.endpoint_id;
                                    app.current_device_name = dev.friendly_name;
                                    engine.audio_router().set_app_output_device(app.pid, app.process_name, dev.endpoint_id);
                                    if (active_profile_ptr) {
                                        active_profile_ptr->set_app_routing_entry(app.process_name, dev.endpoint_id);
                                    }
                                    router_status_msg = "Routed " + app.process_name + " -> " + dev.friendly_name;
                                }
                                if (is_sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }

                        // Col 3: Status
                        ImGui::TableSetColumnIndex(3);
                        if (app.current_device_id.empty() || app.current_device_id == "(default)") {
                            ImGui::TextDisabled("Default");
                        } else {
                            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Routed");
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                ImGui::Separator();

                // Bottom row: Revert checkbox & Status & Close
                bool revert = engine.audio_router().get_revert_on_exit();
                if (ImGui::Checkbox("Revert to original configuration upon exit", &revert)) {
                    engine.audio_router().set_revert_on_exit(revert);
                }

                ImGui::SameLine(0.0f, 20.0f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "| %s", router_status_msg.c_str());

                float avail_w = ImGui::GetContentRegionAvail().x;
                if (avail_w > 90.0f) {
                    ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
                }
                if (ImGui::Button("Close", ImVec2(75, 0))) {
                    show_app_router_window = false;
                }
            }
            ImGui::End();
        }

        ImGui::End(); // End AudioToolMainHost

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.10f, 0.10f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // VSync enabled
    }

    // Cleanup
    if (engine.audio_router().get_revert_on_exit()) {
        engine.audio_router().revert_all_to_original();
    }
    engine.stop_realtime_loop();
    engine.shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

namespace {

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace
