#pragma once
// ============================================================================
// AudioTool (AT) — Audio & Control Graph Engine
// ============================================================================

#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "engine/DeviceManager.h"
#include "engine/PluginManager.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace at {

struct EngineConfig {
    uint32_t sample_rate              = 48000;
    uint32_t buffer_size_frames       = 256;
    uint32_t max_connections_per_port = 4;
};

struct AudioEdge {
    uint32_t edge_id     = 0;
    uint32_t src_node_id = 0;
    uint32_t dst_node_id = 0;
};

struct ControlEdge {
    uint32_t        edge_id            = 0;
    uint32_t        src_node_id        = 0;
    uint32_t        src_output_port    = 0;
    uint32_t        dst_node_id        = 0;
    uint32_t        target_function_id = 0;
    AT_ControlValue bound_arg          = {};
    std::string     bound_string_storage;
};

struct GraphNodeInstance {
    uint32_t                     node_id = 0;
    std::string                  plugin_id;
    std::string                  display_name;
    const AT_PluginExportBundle* bundle  = nullptr;
    AT_NodeHandle                handle  = nullptr;
    AlignedPlanarBuffer          output_bus{2, 256};
    std::vector<uint32_t>        upstream_node_ids;
    std::vector<uint32_t>        downstream_node_ids;
};

class AudioGraphEngine {
public:
    AudioGraphEngine();
    ~AudioGraphEngine();

    // Initializes paths (%LOCALAPPDATA%\Rouvr\AudioTool), writes Registry keys, and scans plugins
    bool initialize(const EngineConfig& cfg = {}, std::string* out_error = nullptr);

    // Shuts down real-time thread, destroys nodes, and unloads plugins
    void shutdown();

    // Runtime reload of constants (buffer size, sample rate, max connections) without losing graph state
    bool reload_config(const EngineConfig& new_cfg, std::string* out_error = nullptr);

    const EngineConfig& config() const noexcept { return config_; }
    DeviceManager& device_manager() noexcept { return device_manager_; }
    PluginManager& plugin_manager() noexcept { return plugin_manager_; }

    // --- Node & Graph Topology API ---
    uint32_t spawn_node(const std::string& plugin_id, const std::string& custom_name = "", std::string* out_error = nullptr);
    bool     delete_node(uint32_t node_id);
    bool     disconnect_all_node_wires(uint32_t node_id);

    // Audio Plane Connections
    uint32_t connect_audio(uint32_t src_node_id, uint32_t dst_node_id, std::string* out_error = nullptr);
    bool     disconnect_audio(uint32_t edge_id);

    // Control Plane Connections
    uint32_t connect_control(
        uint32_t src_node_id,
        uint32_t src_output_port,
        uint32_t dst_node_id,
        uint32_t target_function_id,
        const AT_ControlValue& bound_arg,
        std::string* out_error = nullptr);
    bool     disconnect_control(uint32_t edge_id);

    // --- Node Control & Function API ---
    AT_Result set_node_control_value(uint32_t node_id, uint32_t control_id, const AT_ControlValue& val);
    AT_Result set_node_control_by_identifier(uint32_t node_id, const std::string& identifier, const AT_ControlValue& val);
    AT_Result get_node_control_value(uint32_t node_id, uint32_t control_id, AT_ControlValue* out_val) const;
    AT_Result get_node_control_by_identifier(uint32_t node_id, const std::string& identifier, AT_ControlValue* out_val) const;

    AT_Result invoke_node_function(uint32_t node_id, uint32_t function_id, const AT_ControlValue& arg);
    AT_Result invoke_node_function_by_identifier(uint32_t node_id, const std::string& identifier, const AT_ControlValue& arg);

    // --- Execution Control ---
    // Executes a single deterministic audio & control tick (buffer_size_frames samples)
    void process_single_tick();

    // Starts/stops continuous real-time audio & control thread loop
    bool start_realtime_loop();
    void stop_realtime_loop();
    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    uint64_t total_frames_processed() const noexcept { return sample_timestamp_.load(std::memory_order_relaxed); }

    // Internal callback invoked by plugins via AT_HostServices::emit_control_output
    void on_node_emit_control_output(uint32_t src_node_id, uint32_t output_port_index);

private:
    bool rebuild_topological_order(std::string* out_error = nullptr);
    void realtime_worker_loop();

    mutable std::recursive_mutex                              graph_mutex_;
    EngineConfig                                              config_;
    DeviceManager                                             device_manager_;
    PluginManager                                             plugin_manager_;
    AT_HostServices                                           host_services_{};

    std::unordered_map<uint32_t, std::unique_ptr<GraphNodeInstance>> nodes_;
    std::vector<AudioEdge>                                    audio_edges_;
    std::vector<ControlEdge>                                  control_edges_;
    std::vector<GraphNodeInstance*>                           sorted_audio_nodes_;
    std::vector<GraphNodeInstance*>                           control_nodes_;

    uint32_t                                                  next_node_id_ = 100;
    uint32_t                                                  next_edge_id_ = 1000;
    std::atomic<uint64_t>                                     sample_timestamp_{0};

    std::atomic<bool>                                         running_{false};
    std::thread                                               rt_thread_;
};

} // namespace at
