# 03 — Backend-Frontend Protocol, Multi-Profile Engine & JSON Serialization

## 1. Backend Independence & Dual-Mode Transport

Requirement 46 states:
> *"The backend should run independently from a GUI managing it."*

To satisfy this requirement cleanly:
- **`at_engine` (Backend)** has **zero dependencies** on any UI toolkit, DirectX/OpenGL context, or `HWND` window handles. It can be launched as a headless background service/daemon (`at_engine_daemon.exe --profile my_setup.atprof`) or hosted in a dedicated background thread inside the main executable.
- **`at_gui` (Frontend)** interacts with `at_engine` strictly through an abstract asynchronous message bus (`IEngineTransport`).

### 1.1 Transport Implementations
1. **In-Process Lock-Free Transport (Default Desktop Mode):**
   - `at_gui` and `at_engine` run in separate thread pools within the same process, communicating via lock-free MPSC command queues (`CommandMsg`) and SPSC event/telemetry ring buffers (`EventMsg`).
2. **Out-of-Process IPC Transport (Daemon Mode):**
   - Commands and state updates are serialized over a Windows **Named Pipe** (`\\.\pipe\AudioToolEngine_v1`).
   - Real-time peak meters and control wire pulse animations are written to a **Windows File Mapping (Shared Memory)** block (`Local\AudioToolTelemetry_v1`) so the GUI can poll 60 FPS visual meters with zero syscall overhead.
   - If the GUI closes or crashes, the audio mixer and soundboard continue running uninterrupted in the background.

---

## 2. Multi-Profile Concurrent Execution

Requirements 48–51 state:
> *"All state of every node, every setting should be saved under a workspace profile file - json is enough. This enables of quick loading of different configurations, so the file menu should at least have the options to save, load, create a new profile. Multiple profiles should be able to run at the same time, each controlled from its own tab selectable in a top bar of some sort."*

### 2.1 How Multiple Profiles Run Concurrently
Inside `at_engine`, the `EngineServer` maintains a list of active `ProfileSession` instances (`std::map<ProfileID, std::unique_ptr<ProfileSession>>`):
- **Independent Graph Spaces:** Node IDs (`uint32_t`) and connections (Audio & Control planes) are scoped to their owning `ProfileSession`. A node in Profile Tab 1 cannot accidentally collide with a node ID in Profile Tab 2.
- **Shared Hardware Layer:** All active `ProfileSession` instances share the global `HardwareDeviceManager`.
  - If Profile 1 ("Gaming Voice Chat") and Profile 2 ("Background Soundboard") both have an `OutputNode` routed to `"VB-Audio Virtual Cable"`, the `AudioThread` executes Profile 1's graph, then Profile 2's graph, and sums both outputs into the single Hidden Master Output Node for `"VB-Audio Virtual Cable"`.
- **Per-Profile Mute / Pause:** Each profile tab in the GUI has an `Active / Bypassed` indicator, allowing a user to keep multiple profiles loaded in tabs and either run them simultaneously or pause individual tabs without unloading them.

---

## 3. Backend Command & Event API Specification

### 3.1 Commands (Frontend GUI $\rightarrow$ Backend Engine)

