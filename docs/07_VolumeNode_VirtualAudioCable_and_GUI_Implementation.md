# VolumeNode, Virtual Audio Routing, and GUI Implementation Notes

This document records the architectural details, implementation specifics, and operational guidelines for the **Rouvr AudioTool** system following the completion of the basic Input, Output, and Volume nodes in both backend and frontend.

---

## 1. Architectural Overview & Implemented Nodes

AudioTool separates routing and signal flow into two distinct planes:
- **Audio Plane**: Isochronous, low-latency SIMD floating-point audio frame blocks (48 kHz default, 64-sample to 1024-sample blocks, 64-byte aligned planar memory).
- **Control Plane**: Event-driven signals, function bindings, parameter adjustments, and asynchronous triggers.

### 1.1 Implemented Built-in Nodes

```
+--------------------------------------------------------------------------------------------------+
|                                           AUDIO PLANE                                            |
|                                                                                                  |
|  +--------------------+         +----------------------------------+         +----------------+  |
|  |  VirtualInputNode  |==Audio==> [Input 1: Firefox]               |         |   OutputNode   |  |
|  |  (Firefox Loopback)|         |                                  |         | (Realtek DAC / |  |
|  +--------------------+         |           VolumeNode             |==Audio==>  Speakers)     |  |
|                                 | (Dynamic Channel Strips, Gains,  |         +----------------+  |
|  +--------------------+         |  Mutes, EBU R128 Normalization,  |                             |
|  |  VirtualInputNode  |==Audio==>  Master Gain & Master Mute)      |         +----------------+  |
|  |  (Physical Mic)    |         |                                  |         |   OutputNode   |  |
|  +--------------------+         | [Input 2: Physical Mic]          |==Audio==> (VB-Cable Input|  |
|                                 +----------------------------------+         |  -> Game/VoIP) |  |
|                                                  ^                           +----------------+  |
+--------------------------------------------------|-----------------------------------------------+
|                                            CONTROL PLANE                                         |
|                                                  |                                               |
|  +--------------------+                          |                                               |
|  |     TimerNode      |--------------------------+ (e.g. Action Trigger / Function Binding)       |
|  | (Pulse Generator)  |                                                                          |
|  +--------------------+                                                                          |
+--------------------------------------------------------------------------------------------------+
```

#### 1. `VirtualInputNode.dll` (`at.builtin.audio.virtual_input`)
- **Category:** `Audio/Sources`
- **Capabilities:**
  - Enumerates and captures from physical WASAPI capture endpoints (microphones, line inputs).
  - Binds directly to WASAPI playback loopback endpoints via `loopback:<endpoint_id>` prefix, capturing system or device render audio with zero extraneous drivers.
  - Supports virtual test endpoints (`virtual://test_input`) for automated bit-accurate test suites.
  - Controls: `device_id` (dropdown), `input_gain_db` (slider -60 dB to +24 dB), `mute` (toggle button), `meter_peak_db` (real-time level meter).
  - Functions: `toggle_mute`, `set_input_gain_db`, `add_input_gain_db`.

#### 2. `OutputNode.dll` (`at.builtin.audio.output`)
- **Category:** `Audio/Outputs`
- **Flag:** `AT_NODE_FLAG_AUTO_SUM_INPUTS`
- **Capabilities:**
  - Automatically sums all connected upstream audio input buses into its scratch bus using SIMD vector accumulation.
  - Streams rendered frames into physical WASAPI render endpoints, VB-Audio Virtual Cable inputs, or virtual test endpoints (`virtual://test_output`).
  - Controls: `device_id` (dropdown), `output_gain_db` (slider -60 dB to +12 dB), `mute` (toggle button), `meter_peak_db` (real-time level meter).
  - Functions: `toggle_mute`, `set_output_gain_db`, `add_output_gain_db`.

#### 3. `VolumeNode.dll` (`at.builtin.audio.volume`)
- **Category:** `Audio/Processing`
- **Capabilities:**
  - **Dynamic Per-Input Channel Strips**: When upstream nodes connect to `VolumeNode`, the engine dispatches `on_input_sources_changed`. `VolumeNode` dynamically allocates channel strips labeled with each upstream node's custom name (e.g., "Firefox", "Microphone").
  - **Per-Channel Gain & Mute**: Each input strip has independent gain (-60 dB to +24 dB) and mute toggles with smoothed gain interpolation (`SmoothedFloat` with 4 ms linear ramps) to eliminate zipper noise and clicks.
  - **EBU R128 Loudness Auto-Normalization**: Optional automatic gain follower based on short-term gated RMS energy calculation. Gated below -45 dBFS noise floor to prevent amplifying background room noise during speech pauses. Dynamic range adjustment clamps normalization gain between -20 dB and +18 dB.
  - **Master Volume & Master Mute**: Overall output attenuation/boost (-60 dB to +12 dB) and instantaneous master mute.
  - **Exposed Functions**: `toggle_master_mute`, `set_master_volume_db`, `add_master_volume_db`, `toggle_auto_normalize`, `set_input_<i>_gain_db`, `add_input_<i>_gain_db`.

