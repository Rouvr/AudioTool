#pragma once
// ============================================================================
// AudioTool (AT) — AppAudioRouter: Per-application audio output management
//
// Fast in-app routing of active audio applications to WASAPI output endpoints
// via Windows Core Audio session enumeration & AudioPolicyConfig COM activation.
// Supports per-profile persistence, standalone routing presets, and global
// original configuration tracking with safe automatic restoration upon exit.
// ============================================================================

#include "engine/DeviceManager.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace at {

struct AudioAppSession {
    uint32_t    pid = 0;
    std::string process_name;
    std::string display_name;
    std::string current_device_id;   // endpoint ID or empty/default
    std::string current_device_name; // friendly name of current output device
    bool        is_system_sound = false;
};

struct RoutingPresetInfo {
    std::string name;
    std::string file_path;
    std::map<std::string, std::string> mapping; // process_name -> endpoint_id
};

class AppAudioRouter {
public:
    AppAudioRouter();
    ~AppAudioRouter();

    AppAudioRouter(const AppAudioRouter&) = delete;
    AppAudioRouter& operator=(const AppAudioRouter&) = delete;

    // Enumerates all active audio sessions / applications producing output
    std::vector<AudioAppSession> enumerate_apps(const std::vector<AudioDeviceInfo>& available_devices);

    // Routes an application's audio output to endpoint_id.
    // If endpoint_id is empty, "(default)", or "default", restores system default endpoint.
    bool set_app_output_device(uint32_t pid, const std::string& process_name, const std::string& endpoint_id);

    // Applies an entire routing map (process_name -> endpoint_id) to active processes
    void apply_routing_map(const std::map<std::string, std::string>& routing);

    // Global original routing tracking (%LOCALAPPDATA%\Rouvr\AudioTool\app_routing_original.json)
    void capture_original_if_missing(const std::string& process_name, const std::string& current_device_id);
    void revert_all_to_original();
    std::map<std::string, std::string> get_original_routing() const;

    // Presets management (%LOCALAPPDATA%\Rouvr\AudioTool\routing_presets\*.json)
    std::vector<std::string> list_presets() const;
    bool save_preset(const std::string& name, const std::map<std::string, std::string>& mapping, std::string* out_err = nullptr);
    bool load_preset(const std::string& name, std::map<std::string, std::string>* out_mapping, std::string* out_err = nullptr);
    bool delete_preset(const std::string& name, std::string* out_err = nullptr);

    // Revert upon exit configuration
    bool get_revert_on_exit() const;
    void set_revert_on_exit(bool revert);

    void load_state();
    void save_state();

private:
    bool init_com_factory();
    void release_com_factory();
    bool set_persisted_endpoint(uint32_t pid, const std::wstring& endpoint_id_w);

    mutable std::mutex                  router_mutex_;
    std::map<std::string, std::string>  original_routing_;
    bool                                revert_on_exit_ = true;

    // Cached COM interface pointer
    void* audio_policy_factory_ = nullptr;
    bool  factory_attempted_    = false;
};

} // namespace at
