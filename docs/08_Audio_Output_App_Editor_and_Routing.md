# AudioTool — Audio Output App Editor & Per-App Endpoint Routing

## 1. Overview & Objective
In modern Windows (Windows 10 RS4+ and Windows 11), per-application audio device routing is buried deep within the Windows Settings app under *Settings -> System -> Sound -> App volume and device preferences*. This requires numerous clicks and is inconvenient during live streaming, audio mixing, or gaming sessions.

AudioTool incorporates a built-in **Audio Output App Editor** (`AppAudioRouter`) accessible directly from the top menu bar (`Ctrl+R` or the `"App Audio Router"` button). It enables users to:
1. Instantly enumerate all currently running applications outputting audio.
2. Select any system playback endpoint (physical speakers, headsets, or VB-Cable Virtual Audio Cables) from a fast in-app dropdown for each application.
3. Automatically apply per-process overrides immediately without requiring app restarts.
4. Save per-application routing preferences under the active workspace profile (`*.atprof`).
5. Save, load, and delete standalone routing presets (`%LOCALAPPDATA%\Rouvr\AudioTool\routing_presets\*.json`).
6. Track original system routing globally outside the profile in `%LOCALAPPDATA%\Rouvr\AudioTool\app_routing_original.json`.
7. Revert all applications back to their original configuration upon exit or via a one-click reset button.

---

## 2. Technical Architecture & Windows Core Audio Integration

### 2.1 Enumeration of Active Audio Applications
Windows does not maintain a single flat list of audio applications. Instead, active audio sessions are distributed across the respective render endpoints (`IMMDevice`).
`AppAudioRouter::enumerate_apps()`:
1. Enumerates all active playback endpoints (`eRender`) via `IMMDeviceEnumerator::EnumAudioEndpoints()`.
2. Activates `IAudioSessionManager2` on each endpoint.
3. Iterates over `IAudioSessionEnumerator` sessions, filtering out expired sessions (`AudioSessionStateExpired`).
4. Queries `IAudioSessionControl2` for the Process ID (`pid`) and display name.
5. Obtains the executable name (e.g. `firefox.exe`, `spotify.exe`, `chrome.exe`) via `OpenProcess` with `PROCESS_QUERY_LIMITED_INFORMATION` and `QueryFullProcessImageNameW`.
6. Resolves the current output device and captures the baseline endpoint ID into the global original state if not already recorded.

### 2.2 Live Endpoint Redirection via `AudioPolicyConfig`
To programmatically redirect an active process's audio stream without touching private process memory, AudioTool connects to the Windows Media Policy COM activation factory:
- **WinRT Class Name:** `Windows.Media.Internal.AudioPolicyConfig`
- **Activation:** `RoGetActivationFactory` via `combase.dll`.
- **Interface IDs (IID):**
  - Modern Windows 10 & Windows 11 (Build 21390+ / 21H2+): `{ab3d4648-e242-459f-b02f-541c70306324}`
  - Downlevel Windows 10: `{2a59116d-6c4f-45e0-a74f-707e3fef9258}`
- **Redirection Call:**
  ```cpp
  policy->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, deviceId);
  policy->SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, deviceId);
  ```
  - Passing `nullptr` or `L""` clears the override and resets the application to system default.
  - Multi-process applications (such as Chromium and Firefox content/utility processes) are handled by enumerating all sibling PIDs sharing the executable image name via `CreateToolhelp32Snapshot` and redirecting them in tandem.

---

## 3. Storage Hierarchy & Profile Integration

| Setting / Data | Storage Location | Lifetime & Scope |
| :--- | :--- | :--- |
| **Active Profile Routing** | Saved inside `<profile>.atprof` under `"app_routing"` JSON block | Per-profile. Loaded when opening a profile tab. |
| **Standalone Presets** | `%LOCALAPPDATA%\Rouvr\AudioTool\routing_presets\<name>.json` | Global across all profiles. Can be saved, loaded, and deleted independently. |
| **Global Original State** | `%LOCALAPPDATA%\Rouvr\AudioTool\app_routing_original.json` | Persistent globally outside of workspace profiles. Captures first-seen endpoint for each process. |
| **Router Configuration** | `%LOCALAPPDATA%\Rouvr\AudioTool\app_routing_config.json` | Global app config. Persists the `revert_on_exit` boolean flag. |

---

## 4. Revert Upon Exit Safety Mechanism
To ensure user system audio settings are not permanently disturbed by AudioTool testing:
1. The checkbox `"Revert to original configuration upon exit"` is enabled by default.
2. In `WinMain()`, before exiting the message loop and destroying the window, AudioTool inspects `engine.audio_router().get_revert_on_exit()`.
3. If true, `revert_all_to_original()` iterates over all known processes and resets their audio endpoints back to the original baseline captured before AudioTool intervened.
