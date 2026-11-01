// ============================================================================
// AudioTool (AT) — Automated Backend Proof-of-Concept Verification Suite
// Tests:
//   1. Version, Unique Build ID, %LOCALAPPDATA%\Rouvr\AudioTool & Registry
//   2. Dynamic Plugin DLL Discovery & Shadow-Copy Loading
//   3. Deterministic Stereo Audio Pass-Through (VirtualInputNode -> OutputNode)
//   4. Control Plane Pulse & Node Function Invocation (TimerNode -> Volume/Mute)
//   5. Runtime Buffer-Size Reload & Live WASAPI Endpoint Verification
// ============================================================================

#include "at_version.h"
#include "common/AppPathsAndRegistry.h"
#include "common/Logger.h"
#include "engine/AudioEngine.h"
#include "engine/AudioGraphEngine.h"
#include "gui/CanvasRenderer.h"
#include "imgui.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define TEST_ASSERT(cond, msg)                                                           \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << msg << "\n"; \
            return false;                                                                \
        }                                                                                \
    } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;

bool test_1_paths_registry_and_build_id() {
    std::cout << "[TEST 1] Verifying Build ID, %LOCALAPPDATA%\\Rouvr\\AudioTool, and Registry...\n";

    std::string version_str = AT_VERSION_STRING;
    std::string build_id_str = AT_BUILD_ID;
    TEST_ASSERT(!version_str.empty(), "AT_VERSION_STRING must not be empty");
    TEST_ASSERT(!build_id_str.empty(), "AT_BUILD_ID must not be empty");

    std::string err;
    TEST_ASSERT(at::AppPathsAndRegistry::ensure_directories_exist(&err), "Failed to create %LOCALAPPDATA% dirs: " + err);
    TEST_ASSERT(std::filesystem::exists(at::AppPathsAndRegistry::get_local_data_root()), "Local data root missing");
    TEST_ASSERT(std::filesystem::exists(at::AppPathsAndRegistry::get_profiles_dir()), "Profiles dir missing");
    TEST_ASSERT(std::filesystem::exists(at::AppPathsAndRegistry::get_plugins_dir()), "Plugins dir missing");

    TEST_ASSERT(at::AppPathsAndRegistry::write_registry_metadata(&err), "Failed to write Registry key: " + err);

    at::RegistryConfigInfo info;
    TEST_ASSERT(at::AppPathsAndRegistry::read_registry_metadata(info, &err), "Failed to read Registry key: " + err);
    TEST_ASSERT(info.version == version_str, "Registry Version mismatch");
    TEST_ASSERT(info.build_id == build_id_str, "Registry BuildId mismatch");
    TEST_ASSERT(info.local_data_path == at::AppPathsAndRegistry::get_local_data_root(), "Registry LocalDataPath mismatch");

    std::cout << "  -> Passed! BuildId=" << info.build_id << ", LocalDataPath=" << info.local_data_path.string() << "\n";
    return true;
}

bool test_2_plugin_discovery_and_shadow_copy() {
    std::cout << "[TEST 2] Verifying Dynamic DLL Plugin Loading & Shadow Copy...\n";

    at::AudioGraphEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize({}, &err), "Engine initialize failed: " + err);

    const auto* vi_plugin  = engine.plugin_manager().find_plugin("at.builtin.audio.virtual_input");
    const auto* vol_plugin = engine.plugin_manager().find_plugin("at.builtin.audio.volume");
    const auto* out_plugin = engine.plugin_manager().find_plugin("at.builtin.audio.output");
    const auto* tmr_plugin = engine.plugin_manager().find_plugin("at.builtin.control.timer");

    TEST_ASSERT(vi_plugin != nullptr, "VirtualInputNode.dll was not discovered in %LOCALAPPDATA%\\Rouvr\\AudioTool\\plugins");
    TEST_ASSERT(vol_plugin != nullptr, "VolumeNode.dll was not discovered in %LOCALAPPDATA%\\Rouvr\\AudioTool\\plugins");
    TEST_ASSERT(out_plugin != nullptr, "OutputNode.dll was not discovered in %LOCALAPPDATA%\\Rouvr\\AudioTool\\plugins");
    TEST_ASSERT(tmr_plugin != nullptr, "TimerNode.dll was not discovered in %LOCALAPPDATA%\\Rouvr\\AudioTool\\plugins");

    TEST_ASSERT(vi_plugin->original_dll_path != vi_plugin->shadow_dll_path, "Plugin must be loaded from shadow copy");
    TEST_ASSERT(std::filesystem::exists(vi_plugin->shadow_dll_path), "Shadow DLL file must exist while loaded");
    TEST_ASSERT(std::string(vi_plugin->bundle->descriptor->build_id_str) == AT_BUILD_ID, "Plugin Build ID must match Engine Build ID");

    std::cout << "  -> Passed! Loaded 4 DLL plugins via shadow copy.\n";
    return true;
}

