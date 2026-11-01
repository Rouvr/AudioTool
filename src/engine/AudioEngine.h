#pragma once
// ============================================================================
// AudioTool (AT) — AudioEngine: headless backend host
//
// Owns the shared DeviceManager (hidden master devices), PluginManager, the
// list of concurrently running ProfileGraphs and the real-time tick thread.
// Every tick: devices.begin -> each active profile (topological order) -> devices.end
// ============================================================================

#include "engine/DeviceManager.h"
#include "engine/EngineTypes.h"
#include "engine/PluginManager.h"
#include "engine/AppAudioRouter.h"
#include "engine/ProfileGraph.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace at {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Ensures %LOCALAPPDATA%\Rouvr\AudioTool exists, writes registry metadata, scans plugins
    bool initialize(const EngineConfig& cfg = {}, std::string* out_error = nullptr);
    void shutdown();

    // Runtime reload of constants without losing graph state (pauses the tick thread briefly)
    bool reload_config(const EngineConfig& new_cfg, std::string* out_error = nullptr);
    EngineConfig config() const;

    // Loads/stores EngineConfig in %LOCALAPPDATA%\Rouvr\AudioTool\config.json
    static EngineConfig load_config_file();
    static bool         save_config_file(const EngineConfig& cfg, std::string* out_error = nullptr);

    DeviceManager& device_manager() noexcept { return device_manager_; }
    PluginManager& plugin_manager() noexcept { return plugin_manager_; }
    AppAudioRouter& audio_router() noexcept { return audio_router_; }

    // Re-scans %LOCALAPPDATA%\Rouvr\AudioTool\plugins (new DLLs; replaces DLLs without live instances)
    size_t                  rescan_plugins(std::vector<std::string>* out_errors = nullptr);
    std::vector<PluginInfo> list_plugins() const;

    // --- Profiles ---
    ProfileGraph*         create_profile(const std::string& name);
    ProfileGraph*         load_profile(const std::filesystem::path& file, std::string* out_error = nullptr);
    bool                  save_profile(uint32_t profile_id, const std::filesystem::path& file, std::string* out_error = nullptr);
    bool                  close_profile(uint32_t profile_id);
    ProfileGraph*         find_profile(uint32_t profile_id);
    std::vector<uint32_t> profile_ids() const;

    // --- Execution ---
    void process_single_tick();
    bool start_realtime_loop();
    void stop_realtime_loop();
    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    uint64_t    total_frames_processed() const noexcept { return sample_timestamp_.load(std::memory_order_relaxed); }
    EngineStats stats() const;

    // Graph mutex (recursive). Held during every tick and every graph mutation.
    std::recursive_mutex& mutex() const noexcept { return graph_mutex_; }

    // Used by ProfileGraph (mutex must be held)
    const EngineConfig& config_unlocked() const noexcept { return config_; }

private:
    void realtime_worker_loop();

    mutable std::recursive_mutex                      graph_mutex_;
    EngineConfig                                      config_;
    DeviceManager                                     device_manager_;
    PluginManager                                     plugin_manager_;
    AppAudioRouter                                    audio_router_;
    std::map<uint32_t, std::unique_ptr<ProfileGraph>> profiles_;
    uint32_t                                          next_profile_id_ = 1;

    std::atomic<uint64_t>                             sample_timestamp_{0};
    std::atomic<float>                                dsp_load_{0.0f};
    std::atomic<bool>                                 running_{false};
    std::thread                                       rt_thread_;
};

} // namespace at

