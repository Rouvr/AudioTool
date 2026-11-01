// ============================================================================
// AudioTool (AT) — Proof-of-Concept Headless Audio Backend Runner
// Passes audio from Input Node (VirtualInputNode.dll) -> Output Node (OutputNode.dll)
// ============================================================================

#include "at_version.h"
#include "common/AppPathsAndRegistry.h"
#include "engine/AudioGraphEngine.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    // 1. Harden DLL search order immediately on process startup
    at::AppPathsAndRegistry::harden_dll_search_order();

    std::cout << "====================================================================\n";
    std::cout << " Rouvr AudioTool (AT) - Headless Audio Engine Proof of Concept\n";
    std::cout << " Version  : " << AT_VERSION_STRING << "\n";
    std::cout << " Build ID : " << AT_BUILD_ID << "\n";
    std::cout << " Git Hash : " << AT_GIT_COMMIT_HASH << " (" << AT_BUILD_TIMESTAMP_UTC << ")\n";
    std::cout << "====================================================================\n";

    std::string input_device  = "default_input";
    std::string output_device = "default_output";
    int duration_seconds      = 3;
    bool list_only            = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list-devices") {
            list_only = true;
        } else if (arg == "--input" && i + 1 < argc) {
            input_device = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_device = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_seconds = std::stoi(argv[++i]);
        }
    }

    at::AudioGraphEngine engine;
    std::string err;
    if (!engine.initialize({}, &err)) {
        std::cerr << "[ERROR] Engine initialization failed: " << err << "\n";
        return 1;
    }

    at::RegistryConfigInfo reg_info;
    if (at::AppPathsAndRegistry::read_registry_metadata(reg_info)) {
        std::cout << "[Registry HKCU\\Software\\Rouvr\\AudioTool]\n";
        std::cout << "  InstallPath   : " << reg_info.install_path.string() << "\n";
        std::cout << "  LocalDataPath : " << reg_info.local_data_path.string() << "\n";
        std::cout << "  Version       : " << reg_info.version << "\n";
        std::cout << "  BuildId       : " << reg_info.build_id << "\n\n";
    }

    std::cout << "[Loaded Node Plugins from " << at::AppPathsAndRegistry::get_plugins_dir().string() << "]\n";
    auto plugins = engine.plugin_manager().list_plugins();
    for (const auto* p : plugins) {
        std::cout << "  * " << p->plugin_id << " (" << p->display_name
                  << ") [Folder: " << p->relative_folder_path
                  << ", Build: " << p->bundle->descriptor->build_id_str << "]\n";
    }
    std::cout << "\n";

    std::cout << "[Available Audio Endpoints (WASAPI + Virtual)]\n";
    auto devices = engine.device_manager().enumerate_devices();
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        std::cout << "  [" << i << "] " << (d.is_input_capture ? "[IN ] " : "[OUT] ")
                  << (d.is_default ? "(DEFAULT) " : "")
                  << d.friendly_name << " (" << d.native_sample_rate << " Hz, " << d.native_channels << " ch)\n"
                  << "      ID: " << d.endpoint_id << "\n";
    }
    std::cout << "\n";

    if (list_only) {
        return 0;
    }

    // Spawn VirtualInputNode -> OutputNode
    uint32_t in_node = engine.spawn_node("at.builtin.audio.virtual_input", "Main Input", &err);
    if (!in_node) {
        std::cerr << "[ERROR] Failed to spawn VirtualInputNode: " << err << "\n";
        return 1;
    }

    uint32_t out_node = engine.spawn_node("at.builtin.audio.output", "Main Output", &err);
    if (!out_node) {
        std::cerr << "[ERROR] Failed to spawn OutputNode: " << err << "\n";
        return 1;
    }

    AT_ControlValue in_dev_val{};
    in_dev_val.type       = AT_VAL_STRING;
    in_dev_val.string_val = input_device.c_str();
    engine.set_node_control_by_identifier(in_node, "device_id", in_dev_val);

    AT_ControlValue out_dev_val{};
    out_dev_val.type       = AT_VAL_STRING;
    out_dev_val.string_val = output_device.c_str();
    engine.set_node_control_by_identifier(out_node, "device_id", out_dev_val);

    uint32_t edge_id = engine.connect_audio(in_node, out_node, &err);
    if (!edge_id) {
        std::cerr << "[ERROR] Failed to connect VirtualInputNode -> OutputNode: " << err << "\n";
        return 1;
    }

    std::cout << "[Audio Graph Running] VirtualInputNode (#" << in_node << ", '" << input_device
              << "') ---> OutputNode (#" << out_node << ", '" << output_device << "') for "
              << duration_seconds << "s...\n";

    engine.start_realtime_loop();

    for (int s = 0; s < duration_seconds * 4; ++s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        AT_ControlValue in_meter{}, out_meter{};
        engine.get_node_control_by_identifier(in_node, "peak_meter_db", &in_meter);
        engine.get_node_control_by_identifier(out_node, "peak_meter_db", &out_meter);

        std::cout << "\r  Frames: " << std::setw(8) << engine.total_frames_processed()
                  << " | In Peak: " << std::fixed << std::setprecision(1) << std::setw(6) << in_meter.float_val << " dB"
                  << " | Out Peak: " << std::setw(6) << out_meter.float_val << " dB   " << std::flush;
    }
    std::cout << "\n[Done] Stopping Audio Graph Engine cleanly.\n";

    engine.stop_realtime_loop();
    engine.shutdown();
    return 0;
}
