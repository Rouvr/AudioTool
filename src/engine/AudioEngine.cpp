#include "engine/AudioEngine.h"
#include "common/AppPathsAndRegistry.h"
#include "nlohmann/json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#include <chrono>
#include <fstream>
#include <iostream>

namespace at {

using Lock = std::lock_guard<std::recursive_mutex>;
using json = nlohmann::json;

AudioEngine::AudioEngine() {
    plugin_manager_.set_replace_predicate([this](const std::string& plugin_id) {
        Lock lock(graph_mutex_);
        for (const auto& kv : profiles_) {
            if (kv.second->count_instances_of(plugin_id) > 0) {
                return false;
            }
        }
        return true;
    });
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize(const EngineConfig& cfg, std::string* out_error) {
    Lock lock(graph_mutex_);
    config_ = cfg;

    if (!AppPathsAndRegistry::ensure_directories_exist(out_error)) {
        return false;
    }

    if (!AppPathsAndRegistry::write_registry_metadata(out_error)) {
        return false;
    }

    device_manager_.prepare_all(config_.sample_rate, config_.buffer_size_frames);

    std::vector<std::string> scan_errors;
    plugin_manager_.scan_plugins_directory(AppPathsAndRegistry::get_plugins_dir(), &scan_errors);

    return true;
}

void AudioEngine::shutdown() {
    stop_realtime_loop();

    Lock lock(graph_mutex_);
    profiles_.clear();
    plugin_manager_.unload_all();
}

bool AudioEngine::reload_config(const EngineConfig& new_cfg, std::string* out_error) {
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
        Lock lock(graph_mutex_);
        config_ = new_cfg;
        device_manager_.prepare_all(config_.sample_rate, config_.buffer_size_frames);

        for (auto& kv : profiles_) {
            kv.second->prepare_all_nodes();
        }
    }

    if (was_running) {
        start_realtime_loop();
    }
    return true;
}

EngineConfig AudioEngine::config() const {
    Lock lock(graph_mutex_);
    return config_;
}

EngineConfig AudioEngine::load_config_file() {
    EngineConfig cfg;
    std::filesystem::path path = AppPathsAndRegistry::get_config_file_path();
    if (!std::filesystem::exists(path)) {
        return cfg;
    }
    try {
        std::ifstream f(path);
        if (f.is_open()) {
            json j;
            f >> j;
            if (j.contains("sample_rate")) cfg.sample_rate = j["sample_rate"].get<uint32_t>();
            if (j.contains("buffer_size_frames")) cfg.buffer_size_frames = j["buffer_size_frames"].get<uint32_t>();
            if (j.contains("max_connections_per_port")) cfg.max_connections_per_port = j["max_connections_per_port"].get<uint32_t>();
        }
    } catch (...) {}
    return cfg;
}

bool AudioEngine::save_config_file(const EngineConfig& cfg, std::string* out_error) {
    try {
        std::filesystem::path path = AppPathsAndRegistry::get_config_file_path();
        std::filesystem::create_directories(path.parent_path());
        json j;
        j["sample_rate"] = cfg.sample_rate;
        j["buffer_size_frames"] = cfg.buffer_size_frames;
        j["max_connections_per_port"] = cfg.max_connections_per_port;
        std::ofstream f(path);
        if (!f.is_open()) {
            if (out_error) *out_error = "Failed to open config file for writing: " + path.string();
            return false;
        }
        f << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        if (out_error) *out_error = e.what();
        return false;
    }
}

size_t AudioEngine::rescan_plugins(std::vector<std::string>* out_errors) {
    Lock lock(graph_mutex_);
    return plugin_manager_.scan_plugins_directory(AppPathsAndRegistry::get_plugins_dir(), out_errors);
}

std::vector<PluginInfo> AudioEngine::list_plugins() const {
    Lock lock(graph_mutex_);
    std::vector<PluginInfo> out;
    auto registered = plugin_manager_.list_plugins();
    out.reserve(registered.size());
    for (const auto* p : registered) {
        PluginInfo info;
        info.plugin_id = p->plugin_id;
        info.display_name = p->display_name;
        info.relative_folder_path = p->relative_folder_path;
        if (p->bundle && p->bundle->descriptor) {
            info.plane_type = p->bundle->descriptor->plane_type;
            info.audio_category = p->bundle->descriptor->audio_category;
        }
        out.push_back(std::move(info));
    }
    return out;
}

ProfileGraph* AudioEngine::create_profile(const std::string& name) {
    Lock lock(graph_mutex_);
    uint32_t id = next_profile_id_++;
    auto prof = std::make_unique<ProfileGraph>(*this, id, name.empty() ? ("Profile " + std::to_string(id)) : name);
    ProfileGraph* ptr = prof.get();
    profiles_[id] = std::move(prof);
    return ptr;
}

ProfileGraph* AudioEngine::find_profile(uint32_t profile_id) {
    Lock lock(graph_mutex_);
    auto it = profiles_.find(profile_id);
    return it == profiles_.end() ? nullptr : it->second.get();
}

std::vector<uint32_t> AudioEngine::profile_ids() const {
    Lock lock(graph_mutex_);
    std::vector<uint32_t> ids;
    ids.reserve(profiles_.size());
    for (const auto& kv : profiles_) {
        ids.push_back(kv.first);
    }
    return ids;
}

bool AudioEngine::close_profile(uint32_t profile_id) {
    Lock lock(graph_mutex_);
    auto it = profiles_.find(profile_id);
    if (it == profiles_.end()) return false;
    profiles_.erase(it);
    return true;
}

ProfileGraph* AudioEngine::load_profile(const std::filesystem::path& file, std::string* out_error) {
    Lock lock(graph_mutex_);
    try {
        std::ifstream f(file);
        if (!f.is_open()) {
            if (out_error) *out_error = "Could not open profile file: " + file.string();
            return nullptr;
        }
        json j;
        f >> j;

        std::string name = file.stem().string();
        if (j.contains("profile_metadata") && j["profile_metadata"].contains("name")) {
            name = j["profile_metadata"]["name"].get<std::string>();
        }

        ProfileGraph* prof = create_profile(name);
        if (!prof) {
            if (out_error) *out_error = "Failed to instantiate profile";
            return nullptr;
        }

        ProfileMetadata meta;
        meta.name = name;
        meta.file_path = file.string();
        if (j.contains("profile_metadata")) {
            auto& m = j["profile_metadata"];
            if (m.contains("canvas_pan_x")) meta.canvas_pan_x = m["canvas_pan_x"].get<float>();
            if (m.contains("canvas_pan_y")) meta.canvas_pan_y = m["canvas_pan_y"].get<float>();
            if (m.contains("canvas_zoom")) meta.canvas_zoom = m["canvas_zoom"].get<float>();
            if (m.contains("active_plane_view")) meta.active_plane_view = m["active_plane_view"].get<std::string>();
        }
        prof->set_metadata(meta);

        // Spawn nodes
        if (j.contains("nodes") && j["nodes"].is_array()) {
            for (const auto& nj : j["nodes"]) {
                uint32_t node_id = nj.value("node_id", 0u);
                std::string plugin_id = nj.value("plugin_id", "");
                std::string display_name = nj.value("display_name", "");

                std::string spawn_err;
                uint32_t spawned = prof->spawn_node_with_id(plugin_id, node_id, display_name, &spawn_err);
                if (spawned == 0) {
                    continue; // Skip unknown / failed plugins gracefully
                }

                if (nj.contains("ui_state")) {
                    NodeUiState ui;
                    ui.pos_x = nj["ui_state"].value("pos_x", 0.0f);
                    ui.pos_y = nj["ui_state"].value("pos_y", 0.0f);
                    ui.is_expanded = nj["ui_state"].value("is_expanded", true);
                    prof->set_node_ui(spawned, ui);
                }

                if (nj.contains("control_values") && nj["control_values"].is_object()) {
                    for (auto& [ctrl_key, ctrl_val] : nj["control_values"].items()) {
                        OwnedValue v;
                        if (ctrl_val.is_boolean()) {
                            v = OwnedValue::make_bool(ctrl_val.get<bool>());
                        } else if (ctrl_val.is_number_integer()) {
                            v = OwnedValue::make_int(ctrl_val.get<int64_t>());
                        } else if (ctrl_val.is_number_float()) {
                            v = OwnedValue::make_float(ctrl_val.get<double>());
                        } else if (ctrl_val.is_string()) {
                            v = OwnedValue::make_string(ctrl_val.get<std::string>());
                        }
                        prof->set_control(spawned, ctrl_key, v);
                    }
                }
            }
        }

        // Audio connections
        if (j.contains("audio_connections") && j["audio_connections"].is_array()) {
            for (const auto& ej : j["audio_connections"]) {
                uint32_t src = ej.value("source_node_id", 0u);
                uint32_t dst = ej.value("target_node_id", 0u);
                if (src && dst) {
                    prof->connect_audio(src, dst);
                }
            }
        }

        // Control connections
        if (j.contains("control_connections") && j["control_connections"].is_array()) {
            for (const auto& cj : j["control_connections"]) {
                uint32_t src = cj.value("source_node_id", 0u);
                uint32_t port = cj.value("source_output_slot", 0u);
                uint32_t dst = cj.value("target_node_id", 0u);
                uint32_t func_id = cj.value("target_function_id", 0u);

                OwnedValue bound;
                if (cj.contains("bound_argument")) {
                    auto& bj = cj["bound_argument"];
                    if (bj.is_boolean()) bound = OwnedValue::make_bool(bj.get<bool>());
                    else if (bj.is_number_integer()) bound = OwnedValue::make_int(bj.get<int64_t>());
                    else if (bj.is_number_float()) bound = OwnedValue::make_float(bj.get<double>());
                    else if (bj.is_string()) bound = OwnedValue::make_string(bj.get<std::string>());
                }

                if (src && dst && func_id) {
                    prof->connect_control(src, port, dst, func_id, bound);
                }
            }
        }

        return prof;
    } catch (const std::exception& e) {
        if (out_error) *out_error = e.what();
        return nullptr;
    }
}

bool AudioEngine::save_profile(uint32_t profile_id, const std::filesystem::path& file, std::string* out_error) {
    Lock lock(graph_mutex_);
    ProfileGraph* prof = find_profile(profile_id);
    if (!prof) {
        if (out_error) *out_error = "Profile not found: " + std::to_string(profile_id);
        return false;
    }

    try {
        ProfileSnapshot snap = prof->snapshot();
        json j;
        j["schema_version"] = "1.0.0";

        json meta_j;
        meta_j["name"] = snap.meta.name;
        meta_j["file_path"] = file.string();
        meta_j["canvas_pan_x"] = snap.meta.canvas_pan_x;
        meta_j["canvas_pan_y"] = snap.meta.canvas_pan_y;
        meta_j["canvas_zoom"] = snap.meta.canvas_zoom;
        meta_j["active_plane_view"] = snap.meta.active_plane_view;
        j["profile_metadata"] = meta_j;

        json nodes_j = json::array();
        for (const auto& node : snap.nodes) {
            json nj;
            nj["node_id"] = node.node_id;
            nj["plugin_id"] = node.plugin_id;
            nj["display_name"] = node.display_name;
            nj["ui_state"] = {
                {"pos_x", node.ui.pos_x},
                {"pos_y", node.ui.pos_y},
                {"is_expanded", node.ui.is_expanded}
            };

            json cv = json::object();
            for (const auto& c : node.controls) {
                if (c.control_type == AT_CTRL_LEVEL_METER) continue; // Do not persist level meters
                switch (c.value.type) {
                    case AT_VAL_BOOL:   cv[c.identifier] = c.value.bool_val; break;
                    case AT_VAL_INT:    cv[c.identifier] = c.value.int_val; break;
                    case AT_VAL_FLOAT:  cv[c.identifier] = c.value.float_val; break;
                    case AT_VAL_STRING: cv[c.identifier] = c.value.string_val; break;
                    default: break;
                }
            }
            nj["control_values"] = cv;
            nodes_j.push_back(nj);
        }
        j["nodes"] = nodes_j;

        json audio_conns_j = json::array();
        for (const auto& e : snap.audio_edges) {
            audio_conns_j.push_back({
                {"edge_id", e.edge_id},
                {"source_node_id", e.src_node_id},
                {"target_node_id", e.dst_node_id}
            });
        }
        j["audio_connections"] = audio_conns_j;

        json ctrl_conns_j = json::array();
        for (const auto& ce : snap.control_edges) {
            json cj;
            cj["edge_id"] = ce.edge_id;
            cj["source_node_id"] = ce.src_node_id;
            cj["source_output_slot"] = ce.src_output_port;
            cj["target_node_id"] = ce.dst_node_id;
            cj["target_function_id"] = ce.target_function_id;
            switch (ce.bound_arg.type) {
                case AT_VAL_BOOL:   cj["bound_argument"] = ce.bound_arg.bool_val; break;
                case AT_VAL_INT:    cj["bound_argument"] = ce.bound_arg.int_val; break;
                case AT_VAL_FLOAT:  cj["bound_argument"] = ce.bound_arg.float_val; break;
                case AT_VAL_STRING: cj["bound_argument"] = ce.bound_arg.string_val; break;
                default: break;
            }
            ctrl_conns_j.push_back(cj);
        }
        j["control_connections"] = ctrl_conns_j;

        std::filesystem::create_directories(file.parent_path());
        std::ofstream f(file);
        if (!f.is_open()) {
            if (out_error) *out_error = "Could not open file for writing: " + file.string();
            return false;
        }
        f << j.dump(2);
        prof->set_file_path(file.string());
        return true;
    } catch (const std::exception& e) {
        if (out_error) *out_error = e.what();
        return false;
    }
}

void AudioEngine::process_single_tick() {
    Lock lock(graph_mutex_);
    const uint32_t frames = config_.buffer_size_frames;
    const uint64_t ts = sample_timestamp_.load(std::memory_order_relaxed);
    const uint64_t time_ns = (ts * 1000000000ULL) / std::max<uint32_t>(1u, config_.sample_rate);

    device_manager_.begin_audio_tick(frames);

    for (auto& kv : profiles_) {
        kv.second->process_tick(frames, ts, time_ns);
    }

    device_manager_.end_audio_tick(frames);

    sample_timestamp_.fetch_add(frames, std::memory_order_relaxed);
}

bool AudioEngine::start_realtime_loop() {
    if (running_.load()) return true;
    running_.store(true);
    rt_thread_ = std::thread(&AudioEngine::realtime_worker_loop, this);
    return true;
}

void AudioEngine::stop_realtime_loop() {
    running_.store(false);
    if (rt_thread_.joinable()) {
        rt_thread_.join();
    }
}

void AudioEngine::realtime_worker_loop() {
    enable_realtime_fpu_flags();

    DWORD task_idx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);

    using clock = std::chrono::steady_clock;
    auto next_wake = clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        auto t0 = clock::now();
        process_single_tick();
        auto t1 = clock::now();

        const auto tick_duration = std::chrono::microseconds(
            (static_cast<int64_t>(config_.buffer_size_frames) * 1000000LL) /
            std::max<uint32_t>(1u, config_.sample_rate));

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);
        float load = static_cast<float>(elapsed.count()) / std::max<float>(1.0f, static_cast<float>(tick_duration.count()));
        dsp_load_.store(load, std::memory_order_relaxed);

        next_wake += tick_duration;
        std::this_thread::sleep_until(next_wake);
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}

EngineStats AudioEngine::stats() const {
    EngineStats s;
    s.running = is_running();
    s.frames_processed = total_frames_processed();
    s.dsp_load = dsp_load_.load(std::memory_order_relaxed);
    s.open_devices = device_manager_.open_device_count();
    Lock lock(graph_mutex_);
    s.profile_count = profiles_.size();
    return s;
}

} // namespace at
