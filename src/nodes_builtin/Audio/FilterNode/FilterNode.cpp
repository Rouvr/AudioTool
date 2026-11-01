// ============================================================================
// AudioTool Plugin DLL: FilterNode (Audio/Filters/FilterNode.dll)
// Features: Multi-Mode Filter (Low-pass, High-pass, Band-pass, 12/24 dB/oct)
// ============================================================================

#define AT_BUILDING_PLUGIN_DLL 1
#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "at_sdk/at_dsp.h"
#include "at_version.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

enum ControlIds : uint32_t {
    CTRL_BYPASS      = 100,
    CTRL_FILTER_TYPE = 101,
    CTRL_CUTOFF_HZ   = 102,
    CTRL_RESONANCE_Q = 103,
    CTRL_SLOPE       = 104,
    CTRL_OUTPUT_GAIN = 105
};

static const char* const kFilterTypeNames[] = {
    "Low-pass",
    "High-pass",
    "Band-pass"
};

static const char* const kSlopeNames[] = {
    "12 dB/oct (2-pole)",
    "24 dB/oct (4-pole)"
};

struct FilterState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool     bypass = false;
    int64_t  filter_type = 0; // 0=LP, 1=HP, 2=BP
    double   cutoff_hz = 1000.0;
    double   resonance_q = 0.707;
    int64_t  slope = 0; // 0=12dB, 1=24dB
    double   output_gain_db = 0.0;

    at::dsp::Biquad stage1;
    at::dsp::Biquad stage2; // for 24 dB/oct cascade
    at::SmoothedFloat smoothed_gain{1.0f};

    std::vector<AT_ControlDescriptor>  cached_controls;

    void update_coefficients() {
        at::dsp::BiquadType bqt = at::dsp::BiquadType::LowPass;
        if (filter_type == 1) bqt = at::dsp::BiquadType::HighPass;
        else if (filter_type == 2) bqt = at::dsp::BiquadType::BandPass;

        float fs = static_cast<float>(sample_rate);
        float fc = static_cast<float>(cutoff_hz);
        float q  = static_cast<float>(resonance_q);

        stage1.configure(bqt, fs, fc, q, 0.0f);
        stage2.configure(bqt, fs, fc, q, 0.0f);
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
            CTRL_FILTER_TYPE, "filter_type", "Type", "",
            AT_CTRL_VALUE_TEXT_INPUT, AT_STYLE_DROPDOWN_COMBO, AT_VAL_INT,
            0.0, 2.0, 1.0, false,
            { AT_VAL_INT, { .int_val = 0 } },
            nullptr, 0, kFilterTypeNames, 3
        });

        cached_controls.push_back({
            CTRL_CUTOFF_HZ, "cutoff_hz", "Cutoff", "Hz",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            20.0, 20000.0, 1.0, true,
            { AT_VAL_FLOAT, { .float_val = 1000.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_RESONANCE_Q, "resonance_q", "Resonance (Q)", "",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.1, 10.0, 0.05, false,
            { AT_VAL_FLOAT, { .float_val = 0.707 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_SLOPE, "slope", "Slope", "",
            AT_CTRL_VALUE_TEXT_INPUT, AT_STYLE_DROPDOWN_COMBO, AT_VAL_INT,
            0.0, 1.0, 1.0, false,
            { AT_VAL_INT, { .int_val = 0 } },
            nullptr, 0, kSlopeNames, 2
        });

        cached_controls.push_back({
            CTRL_OUTPUT_GAIN, "output_gain_db", "Output Gain", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -18.0, 18.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeHandle AT_CDECL flt_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new FilterState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->update_coefficients();
    return reinterpret_cast<AT_NodeHandle>(s);
}

void AT_CDECL flt_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<FilterState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL flt_get_controls(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) return nullptr;
    auto* s = reinterpret_cast<FilterState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(s->cached_controls.size());
    return s->cached_controls.data();
}

const AT_FunctionDescriptor* AT_CDECL flt_get_functions(AT_NodeHandle, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    return nullptr;
}

AT_Result AT_CDECL flt_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_value) {
    if (!handle || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<FilterState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_FILTER_TYPE:
        out_value->type = AT_VAL_INT; out_value->int_val = s->filter_type; return AT_OK;
    case CTRL_CUTOFF_HZ:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->cutoff_hz; return AT_OK;
    case CTRL_RESONANCE_Q:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->resonance_q; return AT_OK;
    case CTRL_SLOPE:
        out_value->type = AT_VAL_INT; out_value->int_val = s->slope; return AT_OK;
    case CTRL_OUTPUT_GAIN:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->output_gain_db; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL flt_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!handle || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<FilterState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_FILTER_TYPE:
        s->filter_type = std::clamp<int64_t>(in_value->int_val, 0, 2);
        s->update_coefficients(); return AT_OK;
    case CTRL_CUTOFF_HZ:
        s->cutoff_hz = std::clamp(in_value->float_val, 20.0, 20000.0);
        s->update_coefficients(); return AT_OK;
    case CTRL_RESONANCE_Q:
        s->resonance_q = std::clamp(in_value->float_val, 0.1, 10.0);
        s->update_coefficients(); return AT_OK;
    case CTRL_SLOPE:
        s->slope = std::clamp<int64_t>(in_value->int_val, 0, 1);
        s->update_coefficients(); return AT_OK;
    case CTRL_OUTPUT_GAIN:
        s->output_gain_db = std::clamp(in_value->float_val, -18.0, 18.0);
        s->smoothed_gain.set_target(at::db_to_linear(static_cast<float>(s->output_gain_db)));
        return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL flt_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t, uint32_t) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<FilterState*>(handle);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->smoothed_gain.prepare(s->sample_rate, 5.0f);
    s->smoothed_gain.set_immediate(at::db_to_linear(static_cast<float>(s->output_gain_db)));
    s->stage1.reset();
    s->stage2.reset();
    s->update_coefficients();
    return AT_OK;
}

void AT_CDECL flt_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* s = reinterpret_cast<FilterState*>(handle);
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

    bool is_24db = (s->slope == 1);
    for (uint32_t n = 0; n < frames; ++n) {
        float l = s->stage1.process_left(left[n]);
        float r = (channels > 1) ? s->stage1.process_right(right[n]) : l;

        if (is_24db) {
            l = s->stage2.process_left(l);
            if (channels > 1) r = s->stage2.process_right(r);
        }

        float g = s->smoothed_gain.next_value();
        left[n] = l * g;
        if (channels > 1) right[n] = r * g;
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.filter",
    "Multi-Mode Filter",
    "Audio/Filters",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_PROCESSING,
    AT_NODE_FLAG_NONE
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &flt_create_instance,
    &flt_destroy_instance,
    &flt_get_controls,
    &flt_get_functions,
    &flt_get_control_value,
    &flt_set_control_value,
    nullptr, nullptr, nullptr,
    &flt_prepare_audio,
    &flt_process_audio,
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