| Command Category | Command Name | Payload Parameters | Description |
| :--- | :--- | :--- | :--- |
| **Profile Lifecycle** | `CMD_CREATE_PROFILE` | `string profile_name` | Creates a new empty running profile session; returns `profile_id`. |
| | `CMD_LOAD_PROFILE` | `string file_path` | Parses `.atprof` JSON, instantiates nodes, compiles graph, starts running. |
| | `CMD_SAVE_PROFILE` | `uint32_t profile_id, string file_path` | Serializes current profile state (nodes, controls, connections, UI coords) to disk. |
| | `CMD_CLOSE_PROFILE` | `uint32_t profile_id` | Stops execution of the profile, releases device refs, destroys nodes. |
| | `CMD_SET_PROFILE_ACTIVE` | `uint32_t profile_id, bool active` | Enables or pauses audio/control processing for a specific profile tab. |
| **Graph Topology** | `CMD_SPAWN_NODE` | `uint32_t profile_id, string plugin_type_id, float ui_x, float ui_y` | Instantiates a node from the plugin registry at canvas position `(ui_x, ui_y)`. |
| | `CMD_DELETE_NODE` | `uint32_t profile_id, uint32_t node_id` | Removes all audio/control wires attached to `node_id` and destroys the node. |
| | `CMD_DISCONNECT_ALL` | `uint32_t profile_id, uint32_t node_id` | Disconnects all incoming and outgoing wires on the active plane (or both planes) for a node (Topbar `[X]` disconnect button). |
| | `CMD_CONNECT_AUDIO` | `uint32_t profile_id, uint32_t src_node, uint32_t dst_node` | Creates a directed audio stream edge `src_node -> dst_node` (validates `max_connections` & cycles). |
| | `CMD_DISCONNECT_AUDIO` | `uint32_t profile_id, uint32_t edge_id` | Deletes the specified audio edge. |
| | `CMD_CONNECT_CONTROL` | `uint32_t profile_id, uint32_t src_node, uint32_t dst_node, uint32_t target_func_id, AT_ControlValue bound_arg` | Creates a control plane edge that invokes `target_func_id(bound_arg)` on `dst_node` whenever `src_node` emits a pulse. |
| | `CMD_DISCONNECT_CONTROL`| `uint32_t profile_id, uint32_t edge_id` | Deletes the specified control edge. |
| **Node State & UI** | `CMD_SET_CONTROL_VAL` | `uint32_t profile_id, uint32_t node_id, uint32_t ctrl_id, AT_ControlValue val` | Updates a control value (slider moved, button toggled, file selected). |
| | `CMD_INVOKE_FUNCTION` | `uint32_t profile_id, uint32_t node_id, uint32_t func_id, AT_ControlValue arg` | Directly invokes a node function from the GUI. |
| | `CMD_UPDATE_NODE_UI` | `uint32_t profile_id, uint32_t node_id, float x, float y, bool is_expanded` | Updates persisted canvas coordinates and Form 1 (collapsed) / Form 2 (expanded) state. |
| **Engine Config** | `CMD_RELOAD_ENGINE_CFG` | `uint32_t buffer_size, uint32_t sample_rate, uint32_t max_conns` | Triggers safe runtime reload of audio buffers and constants. |
| | `CMD_RESCAN_PLUGINS` | `void` | Scans `plugins/` directory tree for newly added/recompiled `.dll` files. |

### 3.2 Events & Telemetry (Backend Engine $\rightarrow$ Frontend GUI)

| Event Name | Payload | Description |
| :--- | :--- | :--- |
| `EVT_PROFILE_SNAPSHOT` | Full `ProfileStateDTO` | Sent on profile load/creation or when a GUI client first attaches to a running daemon. |
| `EVT_NODE_CONTROLS_CHANGED`| `profile_id, node_id, vector<AT_ControlDescriptor>` | Sent when a dynamic node (e.g., `VolumeNode`) adds/removes per-input channel strips after an audio connection changes. |
| `EVT_CONTROL_VAL_UPDATED` | `profile_id, node_id, ctrl_id, AT_ControlValue` | Sent when a Control Plane trigger or auto-normalizer mutates a node's parameter so the GUI slider/button updates live. |
| `EVT_CONTROL_PULSE_FIRED` | `profile_id, edge_id, timestamp` | Allows the Data/Control View canvas to briefly highlight/flash the wire when a control pulse travels across it. |
| `EVT_AUDIO_METERS_BATCH` | Array of `{profile_id, node_id, peak_l, peak_r, rms}` | Published at 60 Hz via ring buffer for smooth level meters. |
| `EVT_ENGINE_ERROR` | `error_code, string message` | Alerts the GUI if a connection exceeded `max_connections`, created an illegal cycle, or a hardware device disconnected. |

---

## 4. Workspace Profile JSON Schema (`.atprof`)

Every workspace profile is saved as a human-readable, UTF-8 JSON file (`.atprof`).

### 4.1 Complete Annotated Example

