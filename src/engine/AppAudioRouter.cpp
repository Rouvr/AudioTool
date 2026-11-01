// ============================================================================
// AudioTool (AT) — AppAudioRouter Implementation
// ============================================================================

#include "engine/AppAudioRouter.h"
#include "common/AppPathsAndRegistry.h"
#include "common/Logger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <roapi.h>
#include <winstring.h>
#include <tlhelp32.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace at {

namespace {

// IID ab3d4648-e242-459f-b02f-541c70306324 (Windows 11 / 21390+)
static const IID IID_AudioPolicyConfig_Win11 = {
    0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 }
};

// IID 2a59116d-6c4f-45e0-a74f-707e3fef9258 (Windows 10 downlevel)
static const IID IID_AudioPolicyConfig_Win10 = {
    0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 }
};

std::string utf8_encode(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring utf8_decode(const std::string& str) {
    if (str.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), needed);
    return result;
}

// Formats an endpoint ID into the fully-qualified SWD#MMDEVAPI path required by AudioPolicyConfig:
// \\?\SWD#MMDEVAPI#{endpoint_id}#{e6327cad-dcec-4949-ae8a-991e976a79d2}
std::wstring make_full_device_id(const std::wstring& endpoint_id, EDataFlow flow) {
    if (endpoint_id.empty() || endpoint_id == L"(default)" || endpoint_id == L"default") {
        return L"";
    }
    const wchar_t* prefix = L"\\\\?\\SWD#MMDEVAPI#";
    const wchar_t* postfix = (flow == eRender)
        ? L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"
        : L"#{e2ef81be-33fa-4800-9670-1cd474972c3f}";
    return std::wstring(prefix) + endpoint_id + postfix;
}

// Extracts the core MMDevice ID ({0.0.0.00000000}.{GUID}) from a full SWD path if needed:
std::string extract_endpoint_id_from_full(const std::wstring& full_id) {
    if (full_id.empty()) return "";
    size_t start = full_id.find(L"{0.0.");
    if (start == std::wstring::npos) {
        // Fallback: search for first opening brace
        start = full_id.find(L'{');
        if (start == std::wstring::npos) return "";
    }
    size_t end = full_id.find(L"}#", start);
    if (end == std::wstring::npos) {
        end = full_id.rfind(L'}');
        if (end != std::wstring::npos) end += 1;
    } else {
        end += 1;
    }
    return (end > start) ? utf8_encode(full_id.substr(start, end - start)) : "";
}

std::vector<DWORD> find_all_pids_for_process(const std::string& process_name) {
    std::vector<DWORD> pids;
    std::wstring target_w = utf8_decode(process_name);
    // Lowercase for case-insensitive matching
    std::transform(target_w.begin(), target_w.end(), target_w.begin(), ::towlower);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            std::wstring exe_w = pe.szExeFile;
            std::transform(exe_w.begin(), exe_w.end(), exe_w.begin(), ::towlower);
            if (exe_w == target_w) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pids;
}

} // namespace

AppAudioRouter::AppAudioRouter() {
    load_state();
}

AppAudioRouter::~AppAudioRouter() {
    if (revert_on_exit_) {
        revert_all_to_original();
    }
    release_com_factory();
    save_state();
}