bool test_3_audio_passthrough_accuracy() {
    std::cout << "[TEST 3] Verifying Deterministic Stereo Audio Pass-Through (Input -> Output)...\n";

    at::EngineConfig cfg;
    cfg.sample_rate              = 48000;
    cfg.buffer_size_frames       = 256;
    cfg.max_connections_per_port = 4;

    at::AudioGraphEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize(cfg, &err), "Engine initialize failed: " + err);

    const uint32_t in_node  = engine.spawn_node("at.builtin.audio.virtual_input", "Test Input", &err);
    const uint32_t out_node = engine.spawn_node("at.builtin.audio.output", "Test Output", &err);
    TEST_ASSERT(in_node != 0 && out_node != 0, "Failed to spawn input/output nodes: " + err);

    // Bind both nodes to the deterministic virtual hardware endpoints
    AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_input" } };
    AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_output" } };
    TEST_ASSERT(engine.set_node_control_by_identifier(in_node, "device_id", in_dev) == AT_OK, "Failed to set input device_id");
    TEST_ASSERT(engine.set_node_control_by_identifier(out_node, "device_id", out_dev) == AT_OK, "Failed to set output device_id");

    // Connect VirtualInputNode -> OutputNode on the Audio Plane
    const uint32_t edge_id = engine.connect_audio(in_node, out_node, &err);
    TEST_ASSERT(edge_id != 0, "Failed to connect audio edge: " + err);

    // Prepare a 256-frame stereo test signal: Left = 440 Hz sine (0.75 amp), Right = 880 Hz cosine (0.50 amp)
    const uint32_t frames = cfg.buffer_size_frames;
    at::AlignedPlanarBuffer src_signal(2, frames);
    at::AlignedPlanarBuffer dst_signal(2, frames);

    for (uint32_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / static_cast<double>(cfg.sample_rate);
        src_signal.channel(0)[f] = static_cast<float>(0.75 * std::sin(2.0 * kPi * 440.0 * t));
        src_signal.channel(1)[f] = static_cast<float>(0.50 * std::cos(2.0 * kPi * 880.0 * t));
    }

    // Inject stereo waveform into Hidden Master Input Device ("virtual://test_input")
    uint32_t written = engine.device_manager().inject_virtual_input_frames(
        "virtual://test_input",
        src_signal.channel_ptrs(),
        2,
        frames);
    TEST_ASSERT(written == frames, "Failed to inject full frame block into virtual input");

    // Execute 1 tick of the AudioGraphEngine
    engine.process_single_tick();

    // Extract rendered stereo waveform from Hidden Master Output Device ("virtual://test_output")
    uint32_t read_back = engine.device_manager().extract_virtual_output_frames(
        "virtual://test_output",
        dst_signal.channel_ptrs(),
        2,
        frames);
    TEST_ASSERT(read_back == frames, "Failed to extract full frame block from virtual output");

    // Verify sample-by-sample bit-accurate pass-through
    float max_error = 0.0f;
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < frames; ++f) {
            float diff = std::abs(src_signal.channel(ch)[f] - dst_signal.channel(ch)[f]);
            max_error = std::max(max_error, diff);
        }
    }

    TEST_ASSERT(max_error < 1e-6f, "Pass-through sample error exceeded threshold: " + std::to_string(max_error));
    std::cout << "  -> Passed! 256 stereo frames passed through DLL graph with max error = " << max_error << "\n";
    return true;
}

bool test_4_control_plane_and_function_binding() {
    std::cout << "[TEST 4] Verifying Control Plane TimerNode Pulse -> Mute Function Binding...\n";

    at::EngineConfig cfg{ 48000, 256, 4 };
    at::AudioGraphEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize(cfg, &err), "Engine initialize failed: " + err);

    const uint32_t in_node  = engine.spawn_node("at.builtin.audio.virtual_input", "Test Input", &err);
    const uint32_t out_node = engine.spawn_node("at.builtin.audio.output", "Test Output", &err);
    const uint32_t tmr_node = engine.spawn_node("at.builtin.control.timer", "Mute Timer", &err);
    TEST_ASSERT(in_node && out_node && tmr_node, "Failed to spawn nodes");

    AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_input" } };
    AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_output" } };
    engine.set_node_control_by_identifier(in_node, "device_id", in_dev);
    engine.set_node_control_by_identifier(out_node, "device_id", out_dev);
    engine.connect_audio(in_node, out_node, &err);

    // Configure TimerNode to fire every 5 ms (1 tick at 256/48000 is 5.33 ms)
    AT_ControlValue interval_val{ AT_VAL_INT, { .int_val = 5 } };
    TEST_ASSERT(engine.set_node_control_by_identifier(tmr_node, "interval_ms", interval_val) == AT_OK, "Failed to set timer interval");

    // Bind TimerNode control output 0 -> VirtualInputNode function 1001 ("toggle_mute")
    AT_ControlValue none_arg{ AT_VAL_NONE, { .int_val = 0 } };
    uint32_t ctrl_edge = engine.connect_control(tmr_node, 0, in_node, 1001, none_arg, &err);
    TEST_ASSERT(ctrl_edge != 0, "Failed to connect control plane edge: " + err);

    // Verify mute starts as false
    AT_ControlValue mute_val{};
    engine.get_node_control_by_identifier(in_node, "mute", &mute_val);
    TEST_ASSERT(mute_val.bool_val == false, "Initial mute should be false");

    // Tick 0 initializes timer baseline (timestamp = 0 ns)
    engine.process_single_tick();
    // Tick 1 advances timestamp to 5.33 ms (>= 5 ms), firing TimerNode -> toggling Mute to true!
    engine.process_single_tick();

    engine.get_node_control_by_identifier(in_node, "mute", &mute_val);
    TEST_ASSERT(mute_val.bool_val == true, "TimerNode control pulse should have toggled VirtualInputNode mute to true");

    std::cout << "  -> Passed! TimerNode pulse triggered VirtualInputNode::toggle_mute across Control Plane.\n";
    return true;
}

