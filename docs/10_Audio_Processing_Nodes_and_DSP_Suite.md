# AudioTool DSP & Audio Processing Nodes Specification

This document provides a comprehensive technical specification for the 11 core audio processing and DSP effects nodes integrated into AudioTool:
1. **16-Band Equalizer** (`at.builtin.audio.equalizer`)
2. **Multi-Mode Filter** (`at.builtin.audio.filter`)
3. **Dynamic Range Compressor** (`at.builtin.audio.compressor`)
4. **Hard-Ceiling Limiter** (`at.builtin.audio.limiter`)
5. **Noise Gate** (`at.builtin.audio.noise_gate`)
6. **Digital Reverb (Freeverb)** (`at.builtin.audio.reverb`)
7. **Stereo Delay & Echo** (`at.builtin.audio.delay`)
8. **Hard-Clipping Distortion** (`at.builtin.audio.distortion`)
9. **Tube-Style Overdrive** (`at.builtin.audio.overdrive`)
10. **Tape Saturation** (`at.builtin.audio.saturation`)
11. **Bitcrusher & Decimator** (`at.builtin.audio.bitcrusher`)

All nodes are compiled as dynamic, hot-reloadable C ABI plugin DLLs deployed to `%LOCALAPPDATA%\Rouvr\AudioTool\plugins\` and organized taxonomically into filesystem-mirrored folders.

---

## 1. Architecture & Real-Time DSP Guarantees

All audio processing algorithms adhere strictly to real-time audio standards:
- **Zero Allocations on Audio Thread:** All scratch buffers, delay lines, filter states, and comb arrays are pre-allocated during `prepare_audio()`. The `process_audio()` method never invokes `malloc`, `new`, `realloc`, or OS synchronization primitives.
- **Denormal & Subnormal Protection:** Floating-point underflows can trigger CPU x87/SSE pipeline stalls. All filters incorporate Flush-To-Zero (FTZ), Denormals-Are-Zero (DAZ) flags, and anti-denormal offsets (`1e-25f`).
- **Planar SIMD Audio:** Stream data is processed as 64-byte aligned planar 32-bit floating-point audio buffers (`float* const* channel_ptrs`).
- **Parameter Smoothing:** Gain and volume controls utilize `at::SmoothedFloat` exponential/linear ramp followers to eliminate zipper noise and clicks when sliders are adjusted live.

---

## 2. Shared DSP Engine (`include/at_sdk/at_dsp.h`)

The shared DSP header encapsulates industry-standard, battle-tested open-source algorithms:

### 2.1 Robert Bristow-Johnson (RBJ) Audio EQ Cookbook Biquads
Implemented using Transposed Direct Form II for superior numerical precision and low quantization noise:
```text
y[n]  = b0 * x[n] + s1[n-1]
s1[n] = b1 * x[n] - a1 * y[n] + s2[n-1]
s2[n] = b2 * x[n] - a2 * y[n]
```
Supported configurations:
- `LowPass` & `HighPass` (Butterworth / resonant 2-pole and 4-pole cascaded)
- `BandPass` (constant 0 dB peak gain)
- `Notch` (narrow band rejection)
- `PeakingEQ` (parametric boost/cut with $Q$ bandwidth)
- `LowShelf` & `HighShelf` (smooth low/high shelf shaping)

### 2.2 Freeverb Reverberator Architecture
Based on Jezar at Dreampoint's public domain architecture:
- **8 Parallel Lowpass Feedback Comb Filters (LBCF)** per channel with stereo-spread tuning lengths (1116 to 1617 samples at 44.1 kHz, scaled for arbitrary sample rates).
- **4 Cascaded Allpass Diffusers** per channel to maximize echo density and eliminate metallic flutter.
- Real-time parameter controls for room size, high-frequency damping, stereo width, wet/dry blend, and freeze mode.

### 2.3 Dynamics Processing
- **Compressor:** Based on JAES Giannoulis/Massberg/Reiss digital dynamic range compressor topologies. Employs a logarithmic decibel-domain static curve with adjustable soft-knee quadratic interpolation, decoupled attack/release ballistics, and continuous gain reduction metering.
- **Limiter:** Hard ceiling peak limiter with zero-overshoot instantaneous peak envelope detection and smooth exponential release to prevent clipping distortion.
- **Noise Gate:** Hysteresis state machine (`Closed`, `Opening`, `Open`, `Holding`, `Releasing`) with independent attack, hold, and release envelopes to prevent chatter on soft audio tails.

---

## 3. Node Specifications & Parameters

### 3.1 16-Band Equalizer (`at.builtin.audio.equalizer`)
- **Plugin Path:** `Audio/EQ/EqualizerNode.dll`
- **Category Path:** `Audio/EQ`
- **Center Frequencies (ISO 2/3-Octave):**
  `25 Hz`, `40 Hz`, `63 Hz`, `100 Hz`, `160 Hz`, `250 Hz`, `400 Hz`, `630 Hz`, `1.0 kHz`, `1.6 kHz`, `2.5 kHz`, `4.0 kHz`, `6.3 kHz`, `10.0 kHz`, `16.0 kHz`, `20.0 kHz`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `master_gain` (Slider, float, range `[-18.0, 18.0]` dB, step `0.5`, default `0.0` dB)
  - `band_0` .. `band_15` (16 Sliders, float, range `[-18.0, 18.0]` dB, step `0.5`, default `0.0` dB)

### 3.2 Multi-Mode Filter (`at.builtin.audio.filter`)
- **Plugin Path:** `Audio/Filters/FilterNode.dll`
- **Category Path:** `Audio/Filters`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `filter_type` (Dropdown Combo, int, `0: Low-pass`, `1: High-pass`, `2: Band-pass`)
  - `cutoff_hz` (Logarithmic Slider, float, range `[20.0, 20000.0]` Hz, default `1000.0` Hz)
  - `resonance_q` (Slider, float, range `[0.1, 10.0]`, default `0.707`)
  - `slope` (Dropdown Combo, int, `0: 12 dB/oct (2-pole)`, `1: 24 dB/oct (4-pole cascaded)`)
  - `output_gain_db` (Slider, float, range `[-18.0, 18.0]` dB, default `0.0` dB)

### 3.3 Dynamic Range Compressor (`at.builtin.audio.compressor`)
- **Plugin Path:** `Audio/Dynamics/CompressorNode.dll`
- **Category Path:** `Audio/Dynamics`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `threshold_db` (Slider, float, range `[-60.0, 0.0]` dB, default `-18.0` dB)
  - `ratio` (Slider, float, range `[1.0, 20.0]`:1, default `4.0`:1)
  - `attack_ms` (Slider, float, range `[0.1, 200.0]` ms, default `10.0` ms)
  - `release_ms` (Slider, float, range `[10.0, 1000.0]` ms, default `100.0` ms)
  - `knee_db` (Slider, float, range `[0.0, 20.0]` dB, default `6.0` dB)
  - `makeup_gain_db` (Slider, float, range `[0.0, 24.0]` dB, default `0.0` dB)
  - `gr_meter` (Read-only Level Meter, range `[-30.0, 0.0]` dB)

### 3.4 Hard-Ceiling Limiter (`at.builtin.audio.limiter`)
- **Plugin Path:** `Audio/Dynamics/LimiterNode.dll`
- **Category Path:** `Audio/Dynamics`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `ceiling_db` (Slider, float, range `[-12.0, 0.0]` dB, default `-0.1` dB)
  - `input_gain_db` (Slider, float, range `[-12.0, 24.0]` dB, default `0.0` dB)
  - `release_ms` (Slider, float, range `[1.0, 500.0]` ms, default `50.0` ms)
  - `peak_meter` (Read-only Level Meter, range `[-60.0, 6.0]` dB)

### 3.5 Noise Gate (`at.builtin.audio.noise_gate`)
- **Plugin Path:** `Audio/Dynamics/NoiseGateNode.dll`
- **Category Path:** `Audio/Dynamics`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `threshold_db` (Slider, float, range `[-80.0, 0.0]` dB, default `-45.0` dB)
  - `attack_ms` (Slider, float, range `[0.1, 50.0]` ms, default `2.0` ms)
  - `hold_ms` (Slider, float, range `[0.0, 500.0]` ms, default `50.0` ms)
  - `release_ms` (Slider, float, range `[5.0, 1000.0]` ms, default `150.0` ms)
  - `floor_db` (Slider, float, range `[-96.0, 0.0]` dB, default `-80.0` dB)
  - `gate_open` (Status Indicator, bool)

### 3.6 Digital Reverb (`at.builtin.audio.reverb`)
- **Plugin Path:** `Audio/Effects/ReverbNode.dll`
- **Category Path:** `Audio/Effects`
- **Flags:** `AT_NODE_FLAG_PRODUCES_TAIL`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `room_size` (Slider, float, range `[0.0, 1.0]`, default `0.6`)
  - `damping` (Slider, float, range `[0.0, 1.0]`, default `0.4`)
  - `width` (Slider, float, range `[0.0, 1.0]`, default `1.0`)
  - `wet_db` (Slider, float, range `[-60.0, 0.0]` dB, default `-12.0` dB)
  - `dry_db` (Slider, float, range `[-60.0, 0.0]` dB, default `0.0` dB)
  - `freeze` (Toggle Button, bool, default `false`)

### 3.7 Stereo Delay & Echo (`at.builtin.audio.delay`)
- **Plugin Path:** `Audio/Effects/DelayNode.dll`
- **Category Path:** `Audio/Effects`
- **Flags:** `AT_NODE_FLAG_PRODUCES_TAIL`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `delay_ms` (Slider, float, range `[1.0, 2000.0]` ms, default `350.0` ms)
  - `feedback` (Slider, float, range `[0.0, 95.0]` %, default `40.0` %)
  - `tone_hz` (Logarithmic Slider, float, range `[500.0, 20000.0]` Hz, default `8000.0` Hz)
  - `ping_pong` (Toggle Button, bool, default `false`)
  - `mix` (Slider, float, range `[0.0, 100.0]` %, default `30.0` %)

### 3.8 Hard-Clipping Distortion (`at.builtin.audio.distortion`)
- **Plugin Path:** `Audio/Distortion/DistortionNode.dll`
- **Category Path:** `Audio/Distortion`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `drive_db` (Slider, float, range `[0.0, 40.0]` dB, default `12.0` dB)
  - `hardness` (Slider, float, range `[0.0, 1.0]`, default `0.8`)
  - `tone_hz` (Logarithmic Slider, float, range `[500.0, 16000.0]` Hz, default `5000.0` Hz)
  - `output_db` (Slider, float, range `[-24.0, 6.0]` dB, default `-6.0` dB)
  - `mix` (Slider, float, range `[0.0, 100.0]` %, default `100.0` %)

### 3.9 Tube-Style Overdrive (`at.builtin.audio.overdrive`)
- **Plugin Path:** `Audio/Distortion/OverdriveNode.dll`
- **Category Path:** `Audio/Distortion`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `drive_db` (Slider, float, range `[0.0, 36.0]` dB, default `12.0` dB)
  - `warmth_db` (Slider, float, range `[0.0, 12.0]` dB, default `3.0` dB)
  - `tone_hz` (Logarithmic Slider, float, range `[1000.0, 12000.0]` Hz, default `6000.0` Hz)
  - `output_db` (Slider, float, range `[-24.0, 6.0]` dB, default `-3.0` dB)
  - `mix` (Slider, float, range `[0.0, 100.0]` %, default `100.0` %)

### 3.10 Tape Saturation (`at.builtin.audio.saturation`)
- **Plugin Path:** `Audio/Distortion/SaturationNode.dll`
- **Category Path:** `Audio/Distortion`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `drive_db` (Slider, float, range `[0.0, 24.0]` dB, default `6.0` dB)
  - `bias` (Slider, float, range `[-0.8, 0.8]`, default `0.2`)
  - `cutoff_hz` (Logarithmic Slider, float, range `[4000.0, 20000.0]` Hz, default `16000.0` Hz)
  - `output_db` (Slider, float, range `[-18.0, 6.0]` dB, default `0.0` dB)
  - `mix` (Slider, float, range `[0.0, 100.0]` %, default `100.0` %)

### 3.11 Bitcrusher & Decimator (`at.builtin.audio.bitcrusher`)
- **Plugin Path:** `Audio/Distortion/BitcrusherNode.dll`
- **Category Path:** `Audio/Distortion`
- **Controls:**
  - `bypass` (Toggle Button, bool, default `false`)
  - `bit_depth` (Slider, float, range `[1.0, 24.0]` bits, default `8.0` bits)
  - `downsample` (Slider, float, range `[1.0, 50.0]` x, default `4.0` x)
  - `output_db` (Slider, float, range `[-18.0, 6.0]` dB, default `0.0` dB)
  - `mix` (Slider, float, range `[0.0, 100.0]` %, default `100.0` %)

---

## 4. UI Rendering Enhancements

In [CanvasRenderer.cpp](file:///d:/Code/AudioTool/src/gui/CanvasRenderer.cpp):
- Controls with `predefined_values` populated (e.g. `filter_type` and `slope` in `FilterNode`) are dynamically rendered as native ImGui combo dropdowns (`ImGui::BeginCombo`).
- When a user clicks a dropdown item, the engine receives the index `i` cleanly as an integer control value.
- Node right-click spawning automatically displays the taxonomic hierarchy:
  - `Audio`
    - `Distortion` -> Bitcrusher, Distortion, Overdrive, Saturation
    - `Dynamics` -> Compressor, Limiter, Noise Gate
    - `Effects` -> Delay, Reverb
    - `EQ` -> 16-Band Equalizer
    - `Filters` -> Multi-Mode Filter
    - `Processing` -> Volume & Multi-Channel Mixer
    - `Outputs` -> Audio Device Output
    - `Sources` -> Virtual Audio Input
  - `Control`
    - `Triggers` -> Periodic Pulse Timer