bool AppAudioRouter::init_com_factory() {
    if (audio_policy_factory_) return true;
    if (factory_attempted_) return false;

    factory_attempted_ = true;
    AT_LOG_DEBUG("COM/POLICY", "Attempting AudioPolicyConfig COM activation factory acquisition...");

    typedef HRESULT (WINAPI *pfnRoGetActivationFactory)(HSTRING, REFIID, void**);
    typedef HRESULT (WINAPI *pfnWindowsCreateString)(PCNZWCH, UINT32, HSTRING*);
    typedef HRESULT (WINAPI *pfnWindowsDeleteString)(HSTRING);

    HMODULE hComBase = LoadLibraryW(L"combase.dll");
    if (!hComBase) {
        AT_LOG_ERROR("WINAPI", "Failed to load combase.dll: GetLastError=%lu", GetLastError());
        return false;
    }

    auto pRoGetActivationFactory = (pfnRoGetActivationFactory)GetProcAddress(hComBase, "RoGetActivationFactory");
    auto pWindowsCreateString = (pfnWindowsCreateString)GetProcAddress(hComBase, "WindowsCreateString");
    auto pWindowsDeleteString = (pfnWindowsDeleteString)GetProcAddress(hComBase, "WindowsDeleteString");

    if (!pRoGetActivationFactory || !pWindowsCreateString || !pWindowsDeleteString) {
        AT_LOG_ERROR("WINAPI", "Failed to resolve WinRT string/factory entry points in combase.dll");
        return false;
    }

    const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING hsClass = nullptr;
    HRESULT hr = pWindowsCreateString(className, static_cast<UINT32>(wcslen(className)), &hsClass);
    if (FAILED(hr) || !hsClass) {
        AT_LOG_ERROR("WINAPI", "WindowsCreateString failed for AudioPolicyConfig: hr=0x%08X", hr);
        return false;
    }

    void* factory = nullptr;
    // Try Windows 11 / 21390+ first
    hr = pRoGetActivationFactory(hsClass, IID_AudioPolicyConfig_Win11, &factory);
    AT_LOG_DEBUG("COM/POLICY", "RoGetActivationFactory (Win11 IID): hr=0x%08X, factory=%p", hr, factory);

    if (FAILED(hr) || !factory) {
        // Fallback to downlevel Windows 10
        hr = pRoGetActivationFactory(hsClass, IID_AudioPolicyConfig_Win10, &factory);
        AT_LOG_DEBUG("COM/POLICY", "RoGetActivationFactory fallback (Win10 downlevel IID): hr=0x%08X, factory=%p", hr, factory);
    }

    pWindowsDeleteString(hsClass);

    if (SUCCEEDED(hr) && factory) {
        audio_policy_factory_ = factory;
        AT_LOG_INFO("COM/POLICY", "Successfully initialized Windows.Media.Internal.AudioPolicyConfig interface at %p", audio_policy_factory_);
        return true;
    }

    AT_LOG_WARN("COM/POLICY", "Could not activate AudioPolicyConfig interface (unsupported Windows build or restricted security boundary)");
    return false;
}

void AppAudioRouter::release_com_factory() {
    if (audio_policy_factory_) {
        AT_LOG_DEBUG("COM/POLICY", "Releasing AudioPolicyConfig interface (%p)", audio_policy_factory_);
        auto* obj = static_cast<IUnknown*>(audio_policy_factory_);
        obj->Release();
        audio_policy_factory_ = nullptr;
    }
    factory_attempted_ = false;
}

