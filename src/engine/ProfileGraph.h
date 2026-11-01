#pragma once
// ============================================================================
// AudioTool (AT) — ProfileGraph: one workspace profile's audio + control graph
//
// All public methods lock the owning AudioEngine's graph mutex, so they may be
// called from any (non-audio) thread. process_tick() is called by AudioEngine
// with the mutex already held.
// ============================================================================

#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "engine/EngineTypes.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace at {

class AudioEngine;

struct AudioEdge {
    uint32_t edge_id     = 0;
    uint32_t src_node_id = 0;
    uint32_t dst_node_id = 0;
};

struct ControlEdge {
    uint32_t                              edge_id            = 0;
    uint32_t                              src_node_id        = 0;
    uint32_t                              src_output_port    = 0;
    uint32_t                              dst_node_id        = 0;
    uint32_t                              target_function_id = 0;
    OwnedValue                            bound_arg;
    std::chrono::steady_clock::time_point last_fire{};
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
    NodeUiState                  ui;
};

class ProfileGraph {
public:
    ProfileGraph(AudioEngine& engine, uint32_t profile_id, const std::string& name);
    ~ProfileGraph();

    ProfileGraph(const ProfileGraph&) = delete;
    ProfileGraph& operator=(const ProfileGraph&) = delete;

    uint32_t id() const noexcept { return profile_id_; }
    AudioEngine& engine() const noexcept { return engine_; }

    // --- Metadata ---
    ProfileMetadata metadata() const;
    void            set_metadata(const ProfileMetadata& meta);
    void            set_name(const std::string& name);
    void            set_file_path(const std::string& path);
    void            set_canvas_state(float pan_x, float pan_y, const std::string& plane_view);
    bool            is_active() const;
    void            set_active(bool active);

    // --- Nodes ---
    uint32_t spawn_node(const std::string& plugin_id, const std::string& custom_name = "", std::string* out_error = nullptr);
    uint32_t spawn_node_with_id(const std::string& plugin_id, uint32_t node_id, const std::string& custom_name, std::string* out_error);
    bool     delete_node(uint32_t node_id);
    bool     disconnect_all_node_wires(uint32_t node_id, bool audio_plane = true, bool control_plane = true);
    bool     set_node_ui(uint32_t node_id, const NodeUiState& ui);
    bool     rename_node(uint32_t node_id, const std::string& name);
    size_t   node_count() const;
    size_t   count_instances_of(const std::string& plugin_id) const;

    // --- Audio Plane ---
    uint32_t connect_audio(uint32_t src_node_id, uint32_t dst_node_id, std::string* out_error = nullptr);
    bool     disconnect_audio(uint32_t edge_id);

    // --- Control Plane ---
    uint32_t connect_control(uint32_t src_node_id, uint32_t src_output_port, uint32_t dst_node_id,
                             uint32_t target_function_id, const OwnedValue& bound_arg, std::string* out_error = nullptr);
    bool     disconnect_control(uint32_t edge_id);

    // --- Node controls / functions ---
    AT_Result set_control(uint32_t node_id, uint32_t control_id, const OwnedValue& val);
    AT_Result set_control(uint32_t node_id, const std::string& identifier, const OwnedValue& val);
    AT_Result get_control(uint32_t node_id, uint32_t control_id, OwnedValue* out_val) const;
    AT_Result get_control(uint32_t node_id, const std::string& identifier, OwnedValue* out_val) const;
    AT_Result invoke_function(uint32_t node_id, uint32_t function_id, const OwnedValue& arg);
    AT_Result invoke_function(uint32_t node_id, const std::string& identifier, const OwnedValue& arg);

    // --- Snapshots ---
    ProfileSnapshot snapshot() const;
    bool            node_snapshot(uint32_t node_id, NodeSnapshot& out) const;

    // --- Engine internals (called with engine mutex held) ---
    void process_tick(uint32_t frames, uint64_t sample_ts, uint64_t time_ns);
    void prepare_all_nodes();
    void on_node_emit_control_output(uint32_t src_node_id, uint32_t output_port_index);

private:
    bool rebuild_topology(std::string* out_error = nullptr);
    void fill_node_snapshot(const GraphNodeInstance& node, NodeSnapshot& out) const;
    GraphNodeInstance* find_node(uint32_t node_id) const;
    uint32_t           find_control_id(const GraphNodeInstance& node, const std::string& identifier) const;
    uint32_t           find_function_id(const GraphNodeInstance& node, const std::string& identifier) const;
    void               destroy_node_instance(GraphNodeInstance& node);

    AudioEngine&                                                     engine_;
    uint32_t                                                         profile_id_;
    ProfileMetadata                                                  meta_;
    bool                                                             active_ = true;
    AT_HostServices                                                  host_services_{};

    std::unordered_map<uint32_t, std::unique_ptr<GraphNodeInstance>> nodes_;
    std::vector<AudioEdge>                                           audio_edges_;
    std::vector<ControlEdge>                                         control_edges_;
    std::vector<GraphNodeInstance*>                                  sorted_audio_nodes_;
    std::vector<GraphNodeInstance*>                                  control_nodes_;
    std::vector<AT_AudioBusView>                                     scratch_input_views_;

    uint32_t                                                         next_node_id_ = 100;
    uint32_t                                                         next_edge_id_ = 1000;
    uint32_t                                                         control_depth_ = 0;
};

} // namespace at