#### 4. `TimerNode.dll` (`at.builtin.control.timer`)
- **Category:** `Control/Triggers`
- **Capabilities:**
  - Control-plane periodic pulse generator with configurable `interval_ms`.
  - Fires control pulses across control wires to trigger functions on any connected target node (such as muting, volume increments, or mode toggles).

---

## 2. Technical Advisory: Mixing Firefox & Microphone via VB-Audio Virtual Cable

### User Question
> *"With the current architecture, using a VB virtual audio cable pair, is it possible to mix a Firefox output and microphone output into an output audio device like speakers or a game?"*

### Direct Answer
**Yes, absolutely.** Not only is it 100% possible with the current architecture, but AudioTool provides **two distinct ways** to accomplish this, one of which does not even require dedicating a virtual cable to capture Firefox!

---

### Method A: Native WASAPI Loopback Capture (Zero Cable Needed for Firefox)

Because AudioTool includes built-in WASAPI Loopback capture (`loopback:` prefix), AudioTool can listen directly to Windows' default playback stream without routing Firefox to a virtual cable.

1. **Firefox Output:** Plays normally to Windows Default Audio Output (e.g., *Realtek High Definition Audio*).
2. **AudioTool Node Graph:**
   - **Node 1 (`VirtualInputNode` - "Firefox Audio"):** Set `device_id` to `loopback:default_render` (or the specific Realtek Speakers loopback endpoint). This captures the audio playing through the speakers.
   - **Node 2 (`VirtualInputNode` - "Microphone"):** Set `device_id` to your physical microphone (e.g. *USB Microphone Capture*).
   - **Node 3 (`VolumeNode` - "Mixer"):** Connect Node 1 -> Mixer and Node 2 -> Mixer.
     - Adjust Firefox channel strip volume (-6 dB).
     - Adjust Microphone channel strip volume (0 dB).
     - Enable Auto-Normalization if desired to prevent game/browser audio from overpowering voice.
   - **Node 4 (`OutputNode` - "Virtual Cable To Game"):** Set `device_id` to `CABLE Input (VB-Audio Virtual Cable)`.
3. **In the Game or VoIP App (Discord, Zoom, CS2, etc.):**
   - Set Voice Input / Microphone Device to `CABLE Output (VB-Audio Virtual Cable)`.
4. **Result:** The game/VoIP call hears both your voice and Firefox audio mixed cleanly together, while you hear Firefox through your speakers normally.

---

### Method B: Dedicated Application Routing via VB-Cable (Isolated Browser Audio)

If you do *not* want system sounds, Discord chimes, or other apps included—only Firefox—you isolate Firefox using Windows App Volume settings:

```
[Firefox Browser]
       │ (Windows App Volume Routing)
       ▼
[CABLE Input (VB-Audio Virtual Cable)]
       │
       ▼ (VB-Audio Driver Loop)
[CABLE Output (VB-Audio Virtual Cable)]
       │
       ├───────────────────────────────────────┐
       ▼                                       ▼
[VirtualInputNode: Firefox]         [VirtualInputNode: Mic]
       │                                       │
       └───────────────────┬───────────────────┘
                           ▼
                    [VolumeNode]
             (Auto-Normalization & Mix)
                           │
             ┌─────────────┴─────────────┐
             ▼                           ▼
[OutputNode: Physical Speakers]   [OutputNode: Virtual Cable B / Game]
      (What You Hear)                   (What the Game/VoIP Hears)
```

#### Step-by-Step Configuration:
1. **Route Firefox to VB-Cable in Windows:**
   - Open Windows **Settings -> System -> Sound -> Volume mixer** (or `ms-settings:apps-volume`).
   - Start playing audio in Firefox.
   - Under *Apps*, locate *Firefox*, and change its Output Device from *Default* to `CABLE Input (VB-Audio Virtual Cable)`.
2. **Setup AudioTool Profile:**
   - **Input Node 1:** Name it `Firefox`. Set device to `CABLE Output (VB-Audio Virtual Cable)`.
   - **Input Node 2:** Name it `Microphone`. Set device to your physical microphone hardware.
   - **Volume Node:** Connect both `Firefox` and `Microphone` to `VolumeNode`.
     - `VolumeNode` will display:
       - Strip 1: `Firefox` [Gain Slider] [Mute] [Meter]
       - Strip 2: `Microphone` [Gain Slider] [Mute] [Meter]
       - Master: [Master Volume] [Master Mute] [Auto Normalize]
   - **Output Node 1:** Name it `Headphones/Speakers`. Set device to your physical playback device.
   - **Output Node 2:** (Optional) If you also have VB-Cable B (or VoiceMeeter VAIO), set this to `CABLE B Input` so games/friends hear the mix while you hear it on Output Node 1.