bool AppAudioRouter::set_persisted_endpoint(uint32_t pid, const std::wstring& endpoint_id_w) {
    if (!init_com_factory()) {
        AT_LOG_WARN("COM/POLICY", "Cannot set persisted endpoint: factory not initialized");
        return false;
    }
    if (!audio_policy_factory_) {
        AT_LOG_ERROR("COM/POLICY", "audio_policy_factory_ is null during endpoint dispatch");
        return false;
    }

    HMODULE hComBase = GetModuleHandleW(L"combase.dll");
    if (!hComBase) hComBase = LoadLibraryW(L"combase.dll");
    typedef HRESULT (WINAPI *pfnWindowsCreateString)(PCNZWCH, UINT32, HSTRING*);
    typedef HRESULT (WINAPI *pfnWindowsDeleteString)(HSTRING);
    auto pWindowsCreateString = reinterpret_cast<pfnWindowsCreateString>(GetProcAddress(hComBase, "WindowsCreateString"));
    auto pWindowsDeleteString = reinterpret_cast<pfnWindowsDeleteString>(GetProcAddress(hComBase, "WindowsDeleteString"));

    if (!pWindowsCreateString || !pWindowsDeleteString) {
        AT_LOG_ERROR("COM/POLICY", "Failed to resolve WinRT string functions in combase.dll");
        return false;
    }

    std::wstring full_dev_id = make_full_device_id(endpoint_id_w, eRender);
    HSTRING hsTarget = nullptr;
    HRESULT hrStr = pWindowsCreateString(full_dev_id.c_str(), static_cast<UINT32>(full_dev_id.length()), &hsTarget);
    if (FAILED(hrStr)) {
        AT_LOG_ERROR("COM/POLICY", "WindowsCreateString failed for '%s': hr=0x%08X",
                     utf8_encode(full_dev_id).c_str(), hrStr);
        return false;
    }

    AT_LOG_DEBUG("COM/POLICY", "Calling AudioPolicyConfig Slot[25] SetPersistedDefaultAudioEndpoint(PID=%u, device='%s')",
                 pid, full_dev_id.empty() ? "[System Default]" : utf8_encode(full_dev_id).c_str());

    void** vtable = *reinterpret_cast<void***>(audio_policy_factory_);
    // Virtual Method Table Slot 25: SetPersistedDefaultAudioEndpoint(pid, flow, role, hsDevice)
    typedef HRESULT (STDMETHODCALLTYPE *pfnSetPersisted)(void*, uint32_t, uint32_t, uint32_t, HSTRING);
    auto fnSetPersisted = reinterpret_cast<pfnSetPersisted>(vtable[25]);

    // Route for both eConsole (0) and eMultimedia (1) roles
    HRESULT hr1 = fnSetPersisted(audio_policy_factory_, pid, eRender, eConsole, hsTarget);
    HRESULT hr2 = fnSetPersisted(audio_policy_factory_, pid, eRender, eMultimedia, hsTarget);

    pWindowsDeleteString(hsTarget);

    if (FAILED(hr1) && FAILED(hr2)) {
        AT_LOG_WARN("COM/POLICY", "SetPersistedDefaultAudioEndpoint failed for PID=%u (eConsole: 0x%08X, eMultimedia: 0x%08X)",
                    pid, hr1, hr2);
        return false;
    }

    AT_LOG_INFO("COM/POLICY", "Successfully set audio endpoint for PID %u (eConsole: 0x%08X, eMultimedia: 0x%08X)",
                pid, hr1, hr2);
    return true;
}

std::string AppAudioRouter::get_persisted_endpoint(uint32_t pid) {
    if (!init_com_factory() || !audio_policy_factory_) return "";

    void** vtable = *reinterpret_cast<void***>(audio_policy_factory_);
    // Virtual Method Table Slot 26: GetPersistedDefaultAudioEndpoint(pid, flow, role, HSTRING*)
    typedef HRESULT (STDMETHODCALLTYPE *pfnGetPersisted)(void*, uint32_t, uint32_t, uint32_t, HSTRING*);
    auto fnGetPersisted = reinterpret_cast<pfnGetPersisted>(vtable[26]);

    HMODULE hComBase = GetModuleHandleW(L"combase.dll");
    if (!hComBase) hComBase = LoadLibraryW(L"combase.dll");
    typedef HRESULT (WINAPI *pfnWindowsDeleteString)(HSTRING);
    typedef PCWSTR (WINAPI *pfnWindowsGetStringRawBuffer)(HSTRING, UINT32*);
    auto pWindowsDeleteString = reinterpret_cast<pfnWindowsDeleteString>(GetProcAddress(hComBase, "WindowsDeleteString"));
    auto pWindowsGetStringRawBuffer = reinterpret_cast<pfnWindowsGetStringRawBuffer>(GetProcAddress(hComBase, "WindowsGetStringRawBuffer"));

    if (!pWindowsDeleteString || !pWindowsGetStringRawBuffer) return "";

    HSTRING hsResult = nullptr;
    HRESULT hr = fnGetPersisted(audio_policy_factory_, pid, eRender, eMultimedia, &hsResult);
    if (FAILED(hr) || !hsResult) {
        hr = fnGetPersisted(audio_policy_factory_, pid, eRender, eConsole, &hsResult);
    }

    if (SUCCEEDED(hr) && hsResult) {
        UINT32 len = 0;
        PCWSTR raw = pWindowsGetStringRawBuffer(hsResult, &len);
        std::wstring full_w = (raw && len > 0) ? std::wstring(raw, len) : L"";
        pWindowsDeleteString(hsResult);
        if (!full_w.empty()) {
            return extract_endpoint_id_from_full(full_w);
        }
    }
    return "";
}

