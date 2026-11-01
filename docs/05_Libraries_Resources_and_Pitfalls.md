# 05 — Recommended Libraries, Resources & Real-Time Audio Pitfalls

## 1. Recommended C++ Libraries & Resources

To build AudioTool natively in C++20 on Windows with minimal bloat, permissive licensing (MIT / BSD / Public Domain), and low latency, the following stack is recommended:

### 1.1 Audio Device I/O & Decoding
| Library | Role | License | Why It Fits AudioTool |
| :--- | :--- | :--- | :--- |
| **`miniaudio`** (or direct **Windows WASAPI**) | Hardware Audio Capture, Render & Loopback | MIT-0 / Public Domain | Single-header C/C++ library with native Windows **WASAPI** (Shared, Exclusive, and Loopback modes), automatic MMCSS `"Pro Audio"` thread priority registration, device enumeration (`ma_context_get_devices`), and built-in resamplers. Can be used alongside direct `IMMNotificationClient` COM calls for hot-plug detection. |
| **`dr_libs`** (`dr_wav`, `dr_mp3`, `dr_flac`) + **`stb_vorbis`** | Audio File Decoding for `PlaybackNode` | Public Domain | Zero-dependency single-header decoders (bundled with `miniaudio`) enabling fast RAM decoding and background disk streaming for soundboard clips. |
| **`libsamplerate`** (Secret Rabbit Code) | High-Quality Adaptive Resampling (ASRC) | BSD-2-Clause | Supports smooth time-varying resampling ratios (`src_set_ratio`) needed to compensate for hardware clock drift between multiple Windows audio devices. |

### 1.2 DSP & Loudness Measurement
| Library | Role | License | Why It Fits AudioTool |
| :--- | :--- | :--- | :--- |
| **`libebur128`** | Loudness Measurement for `VolumeNode` Normalization | MIT | Implements the ITU-R BS.1770 / EBU R128 standard (K-weighted loudness + relative silence gating). Essential for `VolumeNode` to normalize across multiple inputs accurately without boosting background hiss during silence. |
| **Custom SIMD (`<immintrin.h>`)** or **`xsimd`** | Planar Buffer Mixing & Gain Ramping | BSD-3-Clause | Portable C++ wrappers around `SSE4.2` / `AVX2` intrinsics for 64-byte aligned planar `float` buffers. |

### 1.3 Lock-Free Concurrency & Data Structures
| Library | Role | License | Why It Fits AudioTool |
| :--- | :--- | :--- | :--- |
| **`moodycamel::ReaderWriterQueue`** | Single-Producer Single-Consumer (SPSC) Lock-Free Queue | BSD-2-Clause | Ultra-fast wait-free ring buffer for sending telemetry/meters from `AudioThread` to GUI and streaming disk buffers into `PlaybackNode`. |
| **`moodycamel::ConcurrentQueue`** | Multi-Producer Single-Consumer (MPSC) Lock-Free Queue | BSD-2-Clause | Pre-allocating lock-free queue for funneling control events from both `ControlThread` and `EngineControlThread` into `AudioThread`. |

### 1.4 GUI, Windowing & Serialization
| Library | Role | License | Why It Fits AudioTool |
| :--- | :--- | :--- | :--- |
| **Dear ImGui (Docking Branch) + Win32 / DirectX 11** | GUI Framework & Custom 2D Canvas | MIT | Hardware-accelerated via DX11, zero layout lag at 60–144 FPS with live audio meters, native support for cascading hover context menus, tabs, modal popups, and direct `ImDrawList` vector drawing for custom 2-form table nodes and wires with midpoint `[×]` delete buttons. |
| **`nlohmann/json`** | Workspace Profile (`.atprof`) Serialization | MIT | Industry-standard C++ JSON library with clean DOM/struct mapping for saving and loading profiles. |
| **`nativefiledialog-extended` (`nfd`)** | Native Windows File Picker (`IFileDialog`) | Zlib | Used by `AT_CTRL_FILE_SELECTOR` controls and `File -> Load/Save Profile` to open the native Windows 11 Explorer dialog. |

---

## 2. Common Real-Time Audio Pitfalls & Engineering Mitigations

Requirement 43 states:
> *"Research existing libraries for audio manipulation, research common pitfalls when processing audio - like clock desync."*

Below are the **7 most critical pitfalls** in a multi-device modular Windows audio application and how AudioTool's architecture solves each one.

---

