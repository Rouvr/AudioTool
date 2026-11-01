// ============================================================================
// AudioTool Plugin DLL: DelayNode (Audio/Effects/DelayNode.dll)
// Features: Stereo Delay with Feedback, Lowpass Damping, and Ping-Pong
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
    CTRL_DELAY_MS    = 101,
    CTRL_FEEDBACK    = 102,
    CTRL_TONE_HZ     = 103,
    CTRL_PING_PONG   = 104,
    CTRL_DRY_WET_MIX = 105
};

struct DelayState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool   bypass = false;
    double delay_ms = 350.0;
    double feedback_pct = 40.0;
    double tone_hz = 8000.0;
    bool   ping_pong = false;
    double mix_pct = 30.0;

    at::dsp::StereoDelay delay;
    std::vector<AT_ControlDescriptor> cached_controls;

    void update_parameters() {
        delay.set_parameters(
            static_cast<float>(delay_ms),
            static_cast<float>(feedback_pct * 0.01),
            static_cast<float>(tone_hz),
            static_cast<float>(mix_pct * 0.01),
            ping_pong);
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
            CTRL_DELAY_MS, "delay_ms", "Time", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            1.0, 2000.0, 1.0, false,
            { AT_VAL_FLOAT, { .float_val = 350.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_FEEDBACK, "feedback", "Feedback", "%",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 95.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 40.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_TONE_HZ, "tone_hz", "Tone Damp", "Hz",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            500.0, 20000.0, 50.0, true,
            { AT_VAL_FLOAT, { .float_val = 8000.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_PING_PONG, "ping_pong", "Ping-Pong", "",
            AT_CTRL_BUTTON, AT_STYLE_TOGGLE_BUTTON, AT_VAL_BOOL,
            0.0, 1.0, 1.0, false,
            { AT_VAL_BOOL, { .bool_val = false } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_DRY_WET_MIX, "mix", "Dry / Wet", "%",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 100.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 30.0 } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeHandle AT_CDECL del_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new DelayState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->delay.prepare(48000.0f, 2000.0f);
    s->update_parameters();
    return reinterpret_cast<AT_NodeHandle>(s);
}

void AT_CDECL del_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<DelayState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL del_get_controls(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) return nullptr;
    auto* s = reinterpret_cast<DelayState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(s->cached_controls.size());
    return s->cached_controls.data();
}

const AT_FunctionDescriptor* AT_CDECL del_get_functions(AT_NodeHandle, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    return nullptr;
}

AT_Result AT_CDECL del_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_value) {
    if (!handle || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<DelayState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_DELAY_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->delay_ms; return AT_OK;
    case CTRL_FEEDBACK:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->feedback_pct; return AT_OK;
    case CTRL_TONE_HZ:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->tone_hz; return AT_OK;
    case CTRL_PING_PONG:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->ping_pong; return AT_OK;
    case CTRL_DRY_WET_MIX:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->mix_pct; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL del_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!handle || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<DelayState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_DELAY_MS:
        s->delay_ms = std::clamp(in_value->float_val, 1.0, 2000.0);
        s->update_parameters(); return AT_OK;
    case CTRL_FEEDBACK:
        s->feedback_pct = std::clamp(in_value->float_val, 0.0, 95.0);
        s->update_parameters(); return AT_OK;
    case CTRL_TONE_HZ:
        s->tone_hz = std::clamp(in_value->float_val, 500.0, 20000.0);
        s->update_parameters(); return AT_OK;
    case CTRL_PING_PONG:
        s->ping_pong = in_value->bool_val;
        s->update_parameters(); return AT_OK;
    case CTRL_DRY_WET_MIX:
        s->mix_pct = std::clamp(in_value->float_val, 0.0, 100.0);
        s->update_parameters(); return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL del_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t, uint32_t) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<DelayState*>(handle);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->delay.prepare(static_cast<float>(s->sample_rate), 2000.0f);
    s->update_parameters();
    return AT_OK;
}

void AT_CDECL del_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* s = reinterpret_cast<DelayState*>(handle);
    const uint32_t frames = ctx->num_frames;
    const uint32_t channels = ctx->output_bus.num_channels;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ctx->output_bus.channel_ptrs[ch], 0, frames * sizeof(float));
    }

    if (ctx->input_bus_count > 0) {
        for (uint32_t b = 0; b < ctx->input_bus_count; ++b) {
            const auto& in_bus = ctx->input_buses[b];
            for (uint32_t f = 0; f < frames; ++f) {
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    const uint32_t src_ch = std::min(ch, in_bus.num_channels - 1);
                    ctx->output_bus.channel_ptrs[ch][f] += in_bus.channel_ptrs[src_ch][f];
                }
            }
        }
    }

    if (s->bypass) return;

    float* left = ctx->output_bus.channel_ptrs[0];
    float* right = (channels > 1) ? ctx->output_bus.channel_ptrs[1] : left;

    for (uint32_t n = 0; n < frames; ++n) {
        s->delay.process_stereo(left[n], (channels > 1) ? right[n] : left[n], left[n], right[n]);
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.delay",
    "Stereo Delay & Echo",
    "Audio/Effects",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_PROCESSING,
    AT_NODE_FLAG_PRODUCES_TAIL
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &del_create_instance,
    &del_destroy_instance,
    &del_get_controls,
    &del_get_functions,
    &del_get_control_value,
    &del_set_control_value,
    nullptr, nullptr, nullptr,
    &del_prepare_audio,
    &del_process_audio,
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