std::vector<AudioAppSession> AppAudioRouter::enumerate_apps(const std::vector<AudioDeviceInfo>& available_devices) {
    std::lock_guard<std::mutex> lock(router_mutex_);

    std::map<std::string, std::string> endpoint_to_name;
    for (const auto& dev : available_devices) {
        if (dev.is_output_render) {
            endpoint_to_name[dev.endpoint_id] = dev.friendly_name;
        }
    }

    std::vector<AudioAppSession> result;
    std::map<std::string, size_t> process_to_index;

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        AT_LOG_ERROR("WINAPI", "CoCreateInstance(MMDeviceEnumerator) failed: hr=0x%08X", hr);
        return result;
    }

    IMMDeviceCollection* pCol = nullptr;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
    if (FAILED(hr) || !pCol) {
        AT_LOG_ERROR("WINAPI", "EnumAudioEndpoints(eRender) failed: hr=0x%08X", hr);
        pEnum->Release();
        return result;
    }

    UINT devCount = 0;
    pCol->GetCount(&devCount);
    AT_LOG_DEBUG("WINAPI", "Enumerating audio sessions across %u active render devices...", devCount);

    for (UINT i = 0; i < devCount; ++i) {
        IMMDevice* pDev = nullptr;
        if (FAILED(pCol->Item(i, &pDev)) || !pDev) continue;

        LPWSTR wDevId = nullptr;
        pDev->GetId(&wDevId);
        std::string devId = wDevId ? utf8_encode(wDevId) : "";
        if (wDevId) CoTaskMemFree(wDevId);

        std::string friendlyName = devId;
        auto fit = endpoint_to_name.find(devId);
        if (fit != endpoint_to_name.end()) {
            friendlyName = fit->second;
        } else {
            IPropertyStore* pProps = nullptr;
            if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR && varName.pwszVal) {
                    friendlyName = utf8_encode(varName.pwszVal);
                }
                PropVariantClear(&varName);
                pProps->Release();
            }
        }

        IAudioSessionManager2* pMgr = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&pMgr)) && pMgr) {
            IAudioSessionEnumerator* pSessEnum = nullptr;
            if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessEnum)) && pSessEnum) {
                int sessCount = 0;
                pSessEnum->GetCount(&sessCount);
                for (int s = 0; s < sessCount; ++s) {
                    IAudioSessionControl* pCtrl = nullptr;
                    if (FAILED(pSessEnum->GetSession(s, &pCtrl)) || !pCtrl) continue;

                    AudioSessionState st;
                    if (SUCCEEDED(pCtrl->GetState(&st)) && st == AudioSessionStateExpired) {
                        pCtrl->Release();
                        continue;
                    }

                    IAudioSessionControl2* pCtrl2 = nullptr;
                    if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pCtrl2)) && pCtrl2) {
                        DWORD pid = 0;
                        pCtrl2->GetProcessId(&pid);
                        bool isSys = (pCtrl2->IsSystemSoundsSession() == S_OK);

                        LPWSTR dispNameW = nullptr;
                        pCtrl2->GetDisplayName(&dispNameW);
                        std::string dispName = dispNameW ? utf8_encode(dispNameW) : "";
                        if (dispNameW) CoTaskMemFree(dispNameW);

                        std::string procName = isSys ? "System Sounds" : "Unknown Application";
                        if (pid != 0) {
                            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                            if (hProc) {
                                wchar_t pathBuf[MAX_PATH] = {};
                                DWORD pSize = MAX_PATH;
                                if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pSize)) {
                                    std::wstring fullPath = pathBuf;
                                    size_t lastSlash = fullPath.find_last_of(L"\\/");
                                    procName = utf8_encode((lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath);
                                }
                                CloseHandle(hProc);
                            } else {
                                procName = "[PID " + std::to_string(pid) + "]";
                            }
                        }

                        std::string persisted_id = (pid != 0) ? get_persisted_endpoint(pid) : "";
                        std::string active_device_id = !persisted_id.empty() ? persisted_id : devId;
                        std::string active_device_name = friendlyName;
                        if (!persisted_id.empty()) {
                            auto eit = endpoint_to_name.find(persisted_id);
                            if (eit != endpoint_to_name.end()) {
                                active_device_name = eit->second;
                            }
                        }

                        // Capture original configuration if not already seen
                        capture_original_if_missing(procName, active_device_id);

                        // Group or update application session
                        auto pit = process_to_index.find(procName);
                        if (pit != process_to_index.end()) {
                            // Update existing entry if needed
                            auto& existing = result[pit->second];
                            if (existing.pid == 0 && pid != 0) existing.pid = pid;
                            if (existing.display_name.empty() && !dispName.empty()) existing.display_name = dispName;
                            existing.current_device_id = active_device_id;
                            existing.current_device_name = active_device_name;
                        } else {
                            AudioAppSession app;
                            app.pid = pid;
                            app.process_name = procName;
                            app.display_name = dispName.empty() ? procName : dispName;
                            app.current_device_id = active_device_id;
                            app.current_device_name = active_device_name;
                            app.is_system_sound = isSys;

                            process_to_index[procName] = result.size();
                            result.push_back(std::move(app));
                        }

                        pCtrl2->Release();
                    }
                    pCtrl->Release();
                }
                pSessEnum->Release();
            }
            pMgr->Release();
        }
        pDev->Release();
    }

    pCol->Release();
    pEnum->Release();

    // Sort by process name
    std::sort(result.begin(), result.end(), [](const AudioAppSession& a, const AudioAppSession& b) {
        return a.process_name < b.process_name;
    });

    AT_LOG_INFO("APP/ROUTER", "Audio application enumeration complete: found %zu unique applications", result.size());
    return result;
}

