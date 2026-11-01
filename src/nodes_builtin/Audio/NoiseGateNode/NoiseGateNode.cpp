// ============================================================================
// AudioTool Plugin DLL: NoiseGateNode (Audio/Dynamics/NoiseGateNode.dll)
// Features: Noise Gate with Hysteresis & Attack/Hold/Release
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
    CTRL_BYPASS       = 100,
    CTRL_THRESHOLD_DB = 101,
    CTRL_ATTACK_MS    = 102,
    CTRL_HOLD_MS      = 103,
    CTRL_RELEASE_MS   = 104,
    CTRL_FLOOR_DB     = 105,
    CTRL_GATE_OPEN    = 106
};

struct NoiseGateState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool   bypass = false;
    double threshold_db = -45.0;
    double attack_ms = 2.0;
    double hold_ms = 50.0;
    double release_ms = 150.0;
    double floor_db = -80.0;
    bool   gate_open = false;

    at::dsp::NoiseGate gate;
    std::vector<AT_ControlDescriptor> cached_controls;

    void update_parameters() {
        gate.set_parameters(
            static_cast<float>(threshold_db),
            static_cast<float>(attack_ms),
            static_cast<float>(hold_ms),
            static_cast<float>(release_ms),
            static_cast<float>(floor_db));
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
            CTRL_THRESHOLD_DB, "threshold_db", "Threshold", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -80.0, 0.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = -45.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_ATTACK_MS, "attack_ms", "Attack", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.1, 50.0, 0.2, false,
            { AT_VAL_FLOAT, { .float_val = 2.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_HOLD_MS, "hold_ms", "Hold", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 500.0, 5.0, false,
            { AT_VAL_FLOAT, { .float_val = 50.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_RELEASE_MS, "release_ms", "Release", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            5.0, 1000.0, 5.0, false,
            { AT_VAL_FLOAT, { .float_val = 150.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_FLOOR_DB, "floor_db", "Floor / Mute", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -96.0, 0.0, 1.0, false,
            { AT_VAL_FLOAT, { .float_val = -80.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_GATE_OPEN, "gate_open", "Gate Open", "",
            AT_CTRL_BUTTON, AT_STYLE_TOGGLE_BUTTON, AT_VAL_BOOL,
            0.0, 1.0, 1.0, false,
            { AT_VAL_BOOL, { .bool_val = false } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeHandle AT_CDECL ng_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new NoiseGateState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->gate.prepare(48000.0f);
    s->update_parameters();
    return reinterpret_cast<AT_NodeHandle>(s);
}

void AT_CDECL ng_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<NoiseGateState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL ng_get_controls(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) return nullptr;
    auto* s = reinterpret_cast<NoiseGateState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(s->cached_controls.size());
    return s->cached_controls.data();
}

const AT_FunctionDescriptor* AT_CDECL ng_get_functions(AT_NodeHandle, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    return nullptr;
}

AT_Result AT_CDECL ng_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_value) {
    if (!handle || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<NoiseGateState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_THRESHOLD_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->threshold_db; return AT_OK;
    case CTRL_ATTACK_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->attack_ms; return AT_OK;
    case CTRL_HOLD_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->hold_ms; return AT_OK;
    case CTRL_RELEASE_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->release_ms; return AT_OK;
    case CTRL_FLOOR_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->floor_db; return AT_OK;
    case CTRL_GATE_OPEN:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->gate_open; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL ng_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!handle || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<NoiseGateState*>(handle);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_THRESHOLD_DB:
        s->threshold_db = std::clamp(in_value->float_val, -80.0, 0.0);
        s->update_parameters(); return AT_OK;
    case CTRL_ATTACK_MS:
        s->attack_ms = std::clamp(in_value->float_val, 0.1, 50.0);
        s->update_parameters(); return AT_OK;
    case CTRL_HOLD_MS:
        s->hold_ms = std::clamp(in_value->float_val, 0.0, 500.0);
        s->update_parameters(); return AT_OK;
    case CTRL_RELEASE_MS:
        s->release_ms = std::clamp(in_value->float_val, 5.0, 1000.0);
        s->update_parameters(); return AT_OK;
    case CTRL_FLOOR_DB:
        s->floor_db = std::clamp(in_value->float_val, -96.0, 0.0);
        s->update_parameters(); return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL ng_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t, uint32_t) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<NoiseGateState*>(handle);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->gate.prepare(static_cast<float>(s->sample_rate));
    s->update_parameters();
    return AT_OK;
}

void AT_CDECL ng_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* s = reinterpret_cast<NoiseGateState*>(handle);
    const uint32_t frames = ctx->num_frames;
    const uint32_t channels = ctx->output_bus.num_channels;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ctx->output_bus.channel_ptrs[ch], 0, frames * sizeof(float));
    }

    if (ctx->input_bus_count == 0) {
        s->gate_open = false;
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
        s->gate_open = true;
        return;
    }

    float* left = ctx->output_bus.channel_ptrs[0];
    float* right = (channels > 1) ? ctx->output_bus.channel_ptrs[1] : left;

    for (uint32_t n = 0; n < frames; ++n) {
        s->gate.process_stereo(left[n], (channels > 1) ? right[n] : left[n], left[n], right[n]);
    }
    s->gate_open = s->gate.is_open();
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.noise_gate",
    "Noise Gate",
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
    &ng_create_instance,
    &ng_destroy_instance,
    &ng_get_controls,
    &ng_get_functions,
    &ng_get_control_value,
    &ng_set_control_value,
    nullptr, nullptr, nullptr,
    &ng_prepare_audio,
    &ng_process_audio,
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
