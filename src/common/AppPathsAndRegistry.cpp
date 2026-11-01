#include "common/AppPathsAndRegistry.h"
#include "at_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <vector>

namespace at {

namespace {

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), needed);
    return result;
}

std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

bool set_reg_sz(HKEY hKey, const wchar_t* value_name, const std::wstring& value) {
    const BYTE* data_ptr = reinterpret_cast<const BYTE*>(value.c_str());
    DWORD byte_len = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(hKey, value_name, 0, REG_SZ, data_ptr, byte_len) == ERROR_SUCCESS;
}

bool get_reg_sz(HKEY hKey, const wchar_t* value_name, std::wstring& out_val) {
    DWORD type = 0;
    DWORD byte_len = 0;
    LSTATUS status = RegQueryValueExW(hKey, value_name, nullptr, &type, nullptr, &byte_len);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || byte_len == 0) {
        return false;
    }
    std::vector<wchar_t> buf((byte_len / sizeof(wchar_t)) + 1, L'\0');
    status = RegQueryValueExW(hKey, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &byte_len);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    out_val = buf.data();
    return true;
}

} // namespace

void AppPathsAndRegistry::harden_dll_search_order() {
    // Restrict implicit DLL loading to the application directory and System32 only.
    // Prevents CWD / untrusted PATH DLL hijacking.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

std::filesystem::path AppPathsAndRegistry::get_local_data_root() {
    PWSTR known_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known_path)) && known_path) {
        std::filesystem::path base(known_path);
        CoTaskMemFree(known_path);
        return base / L"Rouvr" / L"AudioTool";
    }

    // Fallback to environment variable
    wchar_t env_buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", env_buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(env_buf) / L"Rouvr" / L"AudioTool";
    }
    return std::filesystem::current_path() / L"Rouvr_AudioTool_LocalData";
}

std::filesystem::path AppPathsAndRegistry::get_profiles_dir() {
    return get_local_data_root() / L"profiles";
}

std::filesystem::path AppPathsAndRegistry::get_plugins_dir() {
    return get_local_data_root() / L"plugins";
}

std::filesystem::path AppPathsAndRegistry::get_plugin_shadow_cache_dir() {
    return get_local_data_root() / L"cache" / L"plugin_shadow";
}

std::filesystem::path AppPathsAndRegistry::get_config_file_path() {
    return get_local_data_root() / L"config.json";
}

std::filesystem::path AppPathsAndRegistry::get_executable_install_dir() {
    wchar_t module_path[MAX_PATH * 4] = {};
    DWORD len = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
    if (len > 0) {
        return std::filesystem::path(module_path).parent_path();
    }
    return std::filesystem::current_path();
}

bool AppPathsAndRegistry::ensure_directories_exist(std::string* out_error) {
    try {
        const auto root = get_local_data_root();
        const auto profiles = get_profiles_dir();
        const auto plugins = get_plugins_dir();
        const auto cache = get_plugin_shadow_cache_dir();

        std::filesystem::create_directories(root);
        std::filesystem::create_directories(profiles);
        std::filesystem::create_directories(plugins / L"Audio" / L"Sources");
        std::filesystem::create_directories(plugins / L"Audio" / L"Processing");
        std::filesystem::create_directories(plugins / L"Audio" / L"Outputs");
        std::filesystem::create_directories(plugins / L"Control" / L"Triggers");
        std::filesystem::create_directories(cache);
        return true;
    } catch (const std::exception& ex) {
        if (out_error) *out_error = ex.what();
        return false;
    }
}

bool AppPathsAndRegistry::write_registry_metadata(std::string* out_error) {
    HKEY hKey = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRegistrySubKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE | KEY_READ,
        nullptr,
        &hKey,
        &disposition);

    if (status != ERROR_SUCCESS || !hKey) {
        if (out_error) *out_error = "Failed to open/create HKCU\\Software\\Rouvr\\AudioTool registry key.";
        return false;
    }

    const std::wstring install_path = get_executable_install_dir().wstring();
    const std::wstring local_data_path = get_local_data_root().wstring();
    const std::wstring version_w = utf8_to_wide(AT_VERSION_STRING);
    const std::wstring build_id_w = utf8_to_wide(AT_BUILD_ID);

    bool ok = true;
    ok = set_reg_sz(hKey, L"InstallPath", install_path) && ok;
    ok = set_reg_sz(hKey, L"LocalDataPath", local_data_path) && ok;
    ok = set_reg_sz(hKey, L"Version", version_w) && ok;
    ok = set_reg_sz(hKey, L"BuildId", build_id_w) && ok;

    RegCloseKey(hKey);
    if (!ok && out_error) {
        *out_error = "Failed to write one or more values under HKCU\\Software\\Rouvr\\AudioTool.";
    }
    return ok;
}

bool AppPathsAndRegistry::read_registry_metadata(RegistryConfigInfo& out_info, std::string* out_error) {
    HKEY hKey = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistrySubKey, 0, KEY_READ, &hKey);
    if (status != ERROR_SUCCESS || !hKey) {
        if (out_error) *out_error = "HKCU\\Software\\Rouvr\\AudioTool key does not exist yet.";
        return false;
    }

    std::wstring install_w, local_w, ver_w, bld_w;
    bool ok = true;
    ok = get_reg_sz(hKey, L"InstallPath", install_w) && ok;
    ok = get_reg_sz(hKey, L"LocalDataPath", local_w) && ok;
    ok = get_reg_sz(hKey, L"Version", ver_w) && ok;
    ok = get_reg_sz(hKey, L"BuildId", bld_w) && ok;

    RegCloseKey(hKey);

    if (!ok) {
        if (out_error) *out_error = "Incomplete registry values in HKCU\\Software\\Rouvr\\AudioTool.";
        return false;
    }

    out_info.install_path = install_w;
    out_info.local_data_path = local_w;
    out_info.version = wide_to_utf8(ver_w);
    out_info.build_id = wide_to_utf8(bld_w);
    return true;
}

bool AppPathsAndRegistry::has_internet_mark_of_the_web(const std::filesystem::path& file_path) {
    // Check NTFS Alternate Data Stream "<path>:Zone.Identifier"
    std::wstring ads_path = file_path.wstring() + L":Zone.Identifier";
    HANDLE hFile = CreateFileW(
        ads_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        // No Zone.Identifier stream -> trusted local file
        return false;
    }

    char buf[512] = {};
    DWORD bytes_read = 0;
    BOOL read_ok = ReadFile(hFile, buf, sizeof(buf) - 1, &bytes_read, nullptr);
    CloseHandle(hFile);

    if (!read_ok || bytes_read == 0) {
        return false;
    }

    std::string content(buf, bytes_read);
    // ZoneId=3 (Internet) or ZoneId=4 (Restricted Sites)
    if (content.find("ZoneId=3") != std::string::npos ||
        content.find("ZoneId=4") != std::string::npos) {
        return true;
    }
    return false;
}

} // namespace at