### Pitfall 1: Hardware Clock Desynchronization (Clock Drift)
#### The Problem:
AudioTool allows a user to capture from **Device A** (e.g., a USB Microphone) and output to **Device B** (e.g., a Virtual Audio Cable or PCIe Soundcard), or even output to **Device B** and **Device C** simultaneously.
- Even if both devices are configured to `48,000 Hz`, each physical device has its own quartz crystal oscillator (or independent kernel clock).
- Device A might actually run at `48,001.8 Hz` (+37.5 ppm), while Device B runs at `47,999.1 Hz` (-18.7 ppm).
- Over 60 seconds, Device A produces **162 more samples** than Device B consumes. Without drift compensation, the intermediate buffer will inevitably either **overflow** (dropping chunks of audio) or **underflow** (producing periodic crackles/dropouts every few seconds).

#### The Engineering Mitigation:
1. **Single Master Audio Clock:** `HardwareDeviceManager` designates one primary render endpoint (or a high-precision MMCSS clock) as the **Master Graph Clock** that triggers `AudioThread` ticks.
2. **Per-Device Drift-Compensating Ring Buffer + ASRC:**
   - Every other capture and render endpoint runs asynchronously into/out of a lock-free ring buffer coupled to an **Adaptive Asynchronous Sample Rate Converter (ASRC)**.
   - An exponential moving average filter measures the ring buffer's fill level $L(t)$ every callback and compares it to the target fill level $L_{\text{target}} = 1.5 \times \text{BufferSize}$.
   - A low-bandwidth **PI (Proportional-Integral) Controller** computes a smoothed resampling ratio adjustment:
     $$r(t) = \frac{f_{\text{device}}}{f_{\text{engine}}} + K_p e(t) + K_i \int e(\tau)\,d\tau$$
   - Because $K_p$ and $K_i$ are tuned for a very slow time constant ($\tau \approx 2\text{--}5\text{ seconds}$), pitch variation is $< 0.005\%$ (completely imperceptible to human ears) while buffer underruns/overruns are completely eliminated over 24/7 operation.

---

### Pitfall 2: Priority Inversion & Hidden Locks on the Audio Thread
#### The Problem:
At `128` samples and `48 kHz`, the entire audio graph must finish executing within **2.66 ms**. If the real-time `AudioThread` ever calls:
- `std::mutex::lock()` (even if contended for only a few microseconds by a lower-priority GUI thread preempted by the OS),
- `malloc()` / `new` / `free()` / `delete` (which acquire the Windows global CRT heap lock),
- `std::vector::push_back()` (which triggers heap reallocation), or
- `printf` / file logging,
then **Priority Inversion** occurs: the time-critical audio thread stalls waiting for a normal-priority thread, causing an audible buffer underrun (glitch/pop).

#### The Engineering Mitigation:
- **RCU (Read-Copy-Update) Immutable Execution Plans:** Graph topology mutations happen on `EngineControlThread`, which allocates a brand-new `AudioExecutionPlan` on the heap and swaps a single `std::atomic<AudioExecutionPlan*>` pointer.
- **Lock-Free Control Queue:** Control plane function invocations and GUI slider changes are pushed as fixed-size POD structs (`32 bytes`) into a pre-allocated lock-free ring buffer and drained at the start of the audio tick.
- **Deferred Garbage Collection:** Old `AudioExecutionPlan` instances and unloaded nodes are pushed onto a lock-free retire list and freed by `EngineControlThread`, never by `AudioThread`.

---

### Pitfall 3: Zipper Noise & Clicks on Control Changes
#### The Problem:
Suppose a `TimerControlNode` triggers `"Set Master Volume to -20 dB"` or a user drags a volume slider rapidly. If the gain multiplier inside `VolumeNode` jumps instantaneously from `1.0f` (`0 dB`) to `0.1f` (`-20 dB`) between sample `n` and sample `n+1`, the step discontinuity in the waveform creates a broadband high-frequency **click / pop ("zipper noise")**.

#### The Engineering Mitigation:
- **Per-Sample Linear / Exponential Gain Smoothing (`AT_SmoothedFloat`):**
  - Provide a tiny header-only `AT_SmoothedFloat` utility in `include/at_sdk/`:
    - When `set_target_value(new_gain)` is called, the gain transitions smoothly over a configurable ramp time (default `10 ms`, i.e., `480 samples` at 48 kHz).
    - Mute/Unmute and Runtime Reload pause/resume transitions also apply a `5 ms` half-cosine fade ramp.

