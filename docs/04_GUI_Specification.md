# 04 — GUI Architecture & User Experience Specification

## 1. Application Window Layout

The AudioTool GUI (`at_gui`) is a native Windows application (DirectX 11 + Win32 backend with custom immediate-mode canvas rendering) structured into three main regions:

```text
+-----------------------------------------------------------------------------------+
| File   Engine   View   Help                     [ Audio Stream View | Data View ] | <- Menu & Mode Bar
+-----------------------------------------------------------------------------------+
| [● Profile 1: Stream Setup  x] [● Profile 2: Soundboard  x] [ + New Profile Tab ] | <- Multi-Profile Tab Bar
+-----------------------------------------------------------------------------------+
|                                                                                   |
|    +-----------------------------+              +-----------------------------+   |
|    | Mic Input       [_][✂][X]  |              | Main Mixer      [_][✂][X]  |   |
|    +-----------------------------+              +-----------------------------+   |
|    | Device: [Blue Yeti      v]  |              | In 1 (Mic):  [====|==] 0dB  |   |
|   (o) Gain:  [======|====] +3dB (o)----[×]---->(o) Normalize:  [ ON ]        (o)  | <- 2D Panning Canvas
|    | Mute:   [ OFF ]             |              | Master Vol:  (  O  )   0dB  |   |
|    +-----------------------------+              +-----------------------------+   |
|                                                                                   |
|                                                                                   |
+-----------------------------------------------------------------------------------+
| Engine: Running (48kHz, 256 smp / 5.3ms) | CPU: 1.8% | Active Devices: 2          | <- Status Bar
+-----------------------------------------------------------------------------------+
```

### 1.1 Top Menu Bar
- **File Menu:**
  - `New Profile` (`Ctrl+N`) — Creates a fresh workspace profile in a new tab.
  - `Load Profile...` (`Ctrl+O`) — Opens a `.atprof` JSON file into a new tab and starts its graph on the backend.
  - `Save Profile` (`Ctrl+S`) — Saves the currently focused tab's profile.
  - `Save Profile As...` (`Ctrl+Shift+S`) — Saves the current tab to a new `.atprof` file path.
  - `Close Profile Tab` (`Ctrl+W`) — Stops and unloads the current tab's profile.
  - `Exit` (`Alt+F4`).
- **Engine Menu:**
  - `Audio Engine Settings...` — Runtime configuration modal for `Buffer Size`, `Sample Rate`, and `Max Connections Per Node` with an **[Apply & Reload Engine]** button.
  - `Rescan Plugin Folder` — Reloads the `plugins/` filesystem hierarchy without restarting.

### 1.2 Multi-Profile Tab Bar
- Displays one tab per loaded workspace profile.
- Each tab shows:
  - **Activity LED (`●`)**: Green when the profile's audio/control graph is actively running; click to toggle pause/mute on that profile independently.
  - **Profile Name** (with `*` dirty indicator if unsaved changes exist).
  - **Close Tab (`×`)** button.
- Switching tabs immediately switches the canvas to that profile's nodes, connections, and pan/zoom coordinates while all other tabs continue executing in the background on the engine.

---

## 2. Dual-Plane Canvas: Audio Stream View vs. Control/Data View

Requirement 59 states:
> *"The user should be able to switch between audio stream view and data view, both look alike and use the same principles, only with a color difference."*

### 2.1 Plane Visibility & Color Coding

