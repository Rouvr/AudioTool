#include "engine/AudioGraphEngine.h"
#include "common/AppPathsAndRegistry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <queue>
#include <unordered_set>

namespace at {

namespace {

void AT_CDECL host_log_message(void* /*ctx*/, int32_t level, const char* plugin_id, const char* message) {
    if (level >= 2) {
        std::cerr << "[" << (plugin_id ? plugin_id : "plugin") << "] " << (message ? message : "") << "\n";
    }
}

void AT_CDECL host_emit_control_output(void* ctx, uint32_t runtime_node_id, uint32_t output_port_index) {
    if (!ctx) return;
    static_cast<AudioGraphEngine*>(ctx)->on_node_emit_control_output(runtime_node_id, output_port_index);
}

void AT_CDECL host_notify_controls_dirty(void* /*ctx*/, uint32_t /*runtime_node_id*/) {
    // Used by GUI notification bus when dynamic controls change
}

AT_DeviceStreamHandle AT_CDECL host_acquire_hardware_device(
    void* ctx,
    const char* device_endpoint_id,
    bool is_input_capture,
    bool is_loopback) {
    if (!ctx) return nullptr;
    return static_cast<AudioGraphEngine*>(ctx)->device_manager().acquire_device(
        device_endpoint_id, is_input_capture, is_loopback);
}

void AT_CDECL host_release_hardware_device(void* ctx, AT_DeviceStreamHandle stream_handle) {
    if (!ctx) return;
    static_cast<AudioGraphEngine*>(ctx)->device_manager().release_device(stream_handle);
}

AT_Result AT_CDECL host_read_hardware_input(
    void* ctx,
    AT_DeviceStreamHandle stream_handle,
    float* const* out_planar_channels,
    uint32_t num_channels,
    uint32_t num_frames) {
    if (!ctx) return AT_ERR_INVALID_ARG;
    return static_cast<AudioGraphEngine*>(ctx)->device_manager().read_hardware_input(
        stream_handle, out_planar_channels, num_channels, num_frames);
}

AT_Result AT_CDECL host_write_hardware_output(
    void* ctx,
    AT_DeviceStreamHandle stream_handle,
    const float* const* in_planar_channels,
    uint32_t num_channels,
    uint32_t num_frames) {
    if (!ctx) return AT_ERR_INVALID_ARG;
    return static_cast<AudioGraphEngine*>(ctx)->device_manager().write_hardware_output(
        stream_handle, in_planar_channels, num_channels, num_frames);
}

} // namespace

AudioGraphEngine::AudioGraphEngine() {
    host_services_.struct_size             = sizeof(AT_HostServices);
    host_services_.host_context            = this;
    host_services_.log_message             = &host_log_message;
    host_services_.emit_control_output     = &host_emit_control_output;
    host_services_.notify_controls_dirty   = &host_notify_controls_dirty;
    host_services_.acquire_hardware_device = &host_acquire_hardware_device;
    host_services_.release_hardware_device = &host_release_hardware_device;
    host_services_.read_hardware_input     = &host_read_hardware_input;
    host_services_.write_hardware_output   = &host_write_hardware_output;
}

AudioGraphEngine::~AudioGraphEngine() {
    shutdown();
}

bool AudioGraphEngine::initialize(const EngineConfig& cfg, std::string* out_error) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    config_ = cfg;

    // 1. Ensure %LOCALAPPDATA%\Rouvr\AudioTool directory structure exists
    if (!AppPathsAndRegistry::ensure_directories_exist(out_error)) {
        return false;
    }

    // 2. Write InstallPath, LocalDataPath, Version, BuildId to HKCU\Software\Rouvr\AudioTool
    if (!AppPathsAndRegistry::write_registry_metadata(out_error)) {
        return false;
    }

    // 3. Prepare HardwareDeviceManager
    device_manager_.prepare_all(config_.sample_rate, config_.buffer_size_frames);

    // 4. Scan %LOCALAPPDATA%\Rouvr\AudioTool\plugins for node DLLs
    std::vector<std::string> scan_errors;
    plugin_manager_.scan_plugins_directory(AppPathsAndRegistry::get_plugins_dir(), &scan_errors);

    return true;
}