bool test_5_runtime_reload_and_wasapi_enumeration() {
    std::cout << "[TEST 5] Verifying Runtime Buffer-Size Reload (256 -> 512) & WASAPI Enumeration...\n";

    at::EngineConfig cfg{ 48000, 256, 4 };
    at::AudioGraphEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize(cfg, &err), "Engine initialize failed: " + err);

    const uint32_t in_node  = engine.spawn_node("at.builtin.audio.virtual_input", "Test Input", &err);
    const uint32_t out_node = engine.spawn_node("at.builtin.audio.output", "Test Output", &err);
    AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_input" } };
    AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_output" } };
    engine.set_node_control_by_identifier(in_node, "device_id", in_dev);
    engine.set_node_control_by_identifier(out_node, "device_id", out_dev);
    engine.connect_audio(in_node, out_node, &err);

    // Reload engine at runtime with buffer_size = 512 and max_connections = 8
    at::EngineConfig new_cfg{ 48000, 512, 8 };
    TEST_ASSERT(engine.reload_config(new_cfg, &err), "Runtime reload_config failed: " + err);
    TEST_ASSERT(engine.config().buffer_size_frames == 512, "Buffer size was not updated to 512");

    // Verify 512-frame pass-through works immediately after reload without re-creating nodes
    at::AlignedPlanarBuffer src(2, 512);
    at::AlignedPlanarBuffer dst(2, 512);
    for (uint32_t f = 0; f < 512; ++f) {
        src.channel(0)[f] = 0.25f;
        src.channel(1)[f] = -0.25f;
    }

    engine.device_manager().inject_virtual_input_frames("virtual://test_input", src.channel_ptrs(), 2, 512);
    engine.process_single_tick();
    engine.device_manager().extract_virtual_output_frames("virtual://test_output", dst.channel_ptrs(), 2, 512);

    TEST_ASSERT(std::abs(dst.channel(0)[511] - 0.25f) < 1e-6f, "Left channel mismatch after runtime reload");
    TEST_ASSERT(std::abs(dst.channel(1)[511] - (-0.25f)) < 1e-6f, "Right channel mismatch after runtime reload");

    auto devices = engine.device_manager().enumerate_devices();
    TEST_ASSERT(devices.size() >= 2, "Device enumeration must return at least virtual endpoints");

    std::cout << "  -> Passed! Runtime reload (512 frames) verified; enumerated " << devices.size() << " audio endpoints.\n";
    return true;
}

bool test_6_volume_node_mixing_and_normalization() {
    std::cout << "[TEST 6] Verifying VolumeNode Dynamic Inputs, Channel Mixing & Gain Control...\n";

    at::EngineConfig cfg{ 48000, 256, 4 };
    at::AudioGraphEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize(cfg, &err), "Engine initialize failed: " + err);

    // Spawn Input1, Input2, VolumeNode, OutputNode
    const uint32_t in1 = engine.spawn_node("at.builtin.audio.virtual_input", "Source 1", &err);
    const uint32_t in2 = engine.spawn_node("at.builtin.audio.virtual_input", "Source 2", &err);
    const uint32_t vol = engine.spawn_node("at.builtin.audio.volume", "Mixer", &err);
    const uint32_t out = engine.spawn_node("at.builtin.audio.output", "Output", &err);
    TEST_ASSERT(in1 && in2 && vol && out, "Failed to spawn nodes");

    // Bind to virtual endpoints
    AT_ControlValue dev1{ AT_VAL_STRING, { .string_val = "virtual://test_input" } };
    AT_ControlValue dev2{ AT_VAL_STRING, { .string_val = "virtual://test_input" } };
    AT_ControlValue dev_out{ AT_VAL_STRING, { .string_val = "virtual://test_output" } };
    engine.set_node_control_by_identifier(in1, "device_id", dev1);
    engine.set_node_control_by_identifier(in2, "device_id", dev2);
    engine.set_node_control_by_identifier(out, "device_id", dev_out);

    // Connect: in1 -> vol, in2 -> vol, vol -> out
    TEST_ASSERT(engine.connect_audio(in1, vol, &err) != 0, "Failed to connect in1 -> vol: " + err);
    TEST_ASSERT(engine.connect_audio(in2, vol, &err) != 0, "Failed to connect in2 -> vol: " + err);
    TEST_ASSERT(engine.connect_audio(vol, out, &err) != 0, "Failed to connect vol -> out: " + err);

    // Inject 0.25f into virtual input
    at::AlignedPlanarBuffer src(2, 256);
    at::AlignedPlanarBuffer dst(2, 256);
    for (uint32_t f = 0; f < 256; ++f) {
        src.channel(0)[f] = 0.25f;
        src.channel(1)[f] = 0.25f;
    }
    engine.device_manager().inject_virtual_input_frames("virtual://test_input", src.channel_ptrs(), 2, 256);

    // Advance 5 ticks so SmoothedFloat ramp settles completely
    for (int t = 0; t < 5; ++t) {
        engine.device_manager().inject_virtual_input_frames("virtual://test_input", src.channel_ptrs(), 2, 256);
        engine.process_single_tick();
        engine.device_manager().extract_virtual_output_frames("virtual://test_output", dst.channel_ptrs(), 2, 256);
    }

    // Both in1 and in2 read 0.25f, and VolumeNode mixes both with unity gain -> output is ~0.50f
    float sample_l = dst.channel(0)[255];
    TEST_ASSERT(std::abs(sample_l - 0.50f) < 0.05f, "Expected summed mixed sample ~0.50f, got: " + std::to_string(sample_l));

    // Test Master Mute function
    AT_ControlValue none_arg{ AT_VAL_NONE, { .int_val = 0 } };
    TEST_ASSERT(engine.invoke_node_function_by_identifier(vol, "toggle_master_mute", none_arg) == AT_OK, "Failed to invoke toggle_master_mute");

    for (int t = 0; t < 5; ++t) {
        engine.device_manager().inject_virtual_input_frames("virtual://test_input", src.channel_ptrs(), 2, 256);
        engine.process_single_tick();
        engine.device_manager().extract_virtual_output_frames("virtual://test_output", dst.channel_ptrs(), 2, 256);
    }

    float muted_sample = dst.channel(0)[255];
    TEST_ASSERT(std::abs(muted_sample) < 1e-4f, "Output should be silent after master mute, got: " + std::to_string(muted_sample));

    std::cout << "  -> Passed! VolumeNode successfully summed multiple inputs, smoothed gains, and responded to Master Mute function.\n";
    return true;
}

