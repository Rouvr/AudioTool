#include "engine/ProfileGraph.h"
#include "engine/AudioEngine.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <queue>

namespace at {

namespace {

constexpr uint32_t kMaxControlHops = 16;

using Lock = std::lock_guard<std::recursive_mutex>;

ProfileGraph* as_graph(void* ctx) { return static_cast<ProfileGraph*>(ctx); }

void AT_CDECL host_log_message(void* /*ctx*/, int32_t level, const char* plugin_id, const char* message) {
    if (level >= 2) {
        std::cerr << "[" << (plugin_id ? plugin_id : "plugin") << "] " << (message ? message : "") << "\n";
    }
}

void AT_CDECL host_emit_control_output(void* ctx, uint32_t runtime_node_id, uint32_t output_port_index) {
    if (ctx) as_graph(ctx)->on_node_emit_control_output(runtime_node_id, output_port_index);
}

void AT_CDECL host_notify_controls_dirty(void* /*ctx*/, uint32_t /*runtime_node_id*/) {
    // GUI polls snapshots every frame, so dirty notifications need no extra plumbing yet
}

AT_DeviceStreamHandle AT_CDECL host_acquire_hardware_device(void* ctx, const char* id, bool is_input, bool is_loopback) {
    return ctx ? as_graph(ctx)->engine().device_manager().acquire_device(id, is_input, is_loopback) : nullptr;
}

void AT_CDECL host_release_hardware_device(void* ctx, AT_DeviceStreamHandle h) {
    if (ctx) as_graph(ctx)->engine().device_manager().release_device(h);
}

AT_Result AT_CDECL host_read_hardware_input(void* ctx, AT_DeviceStreamHandle h, float* const* out, uint32_t ch, uint32_t n) {
    return ctx ? as_graph(ctx)->engine().device_manager().read_hardware_input(h, out, ch, n) : AT_ERR_INVALID_ARG;
}

AT_Result AT_CDECL host_write_hardware_output(void* ctx, AT_DeviceStreamHandle h, const float* const* in, uint32_t ch, uint32_t n) {
    return ctx ? as_graph(ctx)->engine().device_manager().write_hardware_output(h, in, ch, n) : AT_ERR_INVALID_ARG;
}

int32_t AT_CDECL host_get_hardware_device_status(void* ctx, AT_DeviceStreamHandle h, char* msg_buf, uint32_t msg_len) {
    if (!ctx) return AT_DEVICE_STATUS_NONE;
    std::string msg;
    const int32_t st = as_graph(ctx)->engine().device_manager().get_device_status(h, &msg);
    if (msg_buf && msg_len > 0) {
        const size_t n = std::min<size_t>(msg.size(), msg_len - 1);
        std::memcpy(msg_buf, msg.data(), n);
        msg_buf[n] = '\0';
    }
    return st;
}

ControlSnapshot copy_descriptor(const AT_ControlDescriptor& d) {
    ControlSnapshot c;
    c.control_id      = d.control_id;
    c.identifier      = d.identifier ? d.identifier : "";
    c.display_name    = d.display_name ? d.display_name : c.identifier;
    c.unit_label      = d.unit_label ? d.unit_label : "";
    c.control_type    = d.control_type;
    c.preferred_style = d.preferred_style;
    c.value_type      = d.value_type;
    c.min_value       = d.min_value;
    c.max_value       = d.max_value;
    c.step_value      = d.step_value;
    c.is_logarithmic  = d.is_logarithmic;
    c.default_value   = OwnedValue::from(d.default_value);
    c.file_filters    = d.file_filters ? d.file_filters : "";
    c.device_filter   = d.device_filter;
    for (uint32_t i = 0; i < d.predefined_count && d.predefined_values; ++i) {
        c.predefined_values.emplace_back(d.predefined_values[i] ? d.predefined_values[i] : "");
    }
    return c;
}

FunctionSnapshot copy_function(const AT_FunctionDescriptor& d) {
    FunctionSnapshot f;
    f.function_id       = d.function_id;
    f.identifier        = d.identifier ? d.identifier : "";
    f.display_name      = d.display_name ? d.display_name : f.identifier;
    f.description       = d.description ? d.description : "";
    f.op_kind           = d.op_kind;
    f.target_control_id = d.target_control_id;
    f.parameter_type    = d.parameter_type;
    f.parameter_name    = d.parameter_name ? d.parameter_name : "";
    f.parameter_unit    = d.parameter_unit ? d.parameter_unit : "";
    f.min_value         = d.min_value;
    f.max_value         = d.max_value;
    f.default_value     = d.default_value;
    return f;
}

} // namespace

ProfileGraph::ProfileGraph(AudioEngine& engine, uint32_t profile_id, const std::string& name)
    : engine_(engine), profile_id_(profile_id) {
    meta_.name = name;
    host_services_.struct_size                = sizeof(AT_HostServices);
    host_services_.host_context               = this;
    host_services_.log_message                = &host_log_message;
    host_services_.emit_control_output        = &host_emit_control_output;
    host_services_.notify_controls_dirty      = &host_notify_controls_dirty;
    host_services_.acquire_hardware_device    = &host_acquire_hardware_device;
    host_services_.release_hardware_device    = &host_release_hardware_device;
    host_services_.read_hardware_input        = &host_read_hardware_input;
    host_services_.write_hardware_output      = &host_write_hardware_output;
    host_services_.get_hardware_device_status = &host_get_hardware_device_status;
}

ProfileGraph::~ProfileGraph() {
    Lock lock(engine_.mutex());
    audio_edges_.clear();
    control_edges_.clear();
    sorted_audio_nodes_.clear();
    control_nodes_.clear();
    for (auto& kv : nodes_) {
        destroy_node_instance(*kv.second);
    }
    nodes_.clear();
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

ProfileMetadata ProfileGraph::metadata() const {
    Lock lock(engine_.mutex());
    return meta_;
}

void ProfileGraph::set_metadata(const ProfileMetadata& meta) {
    Lock lock(engine_.mutex());
    meta_ = meta;
}

void ProfileGraph::set_name(const std::string& name) {
    Lock lock(engine_.mutex());
    meta_.name = name;
}

void ProfileGraph::set_file_path(const std::string& path) {
    Lock lock(engine_.mutex());
    meta_.file_path = path;
}

void ProfileGraph::set_canvas_state(float pan_x, float pan_y, const std::string& plane_view, float zoom) {
    Lock lock(engine_.mutex());
    meta_.canvas_pan_x      = pan_x;
    meta_.canvas_pan_y      = pan_y;
    meta_.active_plane_view = plane_view;
    if (zoom > 0.05f) {
        meta_.canvas_zoom = zoom;
    }
}

bool ProfileGraph::is_active() const {
    Lock lock(engine_.mutex());
    return active_;
}

void ProfileGraph::set_active(bool active) {
    Lock lock(engine_.mutex());
    active_ = active;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

GraphNodeInstance* ProfileGraph::find_node(uint32_t node_id) const {
    auto it = nodes_.find(node_id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

void ProfileGraph::destroy_node_instance(GraphNodeInstance& node) {
    if (node.handle && node.bundle && node.bundle->vtable->destroy_instance) {
        node.bundle->vtable->destroy_instance(node.handle);
    }
    node.handle = nullptr;
}

uint32_t ProfileGraph::spawn_node(const std::string& plugin_id, const std::string& custom_name, std::string* out_error) {
    Lock lock(engine_.mutex());
    return spawn_node_with_id(plugin_id, 0, custom_name, out_error);
}

uint32_t ProfileGraph::spawn_node_with_id(const std::string& plugin_id, uint32_t node_id, const std::string& custom_name,
                                          std::string* out_error) {
    Lock lock(engine_.mutex());

    const RegisteredPlugin* reg = engine_.plugin_manager().find_plugin(plugin_id);
    if (!reg || !reg->bundle || !reg->bundle->vtable || !reg->bundle->vtable->create_instance) {
        if (out_error) *out_error = "Plugin ID not found in registry: " + plugin_id;
        return 0;
    }

    if (node_id == 0) {
        node_id = ++next_node_id_;
    } else {
        if (nodes_.count(node_id)) {
            if (out_error) *out_error = "Node ID already in use: " + std::to_string(node_id);
            return 0;
        }
        next_node_id_ = std::max(next_node_id_, node_id);
    }

    const EngineConfig& cfg = engine_.config_unlocked();
    AT_NodeHandle handle = reg->bundle->vtable->create_instance(&host_services_, node_id);
    if (!handle) {
        if (out_error) *out_error = "Plugin create_instance returned NULL for: " + plugin_id;
        return 0;
    }

    if (reg->bundle->descriptor->plane_type == AT_NODE_PLANE_AUDIO && reg->bundle->vtable->prepare_audio) {
        reg->bundle->vtable->prepare_audio(handle, cfg.sample_rate, cfg.buffer_size_frames, cfg.max_connections_per_port);
    }

    auto inst = std::make_unique<GraphNodeInstance>();
    inst->node_id      = node_id;
    inst->plugin_id    = plugin_id;
    inst->display_name = custom_name.empty() ? reg->display_name : custom_name;
    inst->bundle       = reg->bundle;
    inst->handle       = handle;
    inst->output_bus.resize(2, cfg.buffer_size_frames);

    nodes_[node_id] = std::move(inst);
    rebuild_topology();
    return node_id;
}

bool ProfileGraph::delete_node(uint32_t node_id) {
    Lock lock(engine_.mutex());
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return false;
    disconnect_all_node_wires(node_id);
    destroy_node_instance(*it->second);
    nodes_.erase(it);
    rebuild_topology();
    return true;
}

bool ProfileGraph::disconnect_all_node_wires(uint32_t node_id, bool audio_plane, bool control_plane) {
    Lock lock(engine_.mutex());
    if (!find_node(node_id)) return false;
    if (audio_plane) {
        audio_edges_.erase(std::remove_if(audio_edges_.begin(), audio_edges_.end(),
                                          [&](const AudioEdge& e) { return e.src_node_id == node_id || e.dst_node_id == node_id; }),
                           audio_edges_.end());
    }
    if (control_plane) {
        control_edges_.erase(std::remove_if(control_edges_.begin(), control_edges_.end(),
                                            [&](const ControlEdge& e) { return e.src_node_id == node_id || e.dst_node_id == node_id; }),
                             control_edges_.end());
    }
    rebuild_topology();
    return true;
}

bool ProfileGraph::set_node_ui(uint32_t node_id, const NodeUiState& ui) {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n) return false;
    n->ui = ui;
    return true;
}

bool ProfileGraph::rename_node(uint32_t node_id, const std::string& name) {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n || name.empty()) return false;
    n->display_name = name;
    rebuild_topology(); // refresh downstream input labels
    return true;
}

size_t ProfileGraph::node_count() const {
    Lock lock(engine_.mutex());
    return nodes_.size();
}

size_t ProfileGraph::count_instances_of(const std::string& plugin_id) const {
    Lock lock(engine_.mutex());
    size_t n = 0;
    for (const auto& kv : nodes_) {
        if (kv.second->plugin_id == plugin_id) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Audio plane
// ---------------------------------------------------------------------------

uint32_t ProfileGraph::connect_audio(uint32_t src_node_id, uint32_t dst_node_id, std::string* out_error) {
    Lock lock(engine_.mutex());

    if (src_node_id == dst_node_id) {
        if (out_error) *out_error = "Cannot connect a node to itself.";
        return 0;
    }
    GraphNodeInstance* src = find_node(src_node_id);
    GraphNodeInstance* dst = find_node(dst_node_id);
    if (!src || !dst) {
        if (out_error) *out_error = "Source or destination node does not exist.";
        return 0;
    }
    if (src->bundle->descriptor->plane_type != AT_NODE_PLANE_AUDIO || dst->bundle->descriptor->plane_type != AT_NODE_PLANE_AUDIO) {
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
    for (const auto& e : audio_edges_) {
        if (e.src_node_id == src_node_id && e.dst_node_id == dst_node_id) {
            if (out_error) *out_error = "Connection already exists between these nodes.";
            return 0;
        }
    }
    const uint32_t max_conn = engine_.config_unlocked().max_connections_per_port;
    if (src->downstream_node_ids.size() >= max_conn) {
        if (out_error) *out_error = "Source node reached the maximum of " + std::to_string(max_conn) + " output connections.";
        return 0;
    }
    if (dst->upstream_node_ids.size() >= max_conn) {
        if (out_error) *out_error = "Destination node reached the maximum of " + std::to_string(max_conn) + " input connections.";
        return 0;
    }

    const uint32_t edge_id = ++next_edge_id_;
    audio_edges_.push_back(AudioEdge{edge_id, src_node_id, dst_node_id});
    if (!rebuild_topology(out_error)) {
        audio_edges_.pop_back();
        rebuild_topology();
        return 0;
    }
    return edge_id;
}

bool ProfileGraph::disconnect_audio(uint32_t edge_id) {
    Lock lock(engine_.mutex());
    auto it = std::find_if(audio_edges_.begin(), audio_edges_.end(), [&](const AudioEdge& e) { return e.edge_id == edge_id; });
    if (it == audio_edges_.end()) return false;
    audio_edges_.erase(it);
    rebuild_topology();
    return true;
}

// ---------------------------------------------------------------------------
// Control plane
// ---------------------------------------------------------------------------

uint32_t ProfileGraph::connect_control(uint32_t src_node_id, uint32_t src_output_port, uint32_t dst_node_id,
                                       uint32_t target_function_id, const OwnedValue& bound_arg, std::string* out_error) {
    Lock lock(engine_.mutex());
    if (src_node_id == dst_node_id) {
        if (out_error) *out_error = "Cannot connect a node to itself.";
        return 0;
    }
    GraphNodeInstance* src = find_node(src_node_id);
    GraphNodeInstance* dst = find_node(dst_node_id);
    if (!src || !dst) {
        if (out_error) *out_error = "Source or destination node does not exist.";
        return 0;
    }
    bool func_found = false;
    if (dst->bundle->vtable->get_functions) {
        uint32_t count = 0;
        const AT_FunctionDescriptor* funcs = dst->bundle->vtable->get_functions(dst->handle, &count);
        for (uint32_t i = 0; i < count; ++i) {
            if (funcs[i].function_id == target_function_id) func_found = true;
        }
    }
    if (!func_found) {
        if (out_error) *out_error = "Target node has no function with ID " + std::to_string(target_function_id);
        return 0;
    }

    ControlEdge edge;
    edge.edge_id            = ++next_edge_id_;
    edge.src_node_id        = src_node_id;
    edge.src_output_port    = src_output_port;
    edge.dst_node_id        = dst_node_id;
    edge.target_function_id = target_function_id;
    edge.bound_arg          = bound_arg;
    control_edges_.push_back(std::move(edge));
    return control_edges_.back().edge_id;
}

bool ProfileGraph::disconnect_control(uint32_t edge_id) {
    Lock lock(engine_.mutex());
    auto it = std::find_if(control_edges_.begin(), control_edges_.end(), [&](const ControlEdge& e) { return e.edge_id == edge_id; });
    if (it == control_edges_.end()) return false;
    control_edges_.erase(it);
    return true;
}

void ProfileGraph::on_node_emit_control_output(uint32_t src_node_id, uint32_t output_port_index) {
    Lock lock(engine_.mutex());
    if (control_depth_ >= kMaxControlHops) return; // breaks control feedback loops
    ++control_depth_;
    const auto now = std::chrono::steady_clock::now();
    // Copy: invoked functions may (in future) mutate the edge list
    std::vector<ControlEdge*> targets;
    for (auto& edge : control_edges_) {
        if (edge.src_node_id == src_node_id && edge.src_output_port == output_port_index) targets.push_back(&edge);
    }
    for (ControlEdge* edge : targets) {
        edge->last_fire = now;
        GraphNodeInstance* dst = find_node(edge->dst_node_id);
        if (dst && dst->bundle->vtable->invoke_function) {
            const AT_ControlValue arg = edge->bound_arg.view();
            dst->bundle->vtable->invoke_function(dst->handle, edge->target_function_id, &arg);
        }
    }
    --control_depth_;
}

// ---------------------------------------------------------------------------
// Controls & functions
// ---------------------------------------------------------------------------

uint32_t ProfileGraph::find_control_id(const GraphNodeInstance& node, const std::string& identifier) const {
    if (!node.bundle->vtable->get_controls) return 0;
    uint32_t count = 0;
    const AT_ControlDescriptor* ctrls = node.bundle->vtable->get_controls(node.handle, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (ctrls[i].identifier && identifier == ctrls[i].identifier) return ctrls[i].control_id;
    }
    return 0;
}

uint32_t ProfileGraph::find_function_id(const GraphNodeInstance& node, const std::string& identifier) const {
    if (!node.bundle->vtable->get_functions) return 0;
    uint32_t count = 0;
    const AT_FunctionDescriptor* funcs = node.bundle->vtable->get_functions(node.handle, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (funcs[i].identifier && identifier == funcs[i].identifier) return funcs[i].function_id;
    }
    return 0;
}

AT_Result ProfileGraph::set_control(uint32_t node_id, uint32_t control_id, const OwnedValue& val) {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n || !n->bundle->vtable->set_control_value) return AT_ERR_NOT_FOUND;
    const AT_ControlValue v = val.view();
    return n->bundle->vtable->set_control_value(n->handle, control_id, &v);
}

AT_Result ProfileGraph::set_control(uint32_t node_id, const std::string& identifier, const OwnedValue& val) {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n) return AT_ERR_NOT_FOUND;
    const uint32_t cid = find_control_id(*n, identifier);
    return cid ? set_control(node_id, cid, val) : AT_ERR_NOT_FOUND;
}

AT_Result ProfileGraph::get_control(uint32_t node_id, uint32_t control_id, OwnedValue* out_val) const {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n || !n->bundle->vtable->get_control_value || !out_val) return AT_ERR_NOT_FOUND;
    AT_ControlValue v{};
    const AT_Result r = n->bundle->vtable->get_control_value(n->handle, control_id, &v);
    if (r == AT_OK) *out_val = OwnedValue::from(v);
    return r;
}

AT_Result ProfileGraph::get_control(uint32_t node_id, const std::string& identifier, OwnedValue* out_val) const {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n) return AT_ERR_NOT_FOUND;
    const uint32_t cid = find_control_id(*n, identifier);
    return cid ? get_control(node_id, cid, out_val) : AT_ERR_NOT_FOUND;
}

AT_Result ProfileGraph::invoke_function(uint32_t node_id, uint32_t function_id, const OwnedValue& arg) {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n || !n->bundle->vtable->invoke_function) return AT_ERR_NOT_FOUND;
    const AT_ControlValue v = arg.view();
    return n->bundle->vtable->invoke_function(n->handle, function_id, &v);
}

AT_Result ProfileGraph::invoke_function(uint32_t node_id, const std::string& identifier, const OwnedValue& arg) {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n) return AT_ERR_NOT_FOUND;
    const uint32_t fid = find_function_id(*n, identifier);
    return fid ? invoke_function(node_id, fid, arg) : AT_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

void ProfileGraph::fill_node_snapshot(const GraphNodeInstance& node, NodeSnapshot& out) const {
    out.node_id            = node.node_id;
    out.plugin_id          = node.plugin_id;
    out.display_name       = node.display_name;
    out.plane_type         = node.bundle->descriptor->plane_type;
    out.audio_category     = node.bundle->descriptor->audio_category;
    out.flags              = node.bundle->descriptor->flags;
    out.ui                 = node.ui;
    out.audio_input_count  = static_cast<uint32_t>(node.upstream_node_ids.size());
    out.audio_output_count = static_cast<uint32_t>(node.downstream_node_ids.size());
    out.controls.clear();
    out.functions.clear();

    const AT_NodeVTable* vt = node.bundle->vtable;
    if (vt->get_controls) {
        uint32_t count = 0;
        const AT_ControlDescriptor* ctrls = vt->get_controls(node.handle, &count);
        out.controls.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ControlSnapshot c = copy_descriptor(ctrls[i]);
            if (vt->get_control_value) {
                AT_ControlValue v{};
                if (vt->get_control_value(node.handle, c.control_id, &v) == AT_OK) c.value = OwnedValue::from(v);
            }
            out.controls.push_back(std::move(c));
        }
    }
    if (vt->get_functions) {
        uint32_t count = 0;
        const AT_FunctionDescriptor* funcs = vt->get_functions(node.handle, &count);
        out.functions.reserve(count);
        for (uint32_t i = 0; i < count; ++i) out.functions.push_back(copy_function(funcs[i]));
    }
}