void AudioGraphEngine::shutdown() {
    stop_realtime_loop();

    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    audio_edges_.clear();
    control_edges_.clear();
    sorted_audio_nodes_.clear();
    control_nodes_.clear();

    for (auto& kv : nodes_) {
        if (kv.second->handle && kv.second->bundle && kv.second->bundle->vtable->destroy_instance) {
            kv.second->bundle->vtable->destroy_instance(kv.second->handle);
            kv.second->handle = nullptr;
        }
    }
    nodes_.clear();
    plugin_manager_.unload_all();
}

bool AudioGraphEngine::reload_config(const EngineConfig& new_cfg, std::string* out_error) {
    if (new_cfg.buffer_size_frames < 16 || new_cfg.buffer_size_frames > 16384 ||
        new_cfg.sample_rate < 8000 || new_cfg.sample_rate > 192000 ||
        new_cfg.max_connections_per_port < 1) {
        if (out_error) *out_error = "Invalid engine configuration parameters.";
        return false;
    }

    const bool was_running = is_running();
    if (was_running) {
        stop_realtime_loop();
    }

    {
        std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
        config_ = new_cfg;
        device_manager_.prepare_all(config_.sample_rate, config_.buffer_size_frames);

        for (auto& kv : nodes_) {
            GraphNodeInstance* node = kv.second.get();
            node->output_bus.resize(2, config_.buffer_size_frames);
            if (node->bundle->descriptor->plane_type == AT_NODE_PLANE_AUDIO &&
                node->bundle->vtable->prepare_audio) {
                node->bundle->vtable->prepare_audio(
                    node->handle,
                    config_.sample_rate,
                    config_.buffer_size_frames,
                    config_.max_connections_per_port);
            }
        }
    }

    if (was_running) {
        start_realtime_loop();
    }
    return true;
}

uint32_t AudioGraphEngine::spawn_node(
    const std::string& plugin_id,
    const std::string& custom_name,
    std::string* out_error) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);

    const RegisteredPlugin* reg = plugin_manager_.find_plugin(plugin_id);
    if (!reg || !reg->bundle || !reg->bundle->vtable || !reg->bundle->vtable->create_instance) {
        if (out_error) *out_error = "Plugin ID not found in registry: " + plugin_id;
        return 0;
    }

    const uint32_t new_id = ++next_node_id_;
    AT_NodeHandle handle = reg->bundle->vtable->create_instance(&host_services_, new_id);
    if (!handle) {
        if (out_error) *out_error = "Plugin create_instance returned NULL for: " + plugin_id;
        return 0;
    }

    if (reg->bundle->descriptor->plane_type == AT_NODE_PLANE_AUDIO &&
        reg->bundle->vtable->prepare_audio) {
        reg->bundle->vtable->prepare_audio(
            handle,
            config_.sample_rate,
            config_.buffer_size_frames,
            config_.max_connections_per_port);
    }

    auto inst = std::make_unique<GraphNodeInstance>();
    inst->node_id      = new_id;
    inst->plugin_id    = plugin_id;
    inst->display_name = custom_name.empty() ? reg->display_name : custom_name;
    inst->bundle       = reg->bundle;
    inst->handle       = handle;
    inst->output_bus.resize(2, config_.buffer_size_frames);

    nodes_[new_id] = std::move(inst);
    rebuild_topological_order();
    return new_id;
}

bool AudioGraphEngine::delete_node(uint32_t node_id) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return false;

    disconnect_all_node_wires(node_id);

    if (it->second->handle && it->second->bundle->vtable->destroy_instance) {
        it->second->bundle->vtable->destroy_instance(it->second->handle);
    }
    nodes_.erase(it);
    rebuild_topological_order();
    return true;
}

bool AudioGraphEngine::disconnect_all_node_wires(uint32_t node_id) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    if (nodes_.find(node_id) == nodes_.end()) return false;

    audio_edges_.erase(
        std::remove_if(audio_edges_.begin(), audio_edges_.end(), [&](const AudioEdge& e) {
            return e.src_node_id == node_id || e.dst_node_id == node_id;
        }),
        audio_edges_.end());

    control_edges_.erase(
        std::remove_if(control_edges_.begin(), control_edges_.end(), [&](const ControlEdge& e) {
            return e.src_node_id == node_id || e.dst_node_id == node_id;
        }),
        control_edges_.end());

    rebuild_topological_order();
    return true;
}