---

### Pitfall 4: Denormal (Subnormal) Floating-Point CPU Spikes
#### The Problem:
Recursive DSP algorithms (such as the comb/allpass filters in `ReverbNode`, Biquad filters in `EqualizerNode`, or exponential RMS detectors in `VolumeNode`) exponentially decay toward zero when audio input stops.
- Once internal state variables decay below $1.175 \times 10^{-38}$ (`FLT_MIN`), x86/x64 CPUs enter **subnormal/denormal microcode handling**, which slows floating-point arithmetic down by **10x to 50x**!
- A reverb node that uses 0.5% CPU during active speech can suddenly spike to 25% CPU during silence.

#### The Engineering Mitigation:
- At the entry of `AudioThread` (and inside the engine's thread initialization), explicitly set the hardware SSE/AVX control register flags:
  ```cpp
  _mm_setcsr(_mm_getcsr() | 0x8040); // Enables DAZ (Denormals-Are-Zero) and FTZ (Flush-To-Zero)
  ```

---

### Pitfall 5: Naive Loudness Normalization Boosting Noise Floor
#### The Problem:
`VolumeNode` is required to *"normalize sound volume across all input nodes"*.
- If Input 1 is a microphone and the user stops talking for 3 seconds, the RMS level of Input 1 drops to `-65 dBFS` (room noise / preamp hiss).
- A naive normalizer (`gain = target_rms / current_rms`) will apply **+49 dB of gain** to the quiet microphone, blasting loud white noise into the output and causing severe pumping when the user speaks again!

#### The Engineering Mitigation:
- **Gated Loudness Follower with Bounded Gain:**
  1. **Noise Gate Threshold (`-50 dBFS`):** If an input bus's short-term level is below `-50 dBFS`, the normalizer freezes its current gain estimate for that input rather than chasing the noise floor upward.
  2. **Maximum Boost Clamp (`+18 dB`):** Caps maximum automatic upward gain to prevent runaway amplification.
  3. **Asymmetric Attack/Release Ballistics:** Fast attack (`50 ms` to duck sudden loud peaks) and slow release (`800 ms` for smooth recovery), following EBU R128 / ITU-R BS.1770 K-weighting.

---

### Pitfall 6: Windows DLL File Locking & Hot-Reload Crashes
#### The Problem:
When Windows loads `plugins/Audio/VolumeNode.dll` via `LoadLibraryW`, the NTFS file handle remains locked until `FreeLibrary` is called. Furthermore, if `FreeLibrary` is called while `AudioThread` is still executing `process_audio()` inside that DLL's code segment, the process crashes immediately with an `ACCESS_VIOLATION`.

#### The Engineering Mitigation:
1. **Shadow Copy Before Load:** `PluginManager` never calls `LoadLibraryW` on the original file in `plugins/`. It copies `Foo.dll` to `%TEMP%/AudioTool_PluginCache/Foo_<UUID>.dll` and loads the shadow copy.
2. **Drain Audio Thread Before `FreeLibrary`:** When unloading or hot-reloading a plugin:
   - First, compile and atomically swap in a new `AudioExecutionPlan` that excludes all instances of that plugin.
   - Wait for the `AudioThread` epoch counter to advance by at least 1 tick (guaranteeing `AudioThread` is no longer inside the old plan).
   - Destroy the node instances on `EngineControlThread`, and only then call `FreeLibrary(hModule)`.

---

### Pitfall 7: WASAPI Exclusive vs. Shared Mode Format Mismatches
#### The Problem:
In **WASAPI Shared Mode** (the standard mode needed when sharing a microphone or Virtual Audio Cable with Discord/OBS/games), Windows Audio Engine (`audiodg.exe`) locks the endpoint to the mix format configured in Windows Sound Control Panel (e.g., `48000 Hz, 2ch, 32-bit float` or `44100 Hz, 1ch, 16-bit PCM`). Calling `IAudioClient::Initialize` with a different sample rate or channel count without conversion flags fails with `AUDCLNT_E_UNSUPPORTED_FORMAT`.

#### The Engineering Mitigation:
- Always query `IAudioClient::GetMixFormat()` on each Windows device and open the WASAPI endpoint in its native mix format (or use `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`), while letting our `HiddenMasterDevice` layer handle channel mapping (e.g., Mono Mic $\rightarrow$ Stereo internal bus) and sample rate conversion into the engine's unified internal format (`32-bit planar float`).

