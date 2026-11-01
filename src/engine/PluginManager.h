#pragma once
// ============================================================================
// AudioTool (AT) — Dynamic DLL Plugin Manager (Shadow-Copy & ABI Verification)
// ============================================================================

#include "at_sdk/at_abi.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace at {

struct RegisteredPlugin {
    std::string                  plugin_id;
    std::string                  display_name;
    std::string                  relative_folder_path; // Relative to %LOCALAPPDATA%/Rouvr/AudioTool/plugins
    std::filesystem::path        original_dll_path;
    std::filesystem::path        shadow_dll_path;
    void*                        module_handle = nullptr; // HMODULE
    const AT_PluginExportBundle* bundle        = nullptr;
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Recursively scans %LOCALAPPDATA%\Rouvr\AudioTool\plugins for node DLLs
    size_t scan_plugins_directory(const std::filesystem::path& plugins_root_dir, std::vector<std::string>* out_errors = nullptr);

    // Loads a single plugin DLL via shadow-copy into %LOCALAPPDATA%\Rouvr\AudioTool\cache\plugin_shadow
    bool load_plugin_dll(
        const std::filesystem::path& dll_path,
        const std::filesystem::path& plugins_root_dir,
        std::string* out_error = nullptr);

    // Unloads all loaded plugin DLLs
    void unload_all();

    // Lookup registered plugin by plugin_id (e.g. "at.builtin.audio.virtual_input")
    const RegisteredPlugin* find_plugin(const std::string& plugin_id) const;

    // Returns list of all registered plugins
    std::vector<const RegisteredPlugin*> list_plugins() const;

    // Predicate consulted before an already-loaded plugin_id is replaced by a newer DLL
    // (must return false while live node instances of that plugin exist).
    void set_replace_predicate(std::function<bool(const std::string&)> pred) { can_replace_ = std::move(pred); }

private:
    std::function<bool(const std::string&)>                         can_replace_;
    mutable std::mutex                                              mutex_;
    std::unordered_map<std::string, std::unique_ptr<RegisteredPlugin>> plugins_;
    uint64_t                                                        shadow_counter_ = 0;
};

} // namespace at