uint32_t AudioGraphEngine::connect_audio(
    uint32_t src_node_id,
    uint32_t dst_node_id,
    std::string* out_error) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);

    if (src_node_id == dst_node_id) {
        if (out_error) *out_error = "Cannot connect a node to itself.";
        return 0;
    }

    auto src_it = nodes_.find(src_node_id);
    auto dst_it = nodes_.find(dst_node_id);
    if (src_it == nodes_.end() || dst_it == nodes_.end()) {
        if (out_error) *out_error = "Source or destination node does not exist.";
        return 0;
    }

    GraphNodeInstance* src = src_it->second.get();
    GraphNodeInstance* dst = dst_it->second.get();

    if (src->bundle->descriptor->plane_type != AT_NODE_PLANE_AUDIO ||
        dst->bundle->descriptor->plane_type != AT_NODE_PLANE_AUDIO) {
        if (out_error) *out_error = "Control-only nodes cannot be connected on the Audio Plane.";
        return 0;
    }

    if (src->bundle->descriptor->audio_category == AT_AUDIO_CAT_OUTPUT) {
        if (out_error) *out_error = "Output nodes do not have audio outputs.";
        return 0;
    }
    if (dst->bundle->descriptor->audio_category == AT_AUDIO_CAT_SOURCE) {
        if (out_error) *out_error = "Source nodes do not have audio inputs.";
        return 0;
    }

    // Check duplicate edge
    for (const auto& e : audio_edges_) {
        if (e.src_node_id == src_node_id && e.dst_node_id == dst_node_id) {
            if (out_error) *out_error = "Connection already exists between these nodes.";
            return 0;
        }
    }

    // Check max_connections_per_port limit (default 4)
    if (src->downstream_node_ids.size() >= config_.max_connections_per_port) {
        if (out_error) *out_error = "Source node reached maximum allowed output connections.";
        return 0;
    }
    if (dst->upstream_node_ids.size() >= config_.max_connections_per_port) {
        if (out_error) *out_error = "Destination node reached maximum allowed input connections.";
        return 0;
    }

    const uint32_t edge_id = ++next_edge_id_;
    audio_edges_.push_back(AudioEdge{edge_id, src_node_id, dst_node_id});

    if (!rebuild_topological_order(out_error)) {
        audio_edges_.pop_back();
        rebuild_topological_order();
        return 0;
    }

    return edge_id;
}

bool AudioGraphEngine::disconnect_audio(uint32_t edge_id) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = std::find_if(audio_edges_.begin(), audio_edges_.end(), [&](const AudioEdge& e) {
        return e.edge_id == edge_id;
    });
    if (it == audio_edges_.end()) return false;
    audio_edges_.erase(it);
    rebuild_topological_order();
    return true;
}

uint32_t AudioGraphEngine::connect_control(
    uint32_t src_node_id,
    uint32_t src_output_port,
    uint32_t dst_node_id,
    uint32_t target_function_id,
    const AT_ControlValue& bound_arg,
    std::string* out_error) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);

    if (nodes_.find(src_node_id) == nodes_.end() || nodes_.find(dst_node_id) == nodes_.end()) {
        if (out_error) *out_error = "Source or destination node does not exist.";
        return 0;
    }

    ControlEdge edge;
    edge.edge_id            = ++next_edge_id_;
    edge.src_node_id        = src_node_id;
    edge.src_output_port    = src_output_port;
    edge.dst_node_id        = dst_node_id;
    edge.target_function_id = target_function_id;
    edge.bound_arg          = bound_arg;
    if (bound_arg.type == AT_VAL_STRING && bound_arg.string_val) {
        edge.bound_string_storage = bound_arg.string_val;
        edge.bound_arg.string_val = edge.bound_string_storage.c_str();
    }

    const uint32_t id = edge.edge_id;
    control_edges_.push_back(std::move(edge));
    return id;
}

bool AudioGraphEngine::disconnect_control(uint32_t edge_id) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = std::find_if(control_edges_.begin(), control_edges_.end(), [&](const ControlEdge& e) {
        return e.edge_id == edge_id;
    });
    if (it == control_edges_.end()) return false;
    control_edges_.erase(it);
    return true;
}

AT_Result AudioGraphEngine::set_node_control_value(
    uint32_t node_id,
    uint32_t control_id,
    const AT_ControlValue& val) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second->bundle->vtable->set_control_value) {
        return AT_ERR_NOT_FOUND;
    }
    return it->second->bundle->vtable->set_control_value(it->second->handle, control_id, &val);
}