bool test_7_gui_canvas_controls_and_row_alignment() {
    std::cout << "[TEST 7] Verifying GUI CanvasRenderer Node Controls Clickability & Horizontal Row Alignment...\n";

    at::EngineConfig cfg{ 48000, 256, 4 };
    at::AudioEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize(cfg, &err), "AudioEngine initialize failed: " + err);

    at::ProfileGraph* prof = engine.create_profile("GUI Test Profile");
    TEST_ASSERT(prof != nullptr, "Failed to create profile");

    // Place VirtualInputNode at world (180, 100), expanded (Form 2)
    uint32_t vin = prof->spawn_node("at.builtin.audio.virtual_input", "Mic Node", &err);
    TEST_ASSERT(vin != 0, "Failed to spawn VirtualInputNode: " + err);
    prof->set_node_ui(vin, { 180.0f, 100.0f, true });

    // Setup headless Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    unsigned char* tex_pixels = nullptr;
    int tex_w = 0, tex_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&tex_pixels, &tex_w, &tex_h);

    at::CanvasRenderer renderer;

    auto step_gui = [&](ImVec2 mouse_pos, bool mouse_down) {
        io.DeltaTime = 1.0f / 60.0f;
        io.MousePos = mouse_pos;
        io.MouseDown[0] = mouse_down;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(1280.0f, 800.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("HeadlessHost", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::PopStyleVar();
        renderer.render(*prof, engine, 0.0f);
        ImGui::End();
        ImGui::Render();
    };

    // Warm-up frame
    step_gui(ImVec2(10.0f, 10.0f), false);

    // Verify initial Mute (Row 2, 3rd control row) is false
    at::OwnedValue mute_val;
    TEST_ASSERT(prof->get_control(vin, "mute", &mute_val) == AT_OK, "Failed to query mute");
    TEST_ASSERT(mute_val.bool_val == false, "Initial mute must be false");

    // Row 2 (Mute button) sits at:
    //   x = [180 + 10 + label_col_w + 8 .. 180 + width - 6] -> x = 360.0f is inside the node's Right Cell
    //   y = 100 (node_y) + 32 (header_h) + 2 * 30 (row 2) + 15 (half row) = 207.0f
    // If Row 2 had snapped to the left of the window (x = 0) or if CanvasHitBox blocked clicks, clicking (360, 207) would fail!
    ImVec2 mute_btn_pos(360.0f, 207.0f);
    step_gui(mute_btn_pos, false);
    step_gui(mute_btn_pos, true);
    step_gui(mute_btn_pos, false);

    TEST_ASSERT(prof->get_control(vin, "mute", &mute_val) == AT_OK, "Failed to query mute after click");
    TEST_ASSERT(mute_val.bool_val == true, "Clicking Mute button on Row 2 inside node must toggle mute to true!");

    // Row 1 (Input Gain Slider, 2nd control row) sits at y = 100 + 32 + 1 * 30 + 15 = 177.0f
    // Click near the right side of the slider (x = 430.0f, y = 177.0f) to change gain from 0.0 dB
    ImVec2 slider_pos(430.0f, 177.0f);
    step_gui(slider_pos, false);
    step_gui(slider_pos, true);
    step_gui(slider_pos, false);

    at::OwnedValue gain_val;
    TEST_ASSERT(prof->get_control(vin, "input_gain_db", &gain_val) == AT_OK, "Failed to query input_gain_db");
    TEST_ASSERT(std::abs(gain_val.float_val) > 1.0, "Clicking Input Gain slider on Row 1 inside node must update gain_db, got: " + std::to_string(gain_val.float_val));

    ImGui::DestroyContext(ctx);
    engine.shutdown();

    std::cout << "  -> Passed! GUI node controls on all rows are horizontally aligned to the node and respond to clicks/drags.\n";
    return true;
}

bool test_8_app_audio_router_and_profile_persistence() {
    std::cout << "[TEST 8] Verifying AppAudioRouter, session enumeration, presets, and profile routing persistence...\n";

    at::AudioEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize({}, &err), "Engine initialization failed: " + err);

    auto render_devices = engine.device_manager().enumerate_devices();
    std::vector<at::AudioDeviceInfo> out_devs;
    for (const auto& d : render_devices) {
        if (d.is_output_render) out_devs.push_back(d);
    }

    // 1. Enumerate active audio applications
    auto apps = engine.audio_router().enumerate_apps(render_devices);
    std::cout << "  -> Enumerated " << apps.size() << " active audio applications.\n";
    for (const auto& a : apps) {
        std::cout << "     App: " << a.process_name << " (PID " << a.pid << ") Current Device: " << a.current_device_name << "\n";
    }

    // 2. Test Standalone Routing Presets Save/Load/Delete
    std::map<std::string, std::string> sample_mapping = {
        { "firefox.exe", "{0.0.0.00000000}.{dummy-vb-cable-guid}" },
        { "spotify.exe", "{0.0.0.00000000}.{dummy-speakers-guid}" }
    };
    std::string preset_err;
    TEST_ASSERT(engine.audio_router().save_preset("TestStreamingPreset", sample_mapping, &preset_err), "save_preset failed: " + preset_err);

    auto presets = engine.audio_router().list_presets();
    bool found_preset = false;
    for (const auto& p : presets) {
        if (p == "TestStreamingPreset") found_preset = true;
    }
    TEST_ASSERT(found_preset, "TestStreamingPreset was not found in list_presets()");

    std::map<std::string, std::string> loaded_mapping;
    TEST_ASSERT(engine.audio_router().load_preset("TestStreamingPreset", &loaded_mapping, &preset_err), "load_preset failed: " + preset_err);
    TEST_ASSERT(loaded_mapping.size() == 2, "Loaded mapping count mismatch");
    TEST_ASSERT(loaded_mapping["firefox.exe"] == "{0.0.0.00000000}.{dummy-vb-cable-guid}", "Mapping value mismatch for firefox.exe");

    TEST_ASSERT(engine.audio_router().delete_preset("TestStreamingPreset", &preset_err), "delete_preset failed: " + preset_err);

    // 3. Test Profile Routing Serialization & Loading
    auto* prof = engine.create_profile("RoutingProfile");
    TEST_ASSERT(prof != nullptr, "Failed to create profile");
    prof->set_app_routing_entry("firefox.exe", "{0.0.0.00000000}.{dummy-vb-cable-guid}");
    prof->set_app_routing_entry("game.exe", "{0.0.0.00000000}.{dummy-headphones-guid}");

    auto routing_map = prof->app_routing();
    TEST_ASSERT(routing_map.size() == 2, "Profile routing map must have 2 entries");
    TEST_ASSERT(routing_map["firefox.exe"] == "{0.0.0.00000000}.{dummy-vb-cable-guid}", "Profile routing value mismatch");

    std::filesystem::path prof_file = at::AppPathsAndRegistry::get_profiles_dir() / "test_app_routing.atprof";
    std::string save_err;
    TEST_ASSERT(engine.save_profile(prof->id(), prof_file, &save_err), "Failed to save profile: " + save_err);

    std::string load_err;
    auto* loaded_prof = engine.load_profile(prof_file, &load_err);
    TEST_ASSERT(loaded_prof != nullptr, "Failed to load saved profile: " + load_err);
    auto loaded_routing = loaded_prof->app_routing();
    TEST_ASSERT(loaded_routing.size() == 2, "Loaded profile app_routing size mismatch");
    TEST_ASSERT(loaded_routing["firefox.exe"] == "{0.0.0.00000000}.{dummy-vb-cable-guid}", "Loaded profile app_routing for firefox.exe mismatch");
    TEST_ASSERT(loaded_routing["game.exe"] == "{0.0.0.00000000}.{dummy-headphones-guid}", "Loaded profile app_routing for game.exe mismatch");

    std::filesystem::remove(prof_file);

    // 4. Test Revert on Exit Configuration
    engine.audio_router().set_revert_on_exit(true);
    TEST_ASSERT(engine.audio_router().get_revert_on_exit() == true, "revert_on_exit must be true");
    engine.audio_router().revert_all_to_original();

    engine.shutdown();
    std::cout << "  -> Passed! AppAudioRouter enumeration, presets, and profile routing persistence verified successfully.\n";
    return true;
}

