# AudioTool (AT) — Master Technical Specification

## 1. Executive Summary

**AudioTool (AT)** is a native Windows C++ modular audio mixer, soundboard, and signal-processing application built around a dual-plane directed graph of **Nodes**.

The system separates **Audio Stream Processing** (real-time continuous sample streams) from **Control Signal Processing** (discrete event triggers and parameter modulations), while exposing a unified **Node Plugin ABI** that allows nodes to be dynamically loaded from DLLs at startup or runtime. Furthermore, the **Audio Backend Engine** runs completely independently from the **Graphical User Interface (GUI)**, allowing headless operation, crash isolation, and concurrent execution of multiple workspace profiles.

---

## 2. Specification Document Index

This specification is organized into six detailed engineering documents located in the [`docs/`](file:///d:/Code/AudioTool/docs) directory:

1. **[01 — Architecture, Audio Engine & Control Loops](file:///d:/Code/AudioTool/docs/01_Architecture_and_Control_Loops.md)**
   - High-level system architecture and thread model.
   - Real-time Audio Processing Loop, Control Plane Loop, and Device Manager Loop.
   - Multi-profile concurrent execution model.
   - Hidden Master Hardware Device multiplexing/demultiplexing.
   - Audio memory layout, zero-copy buffer pooling, and runtime reconfiguration (buffer size, sample rate).
2. **[02 — Node & Plugin ABI/API Specification](file:///d:/Code/AudioTool/docs/02_Node_Plugin_ABI_and_API.md)**
   - Object hierarchy (`INode`, `IAudioNode`, `IControlNode`).
   - Cross-DLL C99/C++ ABI boundary requirements (avoiding CRT heap/STL mismatches).
   - UI Control Descriptor specification (types, ranges, preferred visual styles).
   - Reflection & Action/Function Binding API (`Set`, `Add`, `Toggle`, `Trigger` with typed parameters).
   - Specification of initial built-in nodes (`VirtualInputNode`, `PlaybackNode`, `VolumeNode`, `EffectNode`, `OutputNode`, `TimerControlNode`).
3. **[03 — Backend-Frontend Protocol & Workspace Serialization](file:///d:/Code/AudioTool/docs/03_Backend_Frontend_IPC_and_Serialization.md)**
   - Decoupled Backend API (`IAudioBackendService`) supporting both in-process and out-of-process (Named Pipe / Shared Memory) deployment.
   - Command dispatch, async state snapshots, and real-time metering telemetry.
   - Complete JSON schema for Workspace Profiles (`.atprof`), node state serialization, audio connections, and parameterized control plane bindings.
4. **[04 — GUI & Interaction Specification](file:///d:/Code/AudioTool/docs/04_GUI_Specification.md)**
   - Multi-profile tab bar and File menu workflows.
   - Infinite 2D Canvas with toggleable **Audio Plane View** and **Control/Data Plane View**.
   - Node visual design: Compact (Form 1) vs. Expanded Table-Row Layout (Form 2), header actions, and snap points.
   - Connection creation, directional arrows, mid-point deletion cross, and Control Function Binding popups.
   - Filesystem-mirrored right-click context menu for dynamic node spawning from `%LOCALAPPDATA%\Rouvr\AudioTool\plugins`.
5. **[05 — Libraries, Needed Resources & Real-Time Audio Pitfalls](file:///d:/Code/AudioTool/docs/05_Libraries_Resources_and_Pitfalls.md)**
   - Curated evaluation of C++ libraries for Windows Audio (WASAPI, `miniaudio`), DSP/Resampling, Lock-Free queues, Serialization, and GUI (`Dear ImGui` + custom canvas).
   - Critical real-time audio pitfalls (Hardware Clock Drift / ASRC, Priority Inversion, Denormal numbers, Zipper noise, DLL hot-reload file locking on Windows, Graph Feedback loops) and concrete engineering solutions.
6. **[06 — Security (Running Unsigned Builds Safely) & Standardized Build Pipeline](file:///d:/Code/AudioTool/docs/06_Security_Signing_and_Build_Pipeline.md)**
   - User-configurable file hierarchy under `%LOCALAPPDATA%\Rouvr\AudioTool` (`profiles/`, `plugins/`, `cache/`, `config.json`).
   - Windows Registry specification under `HKEY_CURRENT_USER\Software\Rouvr\AudioTool` (`InstallPath`, `LocalDataPath`, `Version`, `BuildId`).
   - How to run unsigned local builds safely on Windows without weakening security for downloaded applications (DLL search order hardening, Mark-of-the-Web isolation, and optional Local-Only Self-Signed Code Signing).
   - Standardized CMake + `Makefile` / `build.ps1` pipeline that reads manual version from `VERSION.txt`, generates a unique `AT_BUILD_ID` in `include/at_version.h`, commits changes to Git, builds the project, deploys plugins, and runs automated tests.

---

## 3. Core Architectural Principles

| Principle | Design Decision |
| :--- | :--- |
| **Native Windows Performance** | C++20 native build targeting Windows 10/11 (x64), utilizing **WASAPI** (Shared & Exclusive modes) and **MMCSS** (`Pro Audio` thread scheduling). |
| **Dual-Plane Graph** | Every node participates in the **Control Plane** (inputs/outputs for triggers & parameter calls). Audio nodes (`Source`, `Processing`, `Output`) additionally participate in the **Audio Plane** (multi-input, multi-output up to `MAX_CONNECTIONS`, default `4`, runtime-configurable). |
| **Strict Real-Time Safety** | Zero heap allocations (`new`/`malloc`), zero blocking mutexes, zero syscalls, and zero disk/file I/O on the real-time audio callback thread. |
| **ABI-Stable DLL Plugins** | Plugins export a pure C ABI (`extern "C"`) vtable struct with explicit version negotiation and host-provided memory/logging allocators so plugins can be compiled with different compiler versions safely. |
| **Hardware Device Multiplexing** | Multiple `VirtualInputNode` or `OutputNode` instances across one or more active profiles can bind to the same Windows audio endpoint via an internal reference-counted **Hidden Master Device Node** managed by the Engine's `HardwareDeviceManager`. |
| **Headless-Capable Backend** | The Audio Engine compiles as a standalone library/daemon (`at_engine`). The GUI (`at_gui`) communicates strictly through a command/event protocol and never mutates audio graph structures directly. |

---

## 4. Proposed Repository & Runtime Directory Structure

### 4.1 Project Directory (`D:\Code\AudioTool`)
```text
AudioTool/
├── CMakeLists.txt                     # Root CMake configuration (C++20)
├── Makefile                           # Standard make wrapper for CMake + Build ID pipeline
├── VERSION.txt                        # Manually supplied semantic version string
├── requirements.md                    # Original product requirements
├── SPECIFICATION.md                   # Master technical specification index
├── scripts/
│   └── build.ps1                      # Standardized Build ID generator, git commit, CMake build & test
├── docs/                              # Detailed engineering specifications
│   ├── 01_Architecture_and_Control_Loops.md
│   ├── 02_Node_Plugin_ABI_and_API.md
│   ├── 03_Backend_Frontend_IPC_and_Serialization.md
│   ├── 04_GUI_Specification.md
│   ├── 05_Libraries_Resources_and_Pitfalls.md
│   └── 06_Security_Signing_and_Build_Pipeline.md
├── include/
│   ├── at_version.h                   # Auto-generated Build ID & Version header included in builds
│   └── at_sdk/                        # Public Header-Only SDK for Node DLL Plugins
│       ├── at_abi.h                   # C99/C++ ABI structs, enums, version macros
│       ├── at_node_base.hpp           # Optional C++ convenience wrapper for plugin authors
│       └── at_simd_buffer.h           # Non-interleaved 64-byte aligned audio buffer view
├── src/
│   ├── common/                        # Paths/Registry (%LOCALAPPDATA%\Rouvr\AudioTool), SPSC queues
│   ├── engine/                        # Headless Audio & Control Backend
│   │   ├── EngineServer.h/.cpp        # Top-level service & multi-profile coordinator
│   │   ├── ProfileInstance.h/.cpp     # Per-profile graph, scheduler, and control loop
│   │   ├── AudioGraphCompiler.h/.cpp  # Topological sort, buffer allocation, RCU swap
│   │   ├── ControlDispatcher.h/.cpp   # Control plane event router & timer scheduler
│   │   ├── DeviceManager.h/.cpp       # WASAPI Hidden Master Device nodes & ASRC
│   │   └── PluginManager.h/.cpp       # DLL discovery in %LOCALAPPDATA%, shadow-copy loading
│   ├── nodes_builtin/                 # Built-in node plugins (compiled as DLLs, synced to %LOCALAPPDATA%)
│   │   ├── Audio/
│   │   │   ├── VirtualInputNode/
│   │   │   ├── PlaybackNode/
│   │   │   ├── VolumeNode/
│   │   │   ├── Effects/
│   │   │   └── OutputNode/
│   │   └── Control/
│   │       └── TimerNode/
│   ├── app/                           # Proof-of-Concept & Headless CLI runner
│   └── gui/                           # Standalone GUI Application
└── tests/                             # Automated CTest unit & integration audio pass-through tests
```

### 4.2 User Configurable Data Directory (`%LOCALAPPDATA%\Rouvr\AudioTool`) & Registry
- **Registry Key:** `HKEY_CURRENT_USER\Software\Rouvr\AudioTool`
  - `InstallPath` (`REG_SZ`): Path to compiled application binaries.
  - `LocalDataPath` (`REG_SZ`): Resolved path to `%LOCALAPPDATA%\Rouvr\AudioTool`.
  - `Version` (`REG_SZ`) & `BuildId` (`REG_SZ`).
- **User Data Layout (`%LOCALAPPDATA%\Rouvr\AudioTool\`):**
```text
%LOCALAPPDATA%\Rouvr\AudioTool\
├── config.json                        # User-configurable engine constants (buffer size, sample rate)
├── profiles/                          # Saved JSON workspace profiles (*.atprof)
├── plugins/                           # Runtime directory scanned for Node DLLs
│   ├── Audio/
│   │   ├── Sources/
│   │   ├── Processing/
│   │   └── Outputs/
│   └── Control/
│       └── Triggers/
└── cache/
    └── plugin_shadow/                 # Shadow-copied DLLs loaded at runtime
```

---

## 5. Recommended Implementation Phases

1. **Phase 1 — SDK & Core Engine Skeleton:** Define `at_abi.h`, implement lock-free queues, build the `AudioGraphCompiler` (topological sort + buffer pool), and test with dummy in-memory nodes.
2. **Phase 2 — Hardware Device Manager & Master Clock:** Integrate `miniaudio` / WASAPI, implement the Hidden Master Input/Output nodes, ring-buffered Asynchronous Sample Rate Conversion (ASRC), and runtime buffer-size reloading.
3. **Phase 3 — DLL Plugin Loader & Core Nodes:** Implement `PluginManager` (directory tree scanner + shadow-copy DLL loader) and build `VirtualInputNode`, `PlaybackNode`, `VolumeNode`, `OutputNode`, and `TimerControlNode`.
4. **Phase 4 — Serialization & Multi-Profile Backend API:** Implement JSON load/save (`nlohmann/json`), multi-profile execution, and the Backend-to-GUI command/telemetry bridge.
5. **Phase 5 — GUI Application:** Build the DX11 + Dear ImGui workspace, dual-plane canvas (Audio vs. Data view), collapsed/expanded node widgets, wire interaction, and function binding dialogs.

