#include "engine/PluginManager.h"
#include "common/AppPathsAndRegistry.h"
#include "common/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <sstream>

namespace at {

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    unload_all();
}

size_t PluginManager::scan_plugins_directory(
    const std::filesystem::path& plugins_root_dir,
    std::vector<std::string>* out_errors) {
    if (!std::filesystem::exists(plugins_root_dir)) {
        return 0;
    }

    size_t loaded_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(plugins_root_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
        if (ext == L".dll") {
            std::string err;
            if (load_plugin_dll(entry.path(), plugins_root_dir, &err)) {
                ++loaded_count;
            } else if (out_errors && !err.empty()) {
                out_errors->push_back(err);
            }
        }
    }
    return loaded_count;
}

bool PluginManager::load_plugin_dll(
    const std::filesystem::path& dll_path,
    const std::filesystem::path& plugins_root_dir,
    std::string* out_error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!std::filesystem::exists(dll_path)) {
        if (out_error) *out_error = "Plugin DLL does not exist: " + dll_path.string();
        return false;
    }

    // Security check: Reject unsigned DLLs carrying Internet Mark-of-the-Web (ZoneId=3/4)
    if (AppPathsAndRegistry::has_internet_mark_of_the_web(dll_path)) {
        if (out_error) {
            *out_error = "Security policy blocked plugin DLL with Internet Mark-of-the-Web (Zone.Identifier): " + dll_path.string();
        }
        AT_LOG_WARN("SECURITY", "Blocked untrusted plugin DLL with Internet Mark-of-the-Web: %s", dll_path.string().c_str());
        return false;
    }

    // Shadow-copy DLL into %LOCALAPPDATA%\Rouvr\AudioTool\cache\plugin_shadow so original stays unlocked
    const auto shadow_dir = AppPathsAndRegistry::get_plugin_shadow_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(shadow_dir, ec);

    ++shadow_counter_;
    std::ostringstream shadow_name;
    shadow_name << dll_path.stem().string()
                << "_pid" << GetCurrentProcessId()
                << "_" << shadow_counter_ << ".dll";
    const std::filesystem::path shadow_path = shadow_dir / shadow_name.str();

    if (!std::filesystem::copy_file(dll_path, shadow_path, std::filesystem::copy_options::overwrite_existing, ec)) {
        if (out_error) *out_error = "Failed to shadow-copy plugin DLL to: " + shadow_path.string();
        AT_LOG_ERROR("FILESYSTEM", "Failed to shadow-copy plugin DLL '%s' -> '%s': %s",
                     dll_path.string().c_str(), shadow_path.string().c_str(), ec.message().c_str());
        return false;
    }

    const std::filesystem::path abs_shadow = std::filesystem::absolute(shadow_path, ec);
    HMODULE hMod = LoadLibraryExW(abs_shadow.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hMod) {
        DWORD gle = GetLastError();
        if (out_error) *out_error = "LoadLibraryExW failed for: " + abs_shadow.string() + " (GLE=" + std::to_string(gle) + ")";
        AT_LOG_ERROR("WINAPI", "LoadLibraryExW failed for '%s': GLE=%lu", abs_shadow.string().c_str(), gle);
        return false;
    }

    auto entry_fn = reinterpret_cast<AT_GetPluginBundleFn>(
        GetProcAddress(hMod, AT_PLUGIN_ENTRY_SYMBOL_NAME));
    if (!entry_fn) {
        FreeLibrary(hMod);
        std::filesystem::remove(shadow_path, ec);
        if (out_error) *out_error = "DLL missing at_get_plugin_bundle symbol: " + dll_path.string();
        AT_LOG_ERROR("PLUGIN", "Plugin DLL '%s' missing exported entry symbol '%s'", dll_path.string().c_str(), AT_PLUGIN_ENTRY_SYMBOL_NAME);
        return false;
    }

    const AT_PluginExportBundle* bundle = entry_fn(AT_ABI_VERSION);
    if (!bundle || bundle->abi_version != AT_ABI_VERSION || !bundle->descriptor || !bundle->vtable) {
        FreeLibrary(hMod);
        std::filesystem::remove(shadow_path, ec);
        if (out_error) *out_error = "Incompatible AT_ABI_VERSION in plugin: " + dll_path.string();
        AT_LOG_WARN("PLUGIN", "Plugin '%s' ABI incompatible (expected 0x%08X, got 0x%08X)",
                    dll_path.string().c_str(), AT_ABI_VERSION, bundle ? bundle->abi_version : 0);
        return false;
    }

    std::string plugin_id = bundle->descriptor->plugin_id ? bundle->descriptor->plugin_id : "";
    if (plugin_id.empty()) {
        FreeLibrary(hMod);
        std::filesystem::remove(shadow_path, ec);
        if (out_error) *out_error = "Plugin returned empty plugin_id: " + dll_path.string();
        return false;
    }

    // If an older version of this plugin_id was loaded, replace it - but only if no live node
    // instances still execute code from the old module (FreeLibrary would crash them).
    auto existing_it = plugins_.find(plugin_id);
    if (existing_it != plugins_.end()) {
        if (can_replace_ && !can_replace_(plugin_id)) {
            FreeLibrary(hMod);
            std::filesystem::remove(shadow_path, ec);
            if (out_error) *out_error = "Plugin '" + plugin_id + "' kept: live instances exist (delete them to hot-reload).";
            return false;
        }
        if (existing_it->second->module_handle) {
            FreeLibrary(static_cast<HMODULE>(existing_it->second->module_handle));
        }
        std::filesystem::remove(existing_it->second->shadow_dll_path, ec);
        plugins_.erase(existing_it);
    }

    std::filesystem::path rel_folder = std::filesystem::relative(dll_path.parent_path(), plugins_root_dir, ec);
    std::string rel_folder_str = rel_folder.generic_string();
    if (rel_folder_str == ".") rel_folder_str.clear();

    auto reg = std::make_unique<RegisteredPlugin>();
    reg->plugin_id            = plugin_id;
    reg->display_name         = bundle->descriptor->display_name ? bundle->descriptor->display_name : plugin_id;
    reg->relative_folder_path = rel_folder_str;
    reg->original_dll_path    = dll_path;
    reg->shadow_dll_path      = shadow_path;
    reg->module_handle        = hMod;
    reg->bundle               = bundle;

    std::string disp_name = reg->display_name;
    plugins_[plugin_id] = std::move(reg);
    AT_LOG_INFO("PLUGIN", "Registered plugin '%s' [%s] from %s", plugin_id.c_str(), disp_name.c_str(), dll_path.string().c_str());
    return true;
}

void PluginManager::unload_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    for (auto& kv : plugins_) {
        if (kv.second->module_handle) {
            FreeLibrary(static_cast<HMODULE>(kv.second->module_handle));
            kv.second->module_handle = nullptr;
        }
        if (!kv.second->shadow_dll_path.empty()) {
            std::filesystem::remove(kv.second->shadow_dll_path, ec);
        }
    }
    plugins_.clear();
}

const RegisteredPlugin* PluginManager::find_plugin(const std::string& plugin_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plugins_.find(plugin_id);
    return (it != plugins_.end()) ? it->second.get() : nullptr;
}

std::vector<const RegisteredPlugin*> PluginManager::list_plugins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const RegisteredPlugin*> out;
    out.reserve(plugins_.size());
    for (const auto& kv : plugins_) {
        out.push_back(kv.second.get());
    }
    return out;
}

} // namespace at
