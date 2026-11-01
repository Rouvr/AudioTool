#pragma once
// ============================================================================
// AudioTool (AT) — Application Paths (%LOCALAPPDATA%\Rouvr\AudioTool)
//                  & Windows Registry Integration (HKCU\Software\Rouvr\AudioTool)
// ============================================================================

#include <filesystem>
#include <string>

namespace at {

struct RegistryConfigInfo {
    std::filesystem::path install_path;
    std::filesystem::path local_data_path;
    std::string           version;
    std::string           build_id;
};

class AppPathsAndRegistry {
public:
    static constexpr const wchar_t* kRegistrySubKey = L"Software\\Rouvr\\AudioTool";

    // Must be called at the very start of main() to harden DLL search order
    static void harden_dll_search_order();

    // Resolves %LOCALAPPDATA%\Rouvr\AudioTool
    static std::filesystem::path get_local_data_root();

    // User-configurable subdirectories inside %LOCALAPPDATA%\Rouvr\AudioTool
    static std::filesystem::path get_profiles_dir();
    static std::filesystem::path get_plugins_dir();
    static std::filesystem::path get_plugin_shadow_cache_dir();
    static std::filesystem::path get_config_file_path();
    static std::filesystem::path get_logs_dir();
    static std::filesystem::path get_routing_presets_dir();
    static std::filesystem::path get_routing_original_file_path();
    static std::filesystem::path get_routing_config_file_path();

    // Resolves the directory containing the currently running executable
    static std::filesystem::path get_executable_install_dir();

    // Ensures all %LOCALAPPDATA%\Rouvr\AudioTool subdirectories exist on disk
    static bool ensure_directories_exist(std::string* out_error = nullptr);

    // Writes InstallPath, LocalDataPath, Version, and BuildId to HKCU\Software\Rouvr\AudioTool
    static bool write_registry_metadata(std::string* out_error = nullptr);

    // Reads InstallPath, LocalDataPath, Version, and BuildId from HKCU\Software\Rouvr\AudioTool
    static bool read_registry_metadata(RegistryConfigInfo& out_info, std::string* out_error = nullptr);

    // Checks if a file has an Internet Mark-of-the-Web (Zone.Identifier ADS with ZoneId >= 3)
    static bool has_internet_mark_of_the_web(const std::filesystem::path& file_path);
};

} // namespace at

