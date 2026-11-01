// ============================================================================
// AudioTool (AT) — Native Windows Graphical User Interface (DX11 + ImGui)
// ============================================================================

#include "at_version.h"
#include "common/AppPathsAndRegistry.h"
#include "engine/AudioEngine.h"
#include "gui/CanvasRenderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <commdlg.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

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
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

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
        uint32_t vin = cur_profile->spawn_node("at.builtin.audio.virtual_input", "Microphone");
        uint32_t vol = cur_profile->spawn_node("at.builtin.audio.volume", "Volume Mixer");
        uint32_t vout = cur_profile->spawn_node("at.builtin.audio.output", "Speakers / Cable");
        uint32_t tmr = cur_profile->spawn_node("at.builtin.control.timer", "Periodic Pulse");

        if (vin) cur_profile->set_node_ui(vin, {100.0f, 120.0f, true});
        if (vol) cur_profile->set_node_ui(vol, {460.0f, 120.0f, true});
        if (vout) cur_profile->set_node_ui(vout, {820.0f, 120.0f, true});
        if (tmr) cur_profile->set_node_ui(tmr, {100.0f, 380.0f, true});

        if (vin && vol) cur_profile->connect_audio(vin, vol);
        if (vol && vout) cur_profile->connect_audio(vol, vout);
    }

    // Start real-time audio thread
    engine.start_realtime_loop();

    // 2. Win32 Application Window setup
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"AudioToolMainWindowClass", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Rouvr AudioTool", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

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

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding  = 3.0f;
    style.GrabRounding   = 3.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    at::CanvasRenderer canvas_renderer;

    uint32_t active_profile_id = cur_profile ? cur_profile->id() : 1;
    bool show_settings_modal = false;

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
                    if (p) active_profile_id = p->id();
                }
                if (ImGui::MenuItem("Load Profile...", "Ctrl+O")) {
                    std::string fpath = OpenProfileFileDialog(hwnd, false);
                    if (!fpath.empty()) {
                        std::string load_err;
                        auto* p = engine.load_profile(fpath, &load_err);
                        if (p) active_profile_id = p->id();
                        else MessageBoxA(hwnd, load_err.c_str(), "Failed to Load Profile", MB_ICONWARNING);
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
                if (ImGui::MenuItem("Rescan Plugins Folder")) {
                    std::vector<std::string> errs;
                    size_t loaded = engine.rescan_plugins(&errs);
                    std::string rescan_msg = "Plugins reloaded: " + std::to_string(loaded);
                    MessageBoxA(hwnd, rescan_msg.c_str(), "Plugin Scan", MB_ICONINFORMATION);
                }
                ImGui::EndMenu();
            }

            // Right-aligned view switcher: [ Audio Stream View | Data View ]
            float menu_w = ImGui::GetWindowWidth();
            auto* cur_prof = engine.find_profile(active_profile_id);
            if (cur_prof) {
                at::ProfileMetadata m = cur_prof->metadata();
                bool is_audio = (m.active_plane_view == "AUDIO");

                ImGui::SetCursorPosX(menu_w - 300.0f);
                ImGui::TextUnformatted("View:");
                ImGui::SameLine();
                if (ImGui::RadioButton("Audio Stream", is_audio)) {
                    cur_prof->set_canvas_state(m.canvas_pan_x, m.canvas_pan_y, "AUDIO");
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Control / Data", !is_audio)) {
                    cur_prof->set_canvas_state(m.canvas_pan_x, m.canvas_pan_y, "CONTROL");
                }
            }

            ImGui::EndMenuBar();
        }

        // Multi-Profile Tab Bar
        auto prof_ids = engine.profile_ids();
        if (prof_ids.empty()) {
            auto* p = engine.create_profile("Profile 1");
            if (p) active_profile_id = p->id();
            prof_ids = engine.profile_ids();
        }

        if (ImGui::BeginTabBar("ProfileTabs", ImGuiTabBarFlags_AutoSelectNewTabs)) {
            for (uint32_t pid : prof_ids) {
                auto* prof = engine.find_profile(pid);
                if (!prof) continue;

                std::string tab_label = prof->metadata().name + "##tab_" + std::to_string(pid);
                bool open = true;
                if (ImGui::BeginTabItem(tab_label.c_str(), &open)) {
                    active_profile_id = pid;
                    ImGui::EndTabItem();
                }
                if (!open) {
                    engine.close_profile(pid);
                }
            }

            if (ImGui::TabItemButton("+##add_tab")) {
                auto* p = engine.create_profile("New Profile");
                if (p) active_profile_id = p->id();
            }
            ImGui::EndTabBar();
        }

        // Main Visual Editing Canvas
        auto* active_prof = engine.find_profile(active_profile_id);
        if (active_prof) {
            canvas_renderer.render(*active_prof, engine);
        }

        // Status Bar at the bottom
        at::EngineStats stats = engine.stats();
        ImGui::SetCursorPos(ImVec2(10.0f, ImGui::GetWindowHeight() - 24.0f));
        ImGui::Text("Engine: %s | Sample Rate: %d Hz | Buffer: %d smp | DSP Load: %.1f%% | Open Devices: %zu",
                    stats.running ? "Running" : "Stopped",
                    engine.config().sample_rate,
                    engine.config().buffer_size_frames,
                    stats.dsp_load * 100.0f,
                    stats.open_devices);

        // Engine Settings Modal Dialog
        if (show_settings_modal) {
            ImGui::OpenPopup("Engine Settings");
        }
        if (ImGui::BeginPopupModal("Engine Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static int sel_sr = static_cast<int>(engine.config().sample_rate);
            static int sel_bs = static_cast<int>(engine.config().buffer_size_frames);
            static int sel_mc = static_cast<int>(engine.config().max_connections_per_port);

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
                show_settings_modal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                show_settings_modal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
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