bool AppAudioRouter::set_app_output_device(uint32_t pid, const std::string& process_name, const std::string& endpoint_id) {
    std::lock_guard<std::mutex> lock(router_mutex_);

    AT_LOG_INFO("APP/ROUTER", "Routing request: app='%s', PID=%u -> Endpoint='%s'",
                process_name.c_str(), pid, endpoint_id.empty() ? "[Default]" : endpoint_id.c_str());

    // Capture original state before making the first modification
    capture_original_if_missing(process_name, "");

    std::wstring endpoint_w = (endpoint_id.empty() || endpoint_id == "(default)" || endpoint_id == "default")
                              ? L""
                              : utf8_decode(endpoint_id);

    bool any_success = false;
    if (pid != 0) {
        if (set_persisted_endpoint(pid, endpoint_w)) {
            any_success = true;
        }
    }

    // Also route any other running PIDs matching this process name (e.g. multi-process browsers)
    if (!process_name.empty() && process_name != "System Sounds") {
        auto sibling_pids = find_all_pids_for_process(process_name);
        if (!sibling_pids.empty()) {
            AT_LOG_DEBUG("APP/ROUTER", "Found %zu sibling processes for '%s'", sibling_pids.size(), process_name.c_str());
        }
        for (DWORD s_pid : sibling_pids) {
            if (s_pid != pid) {
                if (set_persisted_endpoint(s_pid, endpoint_w)) {
                    any_success = true;
                }
            }
        }
    }

    if (!any_success) {
        AT_LOG_WARN("APP/ROUTER", "Could not route process '%s' (PID %u): SetPersistedDefaultAudioEndpoint failed", process_name.c_str(), pid);
    }
    return any_success;
}

void AppAudioRouter::apply_routing_map(const std::map<std::string, std::string>& routing) {
    for (const auto& [proc, endp] : routing) {
        auto pids = find_all_pids_for_process(proc);
        for (DWORD pid : pids) {
            set_app_output_device(pid, proc, endp);
        }
    }
}

void AppAudioRouter::capture_original_if_missing(const std::string& process_name, const std::string& current_device_id) {
    if (process_name.empty()) return;
    if (original_routing_.find(process_name) == original_routing_.end()) {
        original_routing_[process_name] = current_device_id;
        save_state();
    }
}