```json
{
  "schema_version": "1.0.0",
  "profile_metadata": {
    "name": "Stream Mixer & Soundboard",
    "created_utc": "2026-11-01T00:45:00Z",
    "modified_utc": "2026-11-01T01:15:00Z",
    "canvas_pan_x": -120.0,
    "canvas_pan_y": -45.0,
    "canvas_zoom": 1.0,
    "active_plane_view": "AUDIO"
  },
  "engine_overrides": {
    "max_connections_per_port": 4
  },
  "nodes": [
    {
      "node_id": 101,
      "plugin_id": "at.builtin.audio.virtual_input",
      "display_name": "Mic Input (Blue Yeti)",
      "plane_type": "AUDIO",
      "ui_state": {
        "pos_x": 100.0,
        "pos_y": 150.0,
        "is_expanded": true
      },
      "control_values": {
        "device_id": "{0.0.1.00000000}.{8a3e2c11-44b2-4d91-9e1a-112233445566}",
        "input_gain_db": 3.5,
        "mute": false
      },
      "custom_state": {}
    },
    {
      "node_id": 102,
      "plugin_id": "at.builtin.audio.playback",
      "display_name": "Airhorn Soundboard",
      "plane_type": "AUDIO",
      "ui_state": {
        "pos_x": 100.0,
        "pos_y": 420.0,
        "is_expanded": true
      },
      "control_values": {
        "file_path": "D:/AudioClips/airhorn.wav",
        "loop_enabled": false,
        "retrigger_mode": "Restart",
        "volume_db": -3.0,
        "playback_speed": 1.0
      },
      "custom_state": {}
    },
    {
      "node_id": 201,
      "plugin_id": "at.builtin.audio.volume_mixer",
      "display_name": "Main Channel Mixer",
      "plane_type": "AUDIO",
      "ui_state": {
        "pos_x": 480.0,
        "pos_y": 240.0,
        "is_expanded": true
      },
      "control_values": {
        "input_gain_db_0": 0.0,
        "input_mute_0": false,
        "input_gain_db_1": -6.0,
        "input_mute_1": false,
        "auto_normalize_enabled": true,
        "target_norm_level_db": -16.0,
        "master_volume_db": 0.0,
        "master_mute": false
      },
      "custom_state": {}
    },
    {
      "node_id": 301,
      "plugin_id": "at.builtin.audio.output",
      "display_name": "Virtual Cable Output",
      "plane_type": "AUDIO",
      "ui_state": {
        "pos_x": 860.0,
        "pos_y": 240.0,
        "is_expanded": false
      },
      "control_values": {
        "device_id": "{0.0.0.00000000}.{c91f4a22-77d1-4a2b-8f00-aabbccddeeff}",
        "output_gain_db": 0.0,
        "mute": false
      },
      "custom_state": {}
    },
    {
      "node_id": 501,
      "plugin_id": "at.builtin.control.timer",
      "display_name": "Periodic Volume Duck Timer",
      "plane_type": "CONTROL",
      "ui_state": {
        "pos_x": 140.0,
        "pos_y": 640.0,
        "is_expanded": true
      },
      "control_values": {
        "interval_ms": 5000,
        "enabled": true
      },
      "custom_state": {}
    }
  ],
  "audio_connections": [
    {
      "edge_id": 1001,
      "source_node_id": 101,
      "target_node_id": 201,
      "target_input_slot": 0
    },
    {
      "edge_id": 1002,
      "source_node_id": 102,
      "target_node_id": 201,
      "target_input_slot": 1
    },
    {
      "edge_id": 1003,
      "source_node_id": 201,
      "target_node_id": 301,
      "target_input_slot": 0
    }
  ],
  "control_connections": [
    {
      "edge_id": 2001,
      "source_node_id": 501,
      "source_output_slot": 0,
      "target_node_id": 201,
      "target_function_id": "add_master_volume",
      "bound_argument": {
        "type": "FLOAT",
        "value": -2.5
      }
    }
  ]
}
```

### 4.2 Handling Missing Plugins or Missing Audio Devices on Load
1. **Missing Audio Device (e.g., USB Microphone unplugged):**
   - The `VirtualInputNode` or `OutputNode` retains the saved `device_id` string in its state, displays `"Device Offline: [Last Known Name]"` in red in its selector dropdown, and outputs silence until either the user selects a new device or plugs the USB device back in (detected via `IMMNotificationClient`).
2. **Missing Plugin DLL:**
   - If a `.atprof` references a `plugin_id` whose DLL is not currently installed, the Engine instantiates a `PlaceholderMissingNode` that preserves the node's `control_values`, `custom_state`, and incoming/outgoing connections so saving the profile does not destroy the user's configuration.