| Feature | Audio Stream View (`AUDIO` Mode) | Control / Data View (`CONTROL` Mode) |
| :--- | :--- | :--- |
| **Canvas Background Grid** | Deep Slate Charcoal (`#181A1F`, grid lines `#252830`) | Deep Indigo/Teal Tint (`#141D24`, grid lines `#1F2E38`) |
| **Node Header Accent** | Cool Cyan/Blue (`#2B6CB0`) | Warm Amber/Orange (`#D97706`) |
| **Snap Point & Wire Color** | Electric Cyan (`#00E5FF`) | Vibrant Amber (`#FFB300`) |
| **Visible Nodes** | **Audio Nodes Only** (`Source`, `Processing`, `Output`). Pure `Control` nodes are either hidden or dimmed at 25% opacity (non-interactive) to keep the audio routing view clean. | **All Nodes** (`Control` nodes + `Audio` nodes). Why? Because every `Audio` node inherits from `INode` and has Control Input/Output ports so Control nodes (like `TimerNode`) can connect to Audio nodes! |
| **Snap Point Meaning** | Left Dot = Audio Stream Input (`0..max_conns`)<br>Right Dot = Audio Stream Output (`0..max_conns`) | Left Dot = Control Plane Input (Function Trigger Receiver)<br>Right Dot = Control Plane Output (Pulse/Trigger Emitter) |
| **Visible Connections** | Audio stream wires only | Control plane wires (with bound function badge tooltip) |

---

## 3. Node Visual Design: The Two Forms & Table Layout

Requirements 55–56 & 61–62 specify:
> *"Each node is a rectangle with 2 forms - In both forms there is a small topbar with the options to - minimize/maximize the node, disconnect all inputs/outputs, delete. 1st form is a small rectangle with only name, input snap point and output snap point displayed, 2nd form is an expanded form with all the controls displayed - sliders, knobs, input boxes, selectors..."*
> *"Input snap point is a small dot always on the left middle, output on the right middle."*
> *"To keep things simple, nodes are simple rectangles, their control inputs are stacked as rows under each other in a 'table'. So slider, then under that another control module and so on. Keep the width of the widest element."*

### 3.1 Form 1: Minimized / Compact Form

```text
      +------------------------------------+
      |  Main Volume Mixer    [+] [✂] [X]  |
     (●)                                  (●)
      +------------------------------------+
```
- **Dimensions:** Height = `36 px` (fixed); Width = `max(160 px, TextWidth(NodeName) + 90 px)`.
- **Topbar Buttons (Right-aligned):**
  1. **`[+]` (Maximize):** Expands node to Form 2.
  2. **`[✂]` (Disconnect All):** Immediately disconnects all inputs and outputs of this node on the currently active plane (or both planes if Shift-clicked).
  3. **`[X]` (Delete):** Removes the node and all its connections from the profile.
- **Snap Points:**
  - **Input Snap Point `(●)`**: Radius `6 px`, positioned at `(node_rect.left, node_rect.center_y)`. (Hidden on Audio Source nodes in Audio View since they have 0 audio inputs).
  - **Output Snap Point `(●)`**: Radius `6 px`, positioned at `(node_rect.right, node_rect.center_y)`. (Hidden on Audio Output nodes in Audio View since they have 0 audio outputs).

### 3.2 Form 2: Expanded Table-Row Form

```text
      +--------------------------------------------------+
      |  Main Volume Mixer                  [_] [✂] [X]  | <- Header Row
      +--------------------------------------------------+
      |  In 1 (Mic)        [========|=======]   +0.0 dB  | <- Row 1 (Horizontal Slider)
      +--------------------------------------------------+
      |  In 2 (Soundboard) [====|===========]   -6.0 dB  | <- Row 2 (Horizontal Slider)
      +--------------------------------------------------+
     (●) Auto Normalize    [       ON       ]           (●) <- Snap Points at Vertical Center!
      +--------------------------------------------------+
      |  Target LUFS       [ -16.0       ][▲/▼]     dB   | <- Row 4 (Spinbox Range Input)
      +--------------------------------------------------+
      |  Master Volume          (  ●  )         +0.0 dB  | <- Row 5 (Rotary Knob)
      +--------------------------------------------------+
```