void AppAudioRouter::revert_all_to_original() {
    std::lock_guard<std::mutex> lock(router_mutex_);
    AT_LOG_INFO("APP/ROUTER", "Restoring original system audio routing for %zu application(s)...", original_routing_.size());
    for (const auto& [proc, orig_endp] : original_routing_) {
        std::wstring endp_w = (orig_endp.empty() || orig_endp == "(default)" || orig_endp == "default")
                              ? L""
                              : utf8_decode(orig_endp);
        auto pids = find_all_pids_for_process(proc);
        for (DWORD pid : pids) {
            set_persisted_endpoint(pid, endp_w);
        }
    }
}

std::map<std::string, std::string> AppAudioRouter::get_original_routing() const {
    std::lock_guard<std::mutex> lock(router_mutex_);
    return original_routing_;
}

std::vector<std::string> AppAudioRouter::list_presets() const {
    std::vector<std::string> presets;
    try {
        auto pdir = AppPathsAndRegistry::get_routing_presets_dir();
        if (std::filesystem::exists(pdir)) {
            for (const auto& entry : std::filesystem::directory_iterator(pdir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    presets.push_back(entry.path().stem().string());
                }
            }
        }
    } catch (...) {}
    std::sort(presets.begin(), presets.end());
    return presets;
}

bool AppAudioRouter::save_preset(const std::string& name, const std::map<std::string, std::string>& mapping, std::string* out_err) {
    if (name.empty()) {
        if (out_err) *out_err = "Preset name cannot be empty";
        return false;
    }
    try {
        auto pdir = AppPathsAndRegistry::get_routing_presets_dir();
        std::filesystem::create_directories(pdir);
        auto fpath = pdir / (name + ".json");

        json j;
        j["name"] = name;
        json map_j = json::object();
        for (const auto& [k, v] : mapping) {
            map_j[k] = v;
        }
        j["routing"] = map_j;

        std::ofstream f(fpath);
        if (!f.is_open()) {
            if (out_err) *out_err = "Failed to open file for writing: " + fpath.string();
            AT_LOG_ERROR("FILESYSTEM", "Failed to open preset file for writing: %s", fpath.string().c_str());
            return false;
        }
        f << j.dump(2);
        AT_LOG_INFO("FILESYSTEM", "Saved routing preset '%s' (%zu rules) to %s", name.c_str(), mapping.size(), fpath.string().c_str());
        return true;
    } catch (const std::exception& e) {
        if (out_err) *out_err = e.what();
        AT_LOG_ERROR("FILESYSTEM", "Exception while saving preset '%s': %s", name.c_str(), e.what());
        return false;
    }
}

bool AppAudioRouter::load_preset(const std::string& name, std::map<std::string, std::string>* out_mapping, std::string* out_err) {
    if (!out_mapping) return false;
    try {
        auto pdir = AppPathsAndRegistry::get_routing_presets_dir();
        auto fpath = pdir / (name + ".json");
        if (!std::filesystem::exists(fpath)) {
            if (out_err) *out_err = "Preset file not found: " + fpath.string();
            AT_LOG_WARN("FILESYSTEM", "Preset file not found: %s", fpath.string().c_str());
            return false;
        }
        std::ifstream f(fpath);
        if (!f.is_open()) {
            if (out_err) *out_err = "Failed to open file: " + fpath.string();
            AT_LOG_ERROR("FILESYSTEM", "Failed to open preset file: %s", fpath.string().c_str());
            return false;
        }
        json j;
        f >> j;
        out_mapping->clear();
        if (j.contains("routing") && j["routing"].is_object()) {
            for (auto& [proc, val] : j["routing"].items()) {
                if (val.is_string()) {
                    (*out_mapping)[proc] = val.get<std::string>();
                }
            }
        }
        AT_LOG_INFO("FILESYSTEM", "Loaded routing preset '%s' (%zu rules) from %s", name.c_str(), out_mapping->size(), fpath.string().c_str());
        return true;
    } catch (const std::exception& e) {
        if (out_err) *out_err = e.what();
        AT_LOG_ERROR("FILESYSTEM", "Exception while loading preset '%s': %s", name.c_str(), e.what());
        return false;
    }
}