AT_Result AudioGraphEngine::set_node_control_by_identifier(
    uint32_t node_id,
    const std::string& identifier,
    const AT_ControlValue& val) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second->bundle->vtable->get_controls) {
        return AT_ERR_NOT_FOUND;
    }
    uint32_t count = 0;
    const AT_ControlDescriptor* ctrls = it->second->bundle->vtable->get_controls(it->second->handle, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (ctrls[i].identifier && identifier == ctrls[i].identifier) {
            return set_node_control_value(node_id, ctrls[i].control_id, val);
        }
    }
    return AT_ERR_NOT_FOUND;
}

AT_Result AudioGraphEngine::get_node_control_value(
    uint32_t node_id,
    uint32_t control_id,
    AT_ControlValue* out_val) const {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second->bundle->vtable->get_control_value) {
        return AT_ERR_NOT_FOUND;
    }
    return it->second->bundle->vtable->get_control_value(it->second->handle, control_id, out_val);
}

AT_Result AudioGraphEngine::get_node_control_by_identifier(
    uint32_t node_id,
    const std::string& identifier,
    AT_ControlValue* out_val) const {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second->bundle->vtable->get_controls) {
        return AT_ERR_NOT_FOUND;
    }
    uint32_t count = 0;
    const AT_ControlDescriptor* ctrls = it->second->bundle->vtable->get_controls(it->second->handle, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (ctrls[i].identifier && identifier == ctrls[i].identifier) {
            return get_node_control_value(node_id, ctrls[i].control_id, out_val);
        }
    }
    return AT_ERR_NOT_FOUND;
}

AT_Result AudioGraphEngine::invoke_node_function(
    uint32_t node_id,
    uint32_t function_id,
    const AT_ControlValue& arg) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second->bundle->vtable->invoke_function) {
        return AT_ERR_NOT_FOUND;
    }
    return it->second->bundle->vtable->invoke_function(it->second->handle, function_id, &arg);
}

AT_Result AudioGraphEngine::invoke_node_function_by_identifier(
    uint32_t node_id,
    const std::string& identifier,
    const AT_ControlValue& arg) {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second->bundle->vtable->get_functions) {
        return AT_ERR_NOT_FOUND;
    }
    uint32_t count = 0;
    const AT_FunctionDescriptor* funcs = it->second->bundle->vtable->get_functions(it->second->handle, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (funcs[i].identifier && identifier == funcs[i].identifier) {
            return invoke_node_function(node_id, funcs[i].function_id, arg);
        }
    }
    return AT_ERR_NOT_FOUND;
}

void AudioGraphEngine::on_node_emit_control_output(uint32_t src_node_id, uint32_t output_port_index) {
    for (const auto& edge : control_edges_) {
        if (edge.src_node_id == src_node_id && edge.src_output_port == output_port_index) {
            invoke_node_function(edge.dst_node_id, edge.target_function_id, edge.bound_arg);
        }
    }
}

bool AudioGraphEngine::rebuild_topological_order(std::string* out_error) {
    for (auto& kv : nodes_) {
        kv.second->upstream_node_ids.clear();
        kv.second->downstream_node_ids.clear();
    }

    std::unordered_map<uint32_t, uint32_t> in_degree;
    for (const auto& kv : nodes_) {
        if (kv.second->bundle->descriptor->plane_type == AT_NODE_PLANE_AUDIO) {
            in_degree[kv.first] = 0;
        }
    }

    for (const auto& edge : audio_edges_) {
        auto src_it = nodes_.find(edge.src_node_id);
        auto dst_it = nodes_.find(edge.dst_node_id);
        if (src_it != nodes_.end() && dst_it != nodes_.end()) {
            src_it->second->downstream_node_ids.push_back(edge.dst_node_id);
            dst_it->second->upstream_node_ids.push_back(edge.src_node_id);
            in_degree[edge.dst_node_id]++;
        }
    }

    std::queue<uint32_t> q;
    for (const auto& kv : in_degree) {
        if (kv.second == 0) {
            q.push(kv.first);
        }
    }

    std::vector<GraphNodeInstance*> sorted_audio;
    sorted_audio.reserve(in_degree.size());

    while (!q.empty()) {
        uint32_t u = q.front();
        q.pop();
        GraphNodeInstance* u_node = nodes_[u].get();
        sorted_audio.push_back(u_node);

        for (uint32_t v : u_node->downstream_node_ids) {
            if (--in_degree[v] == 0) {
                q.push(v);
            }
        }
    }

    if (sorted_audio.size() != in_degree.size()) {
        if (out_error) *out_error = "Cycle detected in audio graph; feedback loops are not allowed.";
        return false;
    }

    sorted_audio_nodes_ = std::move(sorted_audio);

    control_nodes_.clear();
    for (auto& kv : nodes_) {
        if (kv.second->bundle->descriptor->plane_type == AT_NODE_PLANE_CONTROL) {
            control_nodes_.push_back(kv.second.get());
        }
        if (kv.second->bundle->vtable->on_connections_changed) {
            kv.second->bundle->vtable->on_connections_changed(
                kv.second->handle,
                static_cast<uint32_t>(kv.second->upstream_node_ids.size()),
                static_cast<uint32_t>(kv.second->downstream_node_ids.size()));
        }
    }

    return true;
}