- **Strict Table Row Stacking Rule:**
  1. Each `AT_ControlDescriptor` exposed by the node occupies **one horizontal row** (`row_index = 0 .. control_count - 1`) stacked vertically with a `1 px` subtle separator border between rows.
  2. Each row has two internal cells:
     - **Left Cell:** Control `display_name` label (`padding_left = 12 px`).
     - **Right Cell:** Interactive widget rendered according to `(control_type, preferred_style)` plus value readout / unit label (`padding_right = 12 px`).
  3. **Dynamic Width Calculation ("Keep the width of the widest element"):**
     - Before drawing the node frame, the layout pass measures the required natural width of the Topbar and every control row `i`:
       $$\text{NodeWidth} = \max\left(\text{HeaderMinWidth}, \max_{i=0}^{K-1} \left(\text{LabelWidth}_i + \text{Gap} + \text{ControlWidgetMinWidth}_i\right)\right) + 2 \times \text{HorizontalPadding}$$
     - Every row in the table then stretches to fill `NodeWidth` cleanly.
  4. **Snap Points in Form 2:**
     - Remain anchored at the exact vertical midpoint of the expanded rectangle:
       - Left Input Dot: `(node_rect.left, (node_rect.top + node_rect.bottom) * 0.5f)`
       - Right Output Dot: `(node_rect.right, (node_rect.top + node_rect.bottom) * 0.5f)`

---

## 4. Connection Wires & Interactive Routing

Requirements 57–59 state:
> *"The user is able to connect input to outputs and vice versa by clicking one, than the other, or dragging. This creates a visual line between these two. The line has a small cross in the middle to delete it and an arrow at the end signaling its direction."*

### 4.1 Two Supported Connection Gestures
The canvas state machine (`WireInteractionState`) supports both connection styles seamlessly in **either direction** (Output $\rightarrow$ Input OR Input $\rightarrow$ Output):
1. **Click-then-Click Mode:**
   - User left-clicks once (and releases within `< 150 ms` and `< 4 px` movement) on any Snap Point `A` (either an Input dot or an Output dot).
   - State enters `CONNECTING_CLICK_MODE`: a live preview line follows the mouse cursor from `A` to `mouse_pos`.
   - User left-clicks on a complementary Snap Point `B` (if `A` was Input, `B` must be Output; if `A` was Output, `B` must be Input). The connection is created!
   - Clicking on empty canvas or pressing `Esc` / Right-Click cancels the pending wire.
2. **Press-and-Drag Mode:**
   - User presses left mouse button on Snap Point `A` and drags (`> 4 px`) toward Snap Point `B`.
   - Releasing the mouse button over a valid complementary Snap Point `B` completes the connection.

### 4.2 Visual Wire Geometry
```text
   Source Output                                       Target Input
       (●)===================[ × ]===================►(●)
                             ^                         ^
                  Clickable Delete Cross        Directional Arrowhead
                    at exact midpoint            pointing into Input
```
- **Geometry:** Smooth anti-aliased line (`thickness = 2.5 px`) or cubic Bezier curve between `P_out = SourceNode.RightDot` and `P_in = TargetNode.LeftDot`.
- **Directional Arrow (`►`):** Regardless of whether the user dragged from Input-to-Output or Output-to-Input, signal flow is always `Output -> Input`. A filled triangle arrowhead (`length = 10 px, width = 8 px`) is drawn at `P_in` pointing into the receiver node's left snap point.
- **Midpoint Delete Cross (`[×]`):**
  - At the midpoint $M = 0.5 \cdot (P_{\text{out}} + P_{\text{in}})$, a circular badge (`radius = 8 px`) with a crisp `×` icon is drawn.
  - Hovering over `[×]` highlights the badge in red (`#E53E3E`).
  - Left-clicking `[×]` immediately dispatches `CMD_DISCONNECT_AUDIO` (or `CMD_DISCONNECT_CONTROL`) to remove the wire.

---

## 5. Control Plane Function Binding Workflow

Requirement 59 specifies:
> *"When connecting a data output to a node, the receiver node should on select display a context menu with a list of all functions available - where a integer/float value is needed, the number can be supplied with a popup dialog upon selecting the function."*