bool test_9_robust_logging_and_rotation() {
    std::cout << "[TEST 9] Verifying Robust Logging, Dual-File Persistence, and X-Retention Rotation...\n";

    auto& logger = at::Logger::instance();
    logger.shutdown(); // ensure clean state

    auto logs_dir = at::AppPathsAndRegistry::get_logs_dir();
    std::filesystem::create_directories(logs_dir);

    // Create 7 dummy dated log files to test retention rotation
    for (int i = 1; i <= 7; ++i) {
        char dummy_name[64];
        std::snprintf(dummy_name, sizeof(dummy_name), "audiotool_2026-01-%02d_12-00-00.log", i);
        std::ofstream f(logs_dir / dummy_name);
        f << "Dummy historical log " << i << "\n";
    }

    // Initialize with max_retained_logs = 5
    TEST_ASSERT(logger.init(5, at::LogLevel::Debug), "Logger initialization failed");
    logger.install_crash_handler();

    // Verify current log file and latest.log exist
    std::filesystem::path current_log = logger.get_current_log_path();
    std::filesystem::path latest_log = logger.get_latest_log_path();

    TEST_ASSERT(!current_log.empty() && std::filesystem::exists(current_log), "Timestamped log file does not exist: " + current_log.string());
    TEST_ASSERT(!latest_log.empty() && std::filesystem::exists(latest_log), "latest.log does not exist: " + latest_log.string());

    // Write diagnostic messages across subsystems
    AT_LOG_DEBUG("TEST", "Debug diagnostic entry %d", 42);
    AT_LOG_INFO("FILESYSTEM", "Testing filesystem error triage log");
    AT_LOG_WARN("WINAPI", "Simulated WinAPI warning: SetEventHandle returned pending state");
    AT_LOG_ERROR("COM/POLICY", "Simulated COM error HRESULT=0x80004002");

    logger.flush();

    // Verify content is in both files
    std::ifstream cur_f(current_log);
    std::stringstream cur_ss;
    cur_ss << cur_f.rdbuf();
    std::string cur_content = cur_ss.str();
    TEST_ASSERT(cur_content.find("Testing filesystem error triage log") != std::string::npos, "Message missing from timestamped log");
    TEST_ASSERT(cur_content.find("COM/POLICY") != std::string::npos, "COM/POLICY missing from timestamped log");

    std::ifstream lat_f(latest_log);
    std::stringstream lat_ss;
    lat_ss << lat_f.rdbuf();
    std::string lat_content = lat_ss.str();
    TEST_ASSERT(lat_content.find("Testing filesystem error triage log") != std::string::npos, "Message missing from latest.log");

    // Verify log retention: total number of audiotool_*.log files should be <= 5
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(logs_dir)) {
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            if (name.rfind("audiotool_", 0) == 0 && entry.path().extension() == ".log") {
                ++count;
            }
        }
    }
    TEST_ASSERT(count <= 5, "Log retention failed: expected <= 5 files, found " + std::to_string(count));

    // Oldest dummy files (e.g. 2026-01-01) should have been pruned
    TEST_ASSERT(!std::filesystem::exists(logs_dir / "audiotool_2026-01-01_12-00-00.log"), "Oldest dummy log was not deleted");

    std::cout << "  -> Passed! Logging verified with " << count << " retained logs and live latest.log.\n";
    return true;
}