3. **Advantages with Current Architecture:**
   - **Independent Monitoring**: You can set different volume balances for yourself (Speakers) versus your stream/game by using two separate `VolumeNode`s.
   - **Zero Latency Clock Alignment**: AudioTool's `DeviceManager` uses WASAPI event-driven shared capture buffers with circular SPSC ring buffers, keeping latency under ~10-15 ms.
   - **Automatic Normalization**: Firefox video loudness fluctuates wildly on YouTube and Twitch. `VolumeNode`'s EBU R128 auto-normalizer keeps browser audio balanced with your microphone speech level automatically.

---

## 3. Frontend GUI Architecture (`at_gui.exe`)

The frontend is implemented using native Win32 + DirectX 11 + Dear ImGui.

### 3.1 Dual-Plane Infinite Canvas (`CanvasRenderer`)
- **Dual Planes:**
  - **Audio Plane**: Renders audio source, processing, and output nodes. Ports and wires are colored **Cyan / Neon Blue** (`#3CD0EA`).
  - **Control Plane**: Renders triggers, timers, and function control ports. Ports and wires are colored **Amber / Orange** (`#FF9F2A`).
  - **Plane Switching**: Toggle switch in top bar or press `Tab`.
- **Canvas Interaction:**
  - **Pan:** Middle-click drag or Right-click drag on empty canvas background.
  - **Zoom:** Mouse wheel zooms between `0.25x` (overview) and `3.0x` (detail), centered smoothly around the mouse cursor position.
  - **Grid:** Infinite background grid with 32 px minor cells and 128 px major cells, dynamically scaled with zoom level.

### 3.2 Dynamic Node Visual Forms (Form 1 & Form 2)
As specified in requirements, nodes feature dual representation:
- **Form 1 (Collapsed Strip):**
  - Compact header (height ~32 px) showing node title, plane badge, and mini I/O pins.
  - Double-click the header or click the `[+]` button to expand into Form 2.
- **Form 2 (Expanded Full Controls):**
  - Full table row view with all controls exposed by the node's reflection descriptors:
    - Sliders with formatted value displays (e.g. `dB`, `ms`).
    - Buttons and Toggle Buttons with active LED highlights.
    - Real-time Level Meters with green-to-red color gradients.
    - Device and channel Dropdown Combo Boxes.
  - Click `[-]` to collapse back to Form 1.

### 3.3 Wire Connection & Function Parameter Binding Modal
- **Wire Creation:** Left-click and drag from any output port snap point creates an active bezier wire. Hovering over a valid input port highlights the port green. Releasing connects the wire.
- **Cycle Prevention:** In-flight topological cycle detection rejects connections that would create feedback loops in the Audio Plane.
- **Wire Deletion:** Hovering over a wire reveals a centered `[×]` deletion cross; clicking it breaks the connection.
- **Function Parameter Binding Modal (Control Plane):**
  - Connecting a control trigger port to a node opens a modal dialog listing the target node's available functions.
  - For parameter functions (such as `set_master_volume_db` or `add_input_gain_db`), the user can specify the fixed parameter argument bound to that wire.

### 3.4 Multi-Profile Tab Management & Persistence
- The top bar displays open profiles as tabs.
- Users can create new profiles (`+` tab) or switch between them instantly.
- Profiles persist to `%LOCALAPPDATA%\Rouvr\AudioTool\profiles\<profile_name>.json`.
- Each profile stores:
  - Graph nodes (IDs, plugin IDs, custom names, canvas X/Y positions, expanded state).
  - Saved control parameter values (volume levels, device selections, mute states).
  - Audio and Control wiring topology.

---

## 4. Verification & Automated Test Status

All 6 automated tests pass cleanly (`ctest` and `at_poc_tests.exe`):

| Test | Description | Result |
|------|-------------|:------:|
| **Test 1** | Build ID generation, Registry key persistence, `%LOCALAPPDATA%` paths | **PASSED** |
| **Test 2** | Dynamic DLL Plugin loading & shadow copy verification (4 plugins) | **PASSED** |
| **Test 3** | Deterministic stereo audio pass-through accuracy (0 error) | **PASSED** |
| **Test 4** | Control Plane `TimerNode` pulse -> `VirtualInputNode::toggle_mute` | **PASSED** |
| **Test 5** | Runtime buffer size reload (256 -> 512) & WASAPI enumeration | **PASSED** |
| **Test 6** | `VolumeNode` multi-channel mixing, auto-normalization, and mute | **PASSED** |

---

## 5. Build & Launch Instructions

To build the full project, update the build ID, commit changes, and run the automated test suite:
```cmd
.\build.bat
```

To launch the native graphical application:
```cmd
.\build\bin\at_gui.exe
```

To run the headless verification tests:
```cmd
.\build\bin\at_poc_tests.exe
```