### 5.1 Step-by-Step UX Flow in Data/Control View
1. **Initiate Connection:** User switches to **Data View** and connects `TimerNode` (Output Dot) to `VolumeNode` (Input Dot) via click-click or drag.
2. **Function Selection Context Menu:**
   - As soon as the receiver node (`VolumeNode`) is selected as the connection endpoint, a **Context Menu** pops up directly at the cursor listing all functions returned by `VolumeNode->get_functions()`:
     ```text
     +------------------------------------------+
     | Bind Trigger to Function on 'Main Mixer' |
     +------------------------------------------+
     |   Toggle Master Mute                     |
     |   Toggle Auto-Normalize                  |
     |   Normalize Inputs Snapshot Now          |
     |   Set Master Volume (dB)...              |
     |   Change Master Volume By (dB)...        |
     |   Set Input 1 Volume (dB)...             |
     |   Change Input 1 Volume By (dB)...       |
     +------------------------------------------+
     ```
3. **Case A — Parameterless Function (`parameter_type == AT_VAL_NONE`):**
   - Example: User clicks `"Toggle Master Mute"`.
   - The connection is immediately finalized and `CMD_CONNECT_CONTROL` is sent to the backend.
4. **Case B — Parameterized Function (`AT_VAL_INT` or `AT_VAL_FLOAT`):**
   - Example: User clicks `"Change Master Volume By (dB)..."`.
   - A modal **Parameter Input Popup Dialog** opens immediately:
     ```text
     +-------------------------------------------------+
     | Configure Parameter: Change Master Volume By    |
     +-------------------------------------------------+
     | Delta Volume (dB)  [-72.0 to +72.0]:            |
     | [ -10.0                                       ] |
     |                                                 |
     |              [ Cancel ]   [ Bind Function ]     |
     +-------------------------------------------------+
     ```
   - The input box validates that the entered `int` or `float` lies within `[func.min_value, func.max_value]`.
   - Clicking **[Bind Function]** (or pressing `Enter`) dispatches `CMD_CONNECT_CONTROL` with the bound value (`-10.0f`) and creates the control wire.

---

## 6. Filesystem-Mirrored Right-Click Node Spawning Menu

Requirement 54 specifies:
> *"Option to create a new node - select from available nodes, right click context menu that on hover displays folders of nodes like they are present on the filesystem."*

### 6.1 How the Plugin Directory Tree Maps to the Context Menu
The `PluginManager` scans `%LOCALAPPDATA%\Rouvr\AudioTool\plugins\` recursively on startup (and on rescan). The directory structure on disk directly dictates the cascading submenus when the user **right-clicks on empty canvas space**:

```text
Filesystem under %LOCALAPPDATA%\Rouvr\AudioTool\plugins\   Right-Click Context Menu on Canvas
--------------------------------------------------------   ----------------------------------
plugins/                                                   +--------------------+
├── Audio/                                                 | Create Node      ▸ |
│   ├── Sources/                                           | Center Canvas      |
│   │   ├── VirtualInput.dll                               +--------------------+
│   │   └── PlaybackSoundboard.dll                                   |
│   ├── Processing/                                                  v
│   │   ├── VolumeMixer.dll                                +--------------------+
│   │   └── Effects/                                       | 📁 Audio         ▸ |----+
│   │       ├── ParametricEQ.dll                           | 📁 Control       ▸ |    |
│   │       └── StereoReverb.dll                           +--------------------+    |
│   └── Outputs/                                                                     v
│       └── AudioDeviceOutput.dll                                          +--------------------+
└── Control/                                                               | 📁 Sources       ▸ |
    └── Triggers/                                                          | 📁 Processing    ▸ |----+
        └── TimerPulse.dll                                                 | 📁 Outputs       ▸ |    |
                                                                           +--------------------+    |
                                                                                                     v
                                                                                           +--------------------+
                                                                                           | Volume Mixer       |
                                                                                           | 📁 Effects       ▸ |
                                                                                           +--------------------+
```

- Clicking any leaf item spawns that node at the exact canvas world coordinates where the user originally right-clicked.
- If the user is currently in **Audio Stream View** and spawns a **Control Node** (e.g., `TimerPulse`), the UI either prompts/switches to **Data View** automatically or displays a toast notification (`"Timer Node created in Data View"`).

