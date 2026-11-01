# AudioTool (AT) — Robust Logging & Crash Diagnostics Specification

## 1. Overview & Architectural Goals

The **AudioTool Diagnostic Logging Subsystem** provides deterministic, structured file and console logging across the entire application lifecycle. Because AudioTool interacts deeply with undocumented Windows COM interfaces (`AudioPolicyConfig`), WASAPI hardware devices, registry keys, and dynamic DLL plugins, diagnostic visibility is critical for rapid root-cause analysis without requiring an active debugger.

### Key Architecture Requirements:
1. **Zero Real-Time Interference**: Strict prohibition of logging in real-time audio threads (`WASAPI` callback, SPSC ring buffer operations, node `process_audio()` and `process_tick()` functions). The logging pipeline acquires a recursive lock and invokes file I/O—these operations violate real-time safety invariants and must never be called on the audio thread.
2. **Dual-File Output**: Every run writes simultaneously to a timestamped archival log (`audiotool_YYYY-MM-DD_HH-MM-SS.log`) and a live session log (`latest.log`) under `%LOCALAPPDATA%\Rouvr\AudioTool\logs\`.
3. **Automatic $X$-File Retention Rotation**: Automatic pruning of older timestamped logs keeps the user's disk footprint clean while retaining the last $X$ sessions (configurable, default 10).
4. **Post-Mortem Crash Diagnostics**: A native Windows Structured Exception Handling (SEH) unhandled exception filter catches access violations, null-pointer dereferences, illegal instructions, and RPC errors, logging faulting addresses, faulting module names + offsets, memory access intents (read vs. write vs. DEP execute), and full 64-bit CPU register states before emergency-flushing buffers.

---

## 2. Directory Layout & File Convention

All log files reside in the dedicated `logs/` directory inside `%LOCALAPPDATA%\Rouvr\AudioTool\`:

```text
%LOCALAPPDATA%\Rouvr\AudioTool\
├── logs/
│   ├── latest.log                             # Truncated and written anew on each startup
│   ├── audiotool_2026-11-01_13-41-48.log      # Session log with creation timestamp
│   ├── audiotool_2026-11-01_12-30-15.log
│   └── ... (up to max_retained_logs files)
├── profiles/
├── plugins/
└── cache/
```

### Automatic Log Rotation ($X$-Retention)
During `Logger::init(max_retained_logs, min_level)`:
1. The `logs/` directory is created if missing (`AppPathsAndRegistry::ensure_directories_exist()`).
2. All regular files matching `audiotool_*.log` are discovered.
3. Filenames are sorted lexicographically (which corresponds to chronological order due to `YYYY-MM-DD_HH-MM-SS` formatting).
4. If `existing_logs.size() >= max_retained_logs`, the oldest `(existing_logs.size() - max_retained_logs + 1)` files are immediately unlinked via `std::filesystem::remove`.
5. The new timestamped log is created in append mode, and `latest.log` is truncated (`std::ios::trunc`).

---

## 3. Log Formatting & Structured Syntax

Every line emitted by `Logger::log` strictly adheres to the following machine- and human-readable format:

```text
[TIMESTAMP] [TID:0xXXXX] [LEVEL] [SUBSYSTEM ] [FILE:LINE] Message text
```

### Components:
- **`TIMESTAMP`**: ISO 8601 millisecond-precision local time: `YYYY-MM-DD HH:MM:SS.mmm`
- **`TID`**: Windows Thread ID in hexadecimal format (`0x172F4`) to immediately distinguish GUI threads, worker threads, and device notification threads.
- **`LEVEL`**: 5-character aligned severity level:
  - `TRACE`: Fine-grained loop iterations and step-by-step states.
  - `DEBUG`: Initialization steps, component resolution, internal indices.
  - `INFO `: Major state changes, plugin loads, profile saves, device creations.
  - `WARN `: Non-fatal recoverable conditions (e.g., missing preset, registry key creation, fallback interface used).
  - `ERROR`: Subsystem failures, COM HRESULT errors, failed file writes, invalid DLL ABI.
  - `FATAL`: Unrecoverable errors preceding program termination or crashes.
- **`SUBSYSTEM`**: 10-character fixed-width tag identifying the architecture layer:
  - `[WINAPI    ]`: Win32 API calls (`RegCreateKeyExW`, `CreateEventW`, `SetThreadPriority`).
  - `[FILESYSTEM]`: File and directory operations (`create_directories`, profile read/write).
  - `[COM/POLICY]`: Undocumented Windows audio policy COM interfaces (`AudioPolicyConfig`, `RoGetActivationFactory`).
  - `[WASAPI    ]`: Windows Audio Session API endpoints, formats, MMCSS registration.
  - `[ENGINE    ]`: Audio engine graph state, sample rates, buffer sizes.
  - `[PLUGIN    ]`: Dynamic DLL loading, Mark-of-the-Web security checks, ABI verification.
  - `[GUI       ]`: D3D11 rendering, ImGui windows, UI state.
  - `[CRASH     ]`: Windows SEH unhandled exception dumps and panic messages.
  - `[SECURITY  ]`: DLL search order hardening and unsigned executable validation.
- **`FILE:LINE`**: Basename of the emitting source file and line number (`AppAudioRouter.cpp:174`).

---

## 4. Crash Handling Architecture (Windows SEH)

AudioTool registers a top-level unhandled exception filter via `SetUnhandledExceptionFilter(CrashExceptionFilter)` during startup in `Logger::install_crash_handler()`.

### Handled Hardware & Kernel Exceptions:
- `EXCEPTION_ACCESS_VIOLATION` (0xC0000005)
- `EXCEPTION_ILLEGAL_INSTRUCTION` (0xC000001D)
- `EXCEPTION_INT_DIVIDE_BY_ZERO` (0xC0000094)
- `EXCEPTION_STACK_OVERFLOW` (0xC00000FD)
- `EXCEPTION_DATATYPE_MISALIGNMENT` (0x80000002)
- `RPC_S_SERVER_UNAVAILABLE` (0x800706BA)

### Diagnostic Dump Output
When an unhandled exception occurs, the crash handler formats and synchronously writes:
1. **Exception Code & Name**: Human-readable decoding of the NTSTATUS / HRESULT code.
2. **Memory Violation Details**:
   - Access Type: `READ`, `WRITE`, or `EXECUTE (DEP Violation)`.
   - Target Virtual Address: The exact memory address being referenced when faulting.
3. **Module Resolution**:
   - Uses `GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)` to locate the faulting DLL/EXE.
   - Computes base address and relative offset (`module+0x1234`).
4. **Full 64-Bit CPU Register Dump**:
   - Instruction Pointer (`RIP`), Stack Pointer (`RSP`), Base Pointer (`RBP`), Flags (`EFLAGS`).
   - General Purpose Registers: `RAX`, `RBX`, `RCX`, `RDX`, `RSI`, `RDI`, `R8` through `R15`.
5. **Immediate Emergency Flush**:
   - Both `dated_stream` and `latest_stream` are immediately flushed to ensure crash logs are stored even if the process dies instantly.

---

## 5. Subsystem-Specific Instrumentation Details

### 5.1 Undocumented COM (`AppAudioRouter`)
Interfacing with `AudioPolicyConfig` (`combase.dll`) involves private COM classes that differ between Windows 10 and Windows 11.
- Logged Events:
  - DLL Loading (`combase.dll`).
  - `WindowsCreateString` / `WindowsDeleteString` calls.
  - `RoGetActivationFactory` attempts: first trying Windows 11 IID (`ab3d4648-e242-4596-8534-5266f7f86188`), then falling back to Windows 10 IID (`2a59116d-6c4f-45e0-a74f-707e3fef9258`).
  - Exact `HRESULT` return codes for factory creation and `SetPersistedDefaultAudioEndpoint`.
  - Process ID to process name mapping via `OpenProcess` and `QueryFullProcessImageNameW`.
  - Application endpoint redirection changes, profile preset saves/loads, and baseline restoration upon exit.

### 5.2 WASAPI & Audio Devices (`DeviceManager`)
- Logged Events:
  - `CoCreateInstance(CLSID_MMDeviceEnumerator)` and endpoint enumeration.
  - MMCSS `AvSetMmThreadCharacteristicsW(L"Pro Audio")` task index registration.
  - Format negotiation: channel counts, sample rates, bit depths, and buffer durations.
  - Creation of buffer event handles via `CreateEventExW`.
  - Device invalidation, stream routing changes, and master device shutdown.
- **Real-Time Loop Prohibition**: The inner loop `while (worker.running)` that services `WaitForMultipleObjects` and copies audio samples NEVER invokes the logger.

### 5.3 Dynamic Plugins (`PluginManager`)
- Logged Events:
  - Directory enumeration of `%LOCALAPPDATA%\Rouvr\AudioTool\plugins`.
  - NTFS Alternate Data Stream check (`:Zone.Identifier`) to detect Mark-of-the-Web (MOTW).
  - Shadow-copying to `%LOCALAPPDATA%\Rouvr\AudioTool\cache\plugin_shadow\`.
  - `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` results.
  - `GetProcAddress` for `at_get_plugin_descriptor`.
  - ABI version validation: comparing `major`/`minor` against `AT_ABI_VERSION_MAJOR`.
  - Plugin registration and descriptor verification.

### 5.4 Filesystem & Registry (`AppPathsAndRegistry`)
- Logged Events:
  - Resolution of `%LOCALAPPDATA%` and `%PROGRAMFILES%`.
  - Directory tree validation and creation.
  - `RegCreateKeyExW`, `RegSetValueExW`, `RegQueryValueExW` operations under `HKCU\Software\Rouvr\AudioTool`.
  - `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)`.

---

## 6. How Future Agents Should Triage Crashes & Failures

When diagnosing an issue reported by a user:

1. **Locate the Log File**:
   - Check `%LOCALAPPDATA%\Rouvr\AudioTool\logs\latest.log`.
   - If multiple runs have occurred, inspect the timestamped files in `%LOCALAPPDATA%\Rouvr\AudioTool\logs\`.
2. **Search for `[CRASH     ]`**:
   - If the application crashed, the crash handler will have written an `[EMERGENCY CRASH DUMP]` section at the end of the log.
   - Look at `Exception Code`:
     - `0xC0000005` (Access Violation) with `Target Address: 0x0000000000000000` indicates a NULL pointer dereference.
     - `Fault Address: module.dll+0x...` identifies which dynamic library or binary threw the fault.
3. **Search for `[ERROR]` and `[WARN]`**:
   - Filter by subsystem:
     - `[COM/POLICY]`: Check HRESULT codes for `SetPersistedDefaultAudioEndpoint`. `0x80004002` = `E_NOINTERFACE` (Windows OS version has a different undocumented IID).
     - `[PLUGIN    ]`: Check if a plugin was blocked due to untrusted download (`Zone.Identifier`) or ABI mismatch.
     - `[WINAPI    ]`: Check registry permissions or thread priority elevation failures.
     - `[WASAPI    ]`: Check for device detachment (`AUDCLNT_E_DEVICE_INVALIDATED`) or format incompatibility.

