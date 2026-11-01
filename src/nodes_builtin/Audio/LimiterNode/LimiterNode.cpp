// ============================================================================
// AudioTool Plugin DLL: LimiterNode (Audio/Dynamics/LimiterNode.dll)
// Features: Hard-Ceiling Audio Peak Limiter
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
    CTRL_BYPASS         = 100,
    CTRL_CEILING_DB     = 101,
    CTRL_INPUT_GAIN_DB  = 102,
    CTRL_RELEASE_MS     = 103,
    CTRL_PEAK_METER     = 104
};

struct LimiterState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool   bypass = false;
    double ceiling_db = -0.1;
    double input_gain_db = 0.0;
    double release_ms = 50.0;
    double peak_meter_db = -96.0;

    at::dsp::Limiter limiter;
    std::vector<AT_ControlDescriptor> cached_controls;

    void update_parameters() {
        limiter.set_parameters(
            static_cast<float>(ceiling_db),
            static_cast<float>(input_gain_db),
            static_cast<float>(release_ms));
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
            CTRL_CEILING_DB, "ceiling_db", "Ceiling", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -12.0, 0.0, 0.1, false,
            { AT_VAL_FLOAT, { .float_val = -0.1 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_INPUT_GAIN_DB, "input_gain_db", "Input Gain", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -12.0, 24.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_RELEASE_MS, "release_ms", "Release", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            1.0, 500.0, 1.0, false,
            { AT_VAL_FLOAT, { .float_val = 50.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_PEAK_METER, "peak_meter", "Output Peak", "dB",
            AT_CTRL_LEVEL_METER, AT_STYLE_DEFAULT, AT_VAL_FLOAT,
            -60.0, 6.0, 0.1, false,
            { AT_VAL_FLOAT, { .float_val = -96.0 } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeHandle AT_CDECL lim_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new LimiterState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->limiter.prepare(48000.0f);
    s->update_parameters();
    return reinterpret_cast<AT_NodeHandle>(s);
}

void AT_CDECL lim_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<LimiterState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL lim_get_controls(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) return nullptr;
    auto* s = reinterpret_cast<LimiterState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(s->cached_controls.size());
    return s->cached_controls.data();
}

const AT_FunctionDescriptor* AT_CDECL lim_get_functions(AT_NodeHandle, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    return nullptr;
}

AT_Result AT_CDECL lim_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_value) {
    if (!handle || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<LimiterState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_CEILING_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->ceiling_db; return AT_OK;
    case CTRL_INPUT_GAIN_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->input_gain_db; return AT_OK;
    case CTRL_RELEASE_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->release_ms; return AT_OK;
    case CTRL_PEAK_METER:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->peak_meter_db; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL lim_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!handle || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<LimiterState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_CEILING_DB:
        s->ceiling_db = std::clamp(in_value->float_val, -12.0, 0.0);
        s->update_parameters(); return AT_OK;
    case CTRL_INPUT_GAIN_DB:
        s->input_gain_db = std::clamp(in_value->float_val, -12.0, 24.0);
        s->update_parameters(); return AT_OK;
    case CTRL_RELEASE_MS:
        s->release_ms = std::clamp(in_value->float_val, 1.0, 500.0);
        s->update_parameters(); return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL lim_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t, uint32_t) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<LimiterState*>(handle);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->limiter.prepare(static_cast<float>(s->sample_rate));
    s->update_parameters();
    return AT_OK;
}

void AT_CDECL lim_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* s = reinterpret_cast<LimiterState*>(handle);
    const uint32_t frames = ctx->num_frames;
    const uint32_t channels = ctx->output_bus.num_channels;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ctx->output_bus.channel_ptrs[ch], 0, frames * sizeof(float));
    }

    if (ctx->input_bus_count == 0) {
        s->peak_meter_db = -96.0;
        return;
    }

    for (uint32_t b = 0; b < ctx->input_bus_count; ++b) {
        const auto& in_bus = ctx->input_buses[b];
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const uint32_t src_ch = std::min(ch, in_bus.num_channels - 1);
                ctx->output_bus.channel_ptrs[ch][f] += in_bus.channel_ptrs[src_ch][f];
            }
        }
    }

    if (s->bypass) {
        s->peak_meter_db = -96.0;
        return;
    }

    float* left = ctx->output_bus.channel_ptrs[0];
    float* right = (channels > 1) ? ctx->output_bus.channel_ptrs[1] : left;

    float max_peak = 0.0f;
    for (uint32_t n = 0; n < frames; ++n) {
        s->limiter.process_stereo(left[n], (channels > 1) ? right[n] : left[n], left[n], right[n]);
        max_peak = std::max(max_peak, std::abs(left[n]));
        if (channels > 1) max_peak = std::max(max_peak, std::abs(right[n]));
    }
    s->peak_meter_db = static_cast<double>(at::linear_to_db(max_peak));
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.limiter",
    "Hard-Ceiling Limiter",
    "Audio/Dynamics",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_PROCESSING,
    AT_NODE_FLAG_NONE
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &lim_create_instance,
    &lim_destroy_instance,
    &lim_get_controls,
    &lim_get_functions,
    &lim_get_control_value,
    &lim_set_control_value,
    nullptr, nullptr, nullptr,
    &lim_prepare_audio,
    &lim_process_audio,
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