bool ProfileGraph::node_snapshot(uint32_t node_id, NodeSnapshot& out) const {
    Lock lock(engine_.mutex());
    GraphNodeInstance* n = find_node(node_id);
    if (!n) return false;
    fill_node_snapshot(*n, out);
    return true;
}

ProfileSnapshot ProfileGraph::snapshot() const {
    Lock lock(engine_.mutex());
    ProfileSnapshot s;
    s.profile_id = profile_id_;
    s.active     = active_;
    s.meta       = meta_;

    std::vector<uint32_t> ids;
    ids.reserve(nodes_.size());
    for (const auto& kv : nodes_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end()); // stable draw / save order

    s.nodes.resize(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) fill_node_snapshot(*nodes_.at(ids[i]), s.nodes[i]);

    for (const auto& e : audio_edges_) s.audio_edges.push_back(AudioEdgeSnapshot{e.edge_id, e.src_node_id, e.dst_node_id});

    const auto now = std::chrono::steady_clock::now();
    for (const auto& e : control_edges_) {
        ControlEdgeSnapshot ce;
        ce.edge_id            = e.edge_id;
        ce.src_node_id        = e.src_node_id;
        ce.src_output_port    = e.src_output_port;
        ce.dst_node_id        = e.dst_node_id;
        ce.target_function_id = e.target_function_id;
        ce.bound_arg          = e.bound_arg;
        if (e.last_fire.time_since_epoch().count() != 0) {
            ce.seconds_since_last_fire = std::chrono::duration<double>(now - e.last_fire).count();
        }
        if (GraphNodeInstance* dst = find_node(e.dst_node_id); dst && dst->bundle->vtable->get_functions) {
            uint32_t count = 0;
            const AT_FunctionDescriptor* funcs = dst->bundle->vtable->get_functions(dst->handle, &count);
            for (uint32_t i = 0; i < count; ++i) {
                if (funcs[i].function_id == e.target_function_id) {
                    ce.target_function_identifier = funcs[i].identifier ? funcs[i].identifier : "";
                    ce.target_function_name       = funcs[i].display_name ? funcs[i].display_name : "";
                }
            }
        }
        s.control_edges.push_back(std::move(ce));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Topology & processing
// ---------------------------------------------------------------------------

bool ProfileGraph::rebuild_topology(std::string* out_error) {
    for (auto& kv : nodes_) {
        kv.second->upstream_node_ids.clear();
        kv.second->downstream_node_ids.clear();
    }

    std::unordered_map<uint32_t, uint32_t> in_degree;
    for (const auto& kv : nodes_) {
        if (kv.second->bundle->descriptor->plane_type == AT_NODE_PLANE_AUDIO) in_degree[kv.first] = 0;
    }
    for (const auto& edge : audio_edges_) {
        GraphNodeInstance* src = find_node(edge.src_node_id);
        GraphNodeInstance* dst = find_node(edge.dst_node_id);
        if (src && dst) {
            src->downstream_node_ids.push_back(edge.dst_node_id);
            dst->upstream_node_ids.push_back(edge.src_node_id);
            in_degree[edge.dst_node_id]++;
        }
    }

    // Kahn's algorithm (seeded in ascending ID order for deterministic execution order)
    std::vector<uint32_t> seeds;
    for (const auto& kv : in_degree) {
        if (kv.second == 0) seeds.push_back(kv.first);
    }
    std::sort(seeds.begin(), seeds.end());
    std::queue<uint32_t> q;
    for (uint32_t s : seeds) q.push(s);

    std::vector<GraphNodeInstance*> sorted;
    sorted.reserve(in_degree.size());
    while (!q.empty()) {
        const uint32_t u = q.front();
        q.pop();
        GraphNodeInstance* u_node = nodes_[u].get();
        sorted.push_back(u_node);
        for (uint32_t v : u_node->downstream_node_ids) {
            if (--in_degree[v] == 0) q.push(v);
        }
    }
    if (sorted.size() != in_degree.size()) {
        if (out_error) *out_error = "Cycle detected in audio graph; feedback loops are not allowed.";
        return false;
    }
    sorted_audio_nodes_ = std::move(sorted);

    control_nodes_.clear();
    std::vector<const char*> names;
    for (auto& kv : nodes_) {
        GraphNodeInstance* n = kv.second.get();
        if (n->bundle->descriptor->plane_type == AT_NODE_PLANE_CONTROL) control_nodes_.push_back(n);

        const AT_NodeVTable* vt = n->bundle->vtable;
        if (vt->on_connections_changed) {
            vt->on_connections_changed(n->handle, static_cast<uint32_t>(n->upstream_node_ids.size()),
                                       static_cast<uint32_t>(n->downstream_node_ids.size()));
        }
        if (vt->on_input_sources_changed) {
            names.clear();
            for (uint32_t up : n->upstream_node_ids) names.push_back(nodes_[up]->display_name.c_str());
            vt->on_input_sources_changed(n->handle, static_cast<uint32_t>(n->upstream_node_ids.size()),
                                         n->upstream_node_ids.empty() ? nullptr : n->upstream_node_ids.data(),
                                         names.empty() ? nullptr : names.data());
        }
    }
    std::sort(control_nodes_.begin(), control_nodes_.end(),
              [](const GraphNodeInstance* a, const GraphNodeInstance* b) { return a->node_id < b->node_id; });
    return true;
}

void ProfileGraph::prepare_all_nodes() {
    const EngineConfig& cfg = engine_.config_unlocked();
    for (auto& kv : nodes_) {
        GraphNodeInstance* node = kv.second.get();
        node->output_bus.resize(2, cfg.buffer_size_frames);
        if (node->bundle->descriptor->plane_type == AT_NODE_PLANE_AUDIO && node->bundle->vtable->prepare_audio) {
            node->bundle->vtable->prepare_audio(node->handle, cfg.sample_rate, cfg.buffer_size_frames, cfg.max_connections_per_port);
        }
    }
}

void ProfileGraph::process_tick(uint32_t frames, uint64_t sample_ts, uint64_t time_ns) {
    if (!active_) return;
    const uint32_t sample_rate = engine_.config_unlocked().sample_rate;

    // 1. Control plane: tick control nodes (they emit pulses synchronously via host services)
    for (GraphNodeInstance* ctrl : control_nodes_) {
        if (ctrl->bundle->vtable->tick_control) ctrl->bundle->vtable->tick_control(ctrl->handle, time_ns);
    }

    // 2. Audio plane in topological order
    for (GraphNodeInstance* node : sorted_audio_nodes_) {
        if (node->output_bus.num_frames() < frames) node->output_bus.resize(2, frames);
        node->output_bus.clear();

        scratch_input_views_.clear();
        for (uint32_t up_id : node->upstream_node_ids) {
            GraphNodeInstance* up = find_node(up_id);
            if (!up) continue;
            AT_AudioBusView view{};
            view.source_node_id   = up_id;
            view.source_node_name = up->display_name.c_str();
            view.num_channels     = up->output_bus.num_channels();
            view.num_frames       = frames;
            view.channel_ptrs     = up->output_bus.channel_ptrs();
            scratch_input_views_.push_back(view);
        }

        AT_AudioProcessContext ctx{};
        ctx.sample_rate                 = sample_rate;
        ctx.num_frames                  = frames;
        ctx.sample_timestamp            = sample_ts;
        ctx.input_bus_count             = static_cast<uint32_t>(scratch_input_views_.size());
        ctx.input_buses                 = scratch_input_views_.empty() ? nullptr : scratch_input_views_.data();
        ctx.output_bus.source_node_id   = node->node_id;
        ctx.output_bus.source_node_name = node->display_name.c_str();
        ctx.output_bus.num_channels     = node->output_bus.num_channels();
        ctx.output_bus.num_frames       = frames;
        ctx.output_bus.channel_ptrs     = node->output_bus.channel_ptrs();

        if (node->bundle->vtable->process_audio) node->bundle->vtable->process_audio(node->handle, &ctx);
    }
}

} // namespace at

