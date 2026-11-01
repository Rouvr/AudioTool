// ============================================================================
// AudioTool Plugin DLL: EqualizerNode (Audio/EQ/EqualizerNode.dll)
// Features: 16-Band ISO Standard Graphic/Parametric Equalizer
// ============================================================================

#define AT_BUILDING_PLUGIN_DLL 1
#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "at_sdk/at_dsp.h"
#include "at_version.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kNumBands = 16;
constexpr float kBandFrequencies[kNumBands] = {
    25.0f, 40.0f, 63.0f, 100.0f, 160.0f, 250.0f, 400.0f, 630.0f,
    1000.0f, 1600.0f, 2500.0f, 4000.0f, 6300.0f, 10000.0f, 16000.0f, 20000.0f
};

const char* const kBandLabels[kNumBands] = {
    "25 Hz", "40 Hz", "63 Hz", "100 Hz", "160 Hz", "250 Hz", "400 Hz", "630 Hz",
    "1.0 kHz", "1.6 kHz", "2.5 kHz", "4.0 kHz", "6.3 kHz", "10.0 kHz", "16.0 kHz", "20.0 kHz"
};

const char* const kBandIdentifiers[kNumBands] = {
    "band_0", "band_1", "band_2", "band_3", "band_4", "band_5", "band_6", "band_7",
    "band_8", "band_9", "band_10", "band_11", "band_12", "band_13", "band_14", "band_15"
};

enum ControlIds : uint32_t {
    CTRL_BYPASS      = 100,
    CTRL_MASTER_GAIN = 101,
    CTRL_BAND_BASE   = 200 // 200 .. 215
};

struct EqualizerState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool bypass = false;
    double master_gain_db = 0.0;
    std::array<double, kNumBands> band_gains_db{};

    at::dsp::Biquad filters[kNumBands];
    at::SmoothedFloat smoothed_master_gain{1.0f};

    std::vector<AT_ControlDescriptor>  cached_controls;

    void update_filters() {
        float fs = static_cast<float>(sample_rate);
        for (size_t i = 0; i < kNumBands; ++i) {
            at::dsp::BiquadType type = at::dsp::BiquadType::PeakingEQ;
            if (i == 0) type = at::dsp::BiquadType::LowShelf;
            else if (i == kNumBands - 1) type = at::dsp::BiquadType::HighShelf;

            filters[i].configure(type, fs, kBandFrequencies[i], 1.414f, static_cast<float>(band_gains_db[i]));
        }
    }

    void init_descriptors() {
        cached_controls.clear();

        cached_controls.push_back({
            CTRL_BYPASS, "bypass", "Bypass", "",
            AT_CTRL_BUTTON, AT_STYLE_TOGGLE_BUTTON, AT_VAL_BOOL,
            0.0, 1.0, 1.0, false,
            { AT_VAL_BOOL, { .bool_val = false } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_MASTER_GAIN, "master_gain", "Master Gain", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -18.0, 18.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });

        for (size_t i = 0; i < kNumBands; ++i) {
            cached_controls.push_back({
                static_cast<uint32_t>(CTRL_BAND_BASE + i),
                kBandIdentifiers[i],
                kBandLabels[i],
                "dB",
                AT_CTRL_SLIDER,
                AT_STYLE_HORIZONTAL_SLIDER,
                AT_VAL_FLOAT,
                -18.0, 18.0, 0.5, false,
                { AT_VAL_FLOAT, { .float_val = 0.0 } },
                nullptr, 0, nullptr, 0
            });
        }
    }
};

AT_NodeHandle AT_CDECL eq_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new EqualizerState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->band_gains_db.fill(0.0);
    s->init_descriptors();
    s->update_filters();
    return reinterpret_cast<AT_NodeHandle>(s);
}

void AT_CDECL eq_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<EqualizerState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL eq_get_controls(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) return nullptr;
    auto* s = reinterpret_cast<EqualizerState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(s->cached_controls.size());
    return s->cached_controls.data();
}

const AT_FunctionDescriptor* AT_CDECL eq_get_functions(AT_NodeHandle, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    return nullptr;
}

