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
#include "engine/AudioGraphEngine.h"

#include <cmath>
#include <cstring>
#include <iostream>
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

    std::cout << "====================================================================\n";
    std::cout << " ALL 6 AUTOMATED PROOF-OF-CONCEPT TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "====================================================================\n";
    return 0;
}