bool AppAudioRouter::delete_preset(const std::string& name, std::string* out_err) {
    try {
        auto pdir = AppPathsAndRegistry::get_routing_presets_dir();
        auto fpath = pdir / (name + ".json");
        if (std::filesystem::exists(fpath)) {
            std::filesystem::remove(fpath);
            AT_LOG_INFO("FILESYSTEM", "Deleted routing preset '%s' at %s", name.c_str(), fpath.string().c_str());
        }
        return true;
    } catch (const std::exception& e) {
        if (out_err) *out_err = e.what();
        AT_LOG_ERROR("FILESYSTEM", "Exception while deleting preset '%s': %s", name.c_str(), e.what());
        return false;
    }
}

bool AppAudioRouter::get_revert_on_exit() const {
    std::lock_guard<std::mutex> lock(router_mutex_);
    return revert_on_exit_;
}

void AppAudioRouter::set_revert_on_exit(bool revert) {
    {
        std::lock_guard<std::mutex> lock(router_mutex_);
        revert_on_exit_ = revert;
    }
    save_state();
}

void AppAudioRouter::load_state() {
    // 1. Load global original routing
    try {
        auto orig_path = AppPathsAndRegistry::get_routing_original_file_path();
        if (std::filesystem::exists(orig_path)) {
            std::ifstream f(orig_path);
            if (f.is_open()) {
                json j;
                f >> j;
                if (j.is_object()) {
                    for (auto& [k, v] : j.items()) {
                        if (v.is_string()) original_routing_[k] = v.get<std::string>();
                    }
                }
                AT_LOG_INFO("FILESYSTEM", "Loaded %zu original routing baseline mappings from %s",
                            original_routing_.size(), orig_path.string().c_str());
            }
        }
    } catch (const std::exception& e) {
        AT_LOG_ERROR("FILESYSTEM", "Error reading original routing baseline file: %s", e.what());
    }

    // 2. Load router configuration (revert_on_exit)
    try {
        auto cfg_path = AppPathsAndRegistry::get_routing_config_file_path();
        if (std::filesystem::exists(cfg_path)) {
            std::ifstream f(cfg_path);
            if (f.is_open()) {
                json j;
                f >> j;
                if (j.contains("revert_on_exit")) {
                    revert_on_exit_ = j["revert_on_exit"].get<bool>();
                }
                AT_LOG_DEBUG("FILESYSTEM", "Loaded router configuration (revert_on_exit=%s) from %s",
                             revert_on_exit_ ? "true" : "false", cfg_path.string().c_str());
            }
        }
    } catch (const std::exception& e) {
        AT_LOG_ERROR("FILESYSTEM", "Error reading router config file: %s", e.what());
    }
}

void AppAudioRouter::save_state() {
    // 1. Save global original routing
    try {
        auto orig_path = AppPathsAndRegistry::get_routing_original_file_path();
        std::filesystem::create_directories(orig_path.parent_path());
        json j = json::object();
        for (const auto& [k, v] : original_routing_) {
            j[k] = v;
        }
        std::ofstream f(orig_path);
        if (f.is_open()) {
            f << j.dump(2);
        }
    } catch (const std::exception& e) {
        AT_LOG_ERROR("FILESYSTEM", "Error writing original routing baseline file: %s", e.what());
    }

    // 2. Save router configuration
    try {
        auto cfg_path = AppPathsAndRegistry::get_routing_config_file_path();
        std::filesystem::create_directories(cfg_path.parent_path());
        json j;
        j["revert_on_exit"] = revert_on_exit_;
        std::ofstream f(cfg_path);
        if (f.is_open()) {
            f << j.dump(2);
        }
    } catch (const std::exception& e) {
        AT_LOG_ERROR("FILESYSTEM", "Error writing router config file: %s", e.what());
    }
}

} // namespace at