AT_Result AT_CDECL eq_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_value) {
    if (!handle || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<EqualizerState*>(handle);
    if (control_id == CTRL_BYPASS) {
        out_value->type = AT_VAL_BOOL;
        out_value->bool_val = s->bypass;
        return AT_OK;
    }
    if (control_id == CTRL_MASTER_GAIN) {
        out_value->type = AT_VAL_FLOAT;
        out_value->float_val = s->master_gain_db;
        return AT_OK;
    }
    if (control_id >= CTRL_BAND_BASE && control_id < CTRL_BAND_BASE + kNumBands) {
        out_value->type = AT_VAL_FLOAT;
        out_value->float_val = s->band_gains_db[control_id - CTRL_BAND_BASE];
        return AT_OK;
    }
    return AT_ERR_NOT_FOUND;
}

AT_Result AT_CDECL eq_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!handle || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<EqualizerState*>(handle);
    if (control_id == CTRL_BYPASS) {
        s->bypass = in_value->bool_val;
        return AT_OK;
    }
    if (control_id == CTRL_MASTER_GAIN) {
        s->master_gain_db = std::clamp(in_value->float_val, -18.0, 18.0);
        s->smoothed_master_gain.set_target(at::db_to_linear(static_cast<float>(s->master_gain_db)));
        return AT_OK;
    }
    if (control_id >= CTRL_BAND_BASE && control_id < CTRL_BAND_BASE + kNumBands) {
        size_t idx = control_id - CTRL_BAND_BASE;
        s->band_gains_db[idx] = std::clamp(in_value->float_val, -18.0, 18.0);
        at::dsp::BiquadType type = at::dsp::BiquadType::PeakingEQ;
        if (idx == 0) type = at::dsp::BiquadType::LowShelf;
        else if (idx == kNumBands - 1) type = at::dsp::BiquadType::HighShelf;
        s->filters[idx].configure(type, static_cast<float>(s->sample_rate), kBandFrequencies[idx], 1.414f, static_cast<float>(s->band_gains_db[idx]));
        return AT_OK;
    }
    return AT_ERR_NOT_FOUND;
}

AT_Result AT_CDECL eq_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t, uint32_t) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<EqualizerState*>(handle);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->smoothed_master_gain.prepare(s->sample_rate, 5.0f);
    s->smoothed_master_gain.set_immediate(at::db_to_linear(static_cast<float>(s->master_gain_db)));
    for (auto& f : s->filters) f.reset();
    s->update_filters();
    return AT_OK;
}

void AT_CDECL eq_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* s = reinterpret_cast<EqualizerState*>(handle);
    const uint32_t frames = ctx->num_frames;
    const uint32_t channels = ctx->output_bus.num_channels;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ctx->output_bus.channel_ptrs[ch], 0, frames * sizeof(float));
    }

    if (ctx->input_bus_count == 0) return;

    for (uint32_t b = 0; b < ctx->input_bus_count; ++b) {
        const auto& in_bus = ctx->input_buses[b];
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const uint32_t src_ch = std::min(ch, in_bus.num_channels - 1);
                ctx->output_bus.channel_ptrs[ch][f] += in_bus.channel_ptrs[src_ch][f];
            }
        }
    }

    if (s->bypass) return;

    float* left = ctx->output_bus.channel_ptrs[0];
    float* right = (channels > 1) ? ctx->output_bus.channel_ptrs[1] : left;

    for (size_t i = 0; i < kNumBands; ++i) {
        auto& f = s->filters[i];
        for (uint32_t n = 0; n < frames; ++n) {
            left[n] = f.process_left(left[n]);
            if (channels > 1) {
                right[n] = f.process_right(right[n]);
            }
        }
    }

    for (uint32_t n = 0; n < frames; ++n) {
        float mg = s->smoothed_master_gain.next_value();
        left[n] *= mg;
        if (channels > 1) {
            right[n] *= mg;
        }
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.equalizer",
    "16-Band Equalizer",
    "Audio/EQ",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_PROCESSING,
    AT_NODE_FLAG_NONE
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &eq_create_instance,
    &eq_destroy_instance,
    &eq_get_controls,
    &eq_get_functions,
    &eq_get_control_value,
    &eq_set_control_value,
    nullptr, nullptr, nullptr,
    &eq_prepare_audio,
    &eq_process_audio,
    nullptr, nullptr, nullptr
};

static const AT_PluginExportBundle kBundle = {
    sizeof(AT_PluginExportBundle),
    AT_ABI_VERSION,
    &kDescriptor,
    &kVTable
};

} // namespace

extern "C" AT_PLUGIN_EXPORT const AT_PluginExportBundle* AT_CDECL at_get_plugin_bundle(uint32_t host_abi_version) {
    if (host_abi_version != AT_ABI_VERSION) return nullptr;
    return &kBundle;
}