void AudioGraphEngine::process_single_tick() {
    std::lock_guard<std::recursive_mutex> lock(graph_mutex_);
    const uint32_t frames = config_.buffer_size_frames;
    const uint64_t ts = sample_timestamp_.load(std::memory_order_relaxed);
    const uint64_t time_ns = (ts * 1000000000ULL) / std::max<uint32_t>(1u, config_.sample_rate);

    // 1. Tick Control Nodes
    for (GraphNodeInstance* ctrl_node : control_nodes_) {
        if (ctrl_node->bundle->vtable->tick_control) {
            ctrl_node->bundle->vtable->tick_control(ctrl_node->handle, time_ns);
        }
    }

    // 2. Pull Hardware Input Ring Buffers & Clear Hardware Output Accumulation Buses
    device_manager_.begin_audio_tick(frames);

    // 3. Process Audio Graph in Topological Order
    std::vector<AT_AudioBusView> input_views;
    input_views.reserve(config_.max_connections_per_port);

    for (GraphNodeInstance* node : sorted_audio_nodes_) {
        node->output_bus.clear();
        input_views.clear();

        for (uint32_t up_id : node->upstream_node_ids) {
            auto up_it = nodes_.find(up_id);
            if (up_it != nodes_.end()) {
                AT_AudioBusView view{};
                view.source_node_id   = up_id;
                view.source_node_name = up_it->second->display_name.c_str();
                view.num_channels     = up_it->second->output_bus.num_channels();
                view.num_frames       = frames;
                view.channel_ptrs     = up_it->second->output_bus.channel_ptrs();
                input_views.push_back(view);
            }
        }

        AT_AudioProcessContext ctx{};
        ctx.sample_rate        = config_.sample_rate;
        ctx.num_frames         = frames;
        ctx.sample_timestamp   = ts;
        ctx.input_bus_count    = static_cast<uint32_t>(input_views.size());
        ctx.input_buses        = input_views.empty() ? nullptr : input_views.data();
        ctx.output_bus.source_node_id   = node->node_id;
        ctx.output_bus.source_node_name = node->display_name.c_str();
        ctx.output_bus.num_channels     = node->output_bus.num_channels();
        ctx.output_bus.num_frames       = frames;
        ctx.output_bus.channel_ptrs     = node->output_bus.channel_ptrs();

        if (node->bundle->vtable->process_audio) {
            node->bundle->vtable->process_audio(node->handle, &ctx);
        }
    }

    // 4. Push Accumulated Hardware Output Buses to WASAPI / Virtual Ring Buffers
    device_manager_.end_audio_tick(frames);

    sample_timestamp_.fetch_add(frames, std::memory_order_relaxed);
}

bool AudioGraphEngine::start_realtime_loop() {
    if (running_.load()) return true;
    running_.store(true);
    rt_thread_ = std::thread(&AudioGraphEngine::realtime_worker_loop, this);
    return true;
}

void AudioGraphEngine::stop_realtime_loop() {
    running_.store(false);
    if (rt_thread_.joinable()) {
        rt_thread_.join();
    }
}

void AudioGraphEngine::realtime_worker_loop() {
    enable_realtime_fpu_flags();

    DWORD task_idx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);

    using clock = std::chrono::steady_clock;
    auto next_wake = clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        process_single_tick();

        const auto tick_duration = std::chrono::microseconds(
            (static_cast< int64_t >(config_.buffer_size_frames) * 1000000LL) /
            std::max<uint32_t>(1u, config_.sample_rate));

        next_wake += tick_duration;
        std::this_thread::sleep_until(next_wake);
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}

} // namespace at