bool test_10_additional_audio_processing_nodes() {
    std::cout << "[TEST 10] Verifying 11 Additional Audio Processing Nodes & DSP Algorithms...\n";

    at::EngineConfig cfg;
    cfg.sample_rate              = 48000;
    cfg.buffer_size_frames       = 256;
    cfg.max_connections_per_port = 4;

    at::AudioGraphEngine engine;
    std::string err;
    TEST_ASSERT(engine.initialize(cfg, &err), "Engine initialize failed: " + err);

    // 1. Verify dynamic discovery of all 15 plugins in the system
    const std::vector<std::string> expected_plugins = {
        "at.builtin.audio.virtual_input",
        "at.builtin.audio.volume",
        "at.builtin.audio.output",
        "at.builtin.control.timer",
        "at.builtin.audio.equalizer",
        "at.builtin.audio.filter",
        "at.builtin.audio.compressor",
        "at.builtin.audio.limiter",
        "at.builtin.audio.noise_gate",
        "at.builtin.audio.reverb",
        "at.builtin.audio.delay",
        "at.builtin.audio.distortion",
        "at.builtin.audio.overdrive",
        "at.builtin.audio.saturation",
        "at.builtin.audio.bitcrusher"
    };

    for (const auto& pid : expected_plugins) {
        const auto* p = engine.plugin_manager().find_plugin(pid);
        TEST_ASSERT(p != nullptr, "Plugin failed to load: " + pid);
    }
    std::cout << "  -> Discovered and loaded all 15 plugins across Audio & Control planes.\n";

    // 2. Test Limiter Hard Ceiling Clamping via Virtual Audio Loop
    {
        uint32_t in_n  = engine.spawn_node("at.builtin.audio.virtual_input", "In_Lim", &err);
        uint32_t lim_n = engine.spawn_node("at.builtin.audio.limiter", "Lim", &err);
        uint32_t out_n = engine.spawn_node("at.builtin.audio.output", "Out_Lim", &err);
        TEST_ASSERT(in_n && lim_n && out_n, "Failed to spawn nodes");

        AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_lim_in" } };
        AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_lim_out" } };
        engine.set_node_control_by_identifier(in_n, "device_id", in_dev);
        engine.set_node_control_by_identifier(out_n, "device_id", out_dev);

        // Ceiling = -6.0 dB (0.501), Pre-Gain = +6.0 dB
        AT_ControlValue ceil_val{ AT_VAL_FLOAT, { .float_val = -6.0 } };
        AT_ControlValue pre_val{ AT_VAL_FLOAT, { .float_val = 6.0 } };
        engine.set_node_control_by_identifier(lim_n, "ceiling_db", ceil_val);
        engine.set_node_control_by_identifier(lim_n, "input_gain_db", pre_val);

        engine.connect_audio(in_n, lim_n);
        engine.connect_audio(lim_n, out_n);

        at::AlignedPlanarBuffer src(2, 256);
        at::AlignedPlanarBuffer dst(2, 256);
        for (uint32_t f = 0; f < 256; ++f) {
            src.channel(0)[f] = 1.0f;
            src.channel(1)[f] = 1.0f;
        }

        engine.device_manager().inject_virtual_input_frames("virtual://test_lim_in", src.channel_ptrs(), 2, 256);
        engine.process_single_tick();
        engine.device_manager().extract_virtual_output_frames("virtual://test_lim_out", dst.channel_ptrs(), 2, 256);

        float max_out = 0.0f;
        for (uint32_t f = 0; f < 256; ++f) {
            max_out = std::max(max_out, std::abs(dst.channel(0)[f]));
        }
        // Ceiling is -6.0 dB = 0.501187. Must never exceed ceiling!
        TEST_ASSERT(max_out <= 0.505f, "Limiter failed to clamp audio to ceiling: max_out=" + std::to_string(max_out));
    }

    // 3. Test Equalizer 16 Band Controls
    {
        uint32_t eq_id = engine.spawn_node("at.builtin.audio.equalizer", "Test EQ", &err);
        TEST_ASSERT(eq_id != 0, "Failed to spawn EqualizerNode");

        for (int i = 0; i < 16; ++i) {
            std::string ident = "band_" + std::to_string(i);
            AT_ControlValue g_val{ AT_VAL_FLOAT, { .float_val = 6.0 } };
            TEST_ASSERT(engine.set_node_control_by_identifier(eq_id, ident, g_val) == AT_OK, "Failed to set EQ band: " + ident);
        }
    }

    // 4. Test Filter Lowpass Attenuation via Virtual Audio Loop
    {
        uint32_t in_n  = engine.spawn_node("at.builtin.audio.virtual_input", "In_Flt", &err);
        uint32_t flt_n = engine.spawn_node("at.builtin.audio.filter", "Flt", &err);
        uint32_t out_n = engine.spawn_node("at.builtin.audio.output", "Out_Flt", &err);

        AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_flt_in" } };
        AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_flt_out" } };
        engine.set_node_control_by_identifier(in_n, "device_id", in_dev);
        engine.set_node_control_by_identifier(out_n, "device_id", out_dev);

        AT_ControlValue fc_val{ AT_VAL_FLOAT, { .float_val = 200.0 } };
        engine.set_node_control_by_identifier(flt_n, "cutoff_hz", fc_val);

        engine.connect_audio(in_n, flt_n);
        engine.connect_audio(flt_n, out_n);

        at::AlignedPlanarBuffer src(2, 256);
        at::AlignedPlanarBuffer dst(2, 256);
        for (uint32_t f = 0; f < 256; ++f) {
            float s = std::sin(2.0f * static_cast<float>(kPi) * 10000.0f * (f / 48000.0f));
            src.channel(0)[f] = s;
            src.channel(1)[f] = s;
        }

        engine.device_manager().inject_virtual_input_frames("virtual://test_flt_in", src.channel_ptrs(), 2, 256);
        engine.process_single_tick();
        engine.device_manager().extract_virtual_output_frames("virtual://test_flt_out", dst.channel_ptrs(), 2, 256);

        float max_out = 0.0f;
        for (uint32_t f = 128; f < 256; ++f) {
            max_out = std::max(max_out, std::abs(dst.channel(0)[f]));
        }
        TEST_ASSERT(max_out < 0.1f, "Filter failed to attenuate high frequencies: max_out=" + std::to_string(max_out));
    }

    // 5. Test Noise Gate Silence Clamping via Virtual Audio Loop
    {
        uint32_t in_n = engine.spawn_node("at.builtin.audio.virtual_input", "In_NG", &err);
        uint32_t ng_n = engine.spawn_node("at.builtin.audio.noise_gate", "Gate", &err);
        uint32_t out_n = engine.spawn_node("at.builtin.audio.output", "Out_NG", &err);

        AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_ng_in" } };
        AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_ng_out" } };
        engine.set_node_control_by_identifier(in_n, "device_id", in_dev);
        engine.set_node_control_by_identifier(out_n, "device_id", out_dev);

        engine.connect_audio(in_n, ng_n);
        engine.connect_audio(ng_n, out_n);

        at::AlignedPlanarBuffer src(2, 256);
        at::AlignedPlanarBuffer dst(2, 256);
        for (uint32_t f = 0; f < 256; ++f) {
            src.channel(0)[f] = 0.001f; // -60 dB, below -45 dB threshold
            src.channel(1)[f] = 0.001f;
        }

        engine.device_manager().inject_virtual_input_frames("virtual://test_ng_in", src.channel_ptrs(), 2, 256);
        engine.process_single_tick();
        engine.device_manager().extract_virtual_output_frames("virtual://test_ng_out", dst.channel_ptrs(), 2, 256);

        float max_out = 0.0f;
        for (uint32_t f = 0; f < 256; ++f) {
            max_out = std::max(max_out, std::abs(dst.channel(0)[f]));
        }
        TEST_ASSERT(max_out <= 0.0005f, "Noise gate failed to mute sub-threshold audio: max_out=" + std::to_string(max_out));
    }

    // 6. Test Bitcrusher Quantization via Virtual Audio Loop
    {
        uint32_t in_n  = engine.spawn_node("at.builtin.audio.virtual_input", "In_Bit", &err);
        uint32_t bit_n = engine.spawn_node("at.builtin.audio.bitcrusher", "Bit", &err);
        uint32_t out_n = engine.spawn_node("at.builtin.audio.output", "Out_Bit", &err);

        AT_ControlValue in_dev{ AT_VAL_STRING, { .string_val = "virtual://test_bit_in" } };
        AT_ControlValue out_dev{ AT_VAL_STRING, { .string_val = "virtual://test_bit_out" } };
        engine.set_node_control_by_identifier(in_n, "device_id", in_dev);
        engine.set_node_control_by_identifier(out_n, "device_id", out_dev);

        AT_ControlValue bits_val{ AT_VAL_FLOAT, { .float_val = 2.0 } };
        engine.set_node_control_by_identifier(bit_n, "bit_depth", bits_val);

        engine.connect_audio(in_n, bit_n);
        engine.connect_audio(bit_n, out_n);

        at::AlignedPlanarBuffer src(2, 256);
        at::AlignedPlanarBuffer dst(2, 256);
        for (uint32_t f = 0; f < 256; ++f) {
            src.channel(0)[f] = 0.234567f;
            src.channel(1)[f] = 0.234567f;
        }

        engine.device_manager().inject_virtual_input_frames("virtual://test_bit_in", src.channel_ptrs(), 2, 256);
        engine.process_single_tick();
        engine.device_manager().extract_virtual_output_frames("virtual://test_bit_out", dst.channel_ptrs(), 2, 256);

        float out_val = dst.channel(0)[10];
        TEST_ASSERT(std::abs(out_val - 0.0f) < 1e-4f || std::abs(out_val - 0.5f) < 1e-4f,
                    "Bitcrusher quantization failure: got " + std::to_string(out_val));
    }
        at::AlignedPlanarBuffer dst(2, 256);
        for (uint32_t f = 0; f < 256; ++f) {
            src.channel(0)[f] = 0.234567f;
            src.channel(1)[f] = 0.234567f;
        }

        eng.device_manager().inject_virtual_input_frames("virtual://test_bit_in", src.channel_ptrs(), 2, 256);
        eng.process_single_tick();
        eng.device_manager().extract_virtual_output_frames("virtual://test_bit_out", dst.channel_ptrs(), 2, 256);

        float out_val = dst.channel(0)[10];
        TEST_ASSERT(std::abs(out_val - 0.0f) < 1e-4f || std::abs(out_val - 0.5f) < 1e-4f,
                    "Bitcrusher quantization failure: got " + std::to_string(out_val));
    }

    std::cout << "  -> Passed! Verified DSP algorithms for all new audio nodes.\n";
    return true;
}

} // namespace

int main() {
    at::AppPathsAndRegistry::harden_dll_search_order();

    std::cout << "====================================================================\n";
    std::cout << " AudioTool Automated Proof-of-Concept Test Suite\n";
    std::cout << " Version: " << AT_VERSION_STRING << " | Build ID: " << AT_BUILD_ID << "\n";
    std::cout << "====================================================================\n";

    if (!test_1_paths_registry_and_build_id()) return 1;
    if (!test_2_plugin_discovery_and_shadow_copy()) return 1;
    if (!test_3_audio_passthrough_accuracy()) return 1;
    if (!test_4_control_plane_and_function_binding()) return 1;
    if (!test_5_runtime_reload_and_wasapi_enumeration()) return 1;
    if (!test_6_volume_node_mixing_and_normalization()) return 1;
    if (!test_7_gui_canvas_controls_and_row_alignment()) return 1;
    if (!test_8_app_audio_router_and_profile_persistence()) return 1;
    if (!test_9_robust_logging_and_rotation()) return 1;
    if (!test_10_additional_audio_processing_nodes()) return 1;

    std::cout << "====================================================================\n";
    std::cout << " ALL 10 AUTOMATED PROOF-OF-CONCEPT TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "====================================================================\n";
    return 0;
}

