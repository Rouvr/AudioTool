// ============================================================================
// AudioTool Plugin DLL: CompressorNode (Audio/Dynamics/CompressorNode.dll)
// Features: Dynamic Range Compressor (JAES Logarithmic Characteristic)
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
    CTRL_THRESHOLD_DB   = 101,
    CTRL_RATIO          = 102,
    CTRL_ATTACK_MS      = 103,
    CTRL_RELEASE_MS     = 104,
    CTRL_KNEE_DB        = 105,
    CTRL_MAKEUP_GAIN_DB = 106,
    CTRL_GR_METER       = 107
};

struct CompressorState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool   bypass = false;
    double threshold_db = -18.0;
    double ratio = 4.0;
    double attack_ms = 10.0;
    double release_ms = 100.0;
    double knee_db = 6.0;
    double makeup_gain_db = 0.0;
    double gr_meter_db = 0.0;

    at::dsp::Compressor comp;
    std::vector<AT_ControlDescriptor> cached_controls;

    void update_parameters() {
        comp.set_parameters(
            static_cast<float>(threshold_db),
            static_cast<float>(ratio),
            static_cast<float>(attack_ms),
            static_cast<float>(release_ms),
            static_cast<float>(knee_db),
            static_cast<float>(makeup_gain_db));
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
            -60.0, 0.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = -18.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_RATIO, "ratio", "Ratio", ":1",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            1.0, 20.0, 0.1, false,
            { AT_VAL_FLOAT, { .float_val = 4.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_ATTACK_MS, "attack_ms", "Attack", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.1, 200.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 10.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_RELEASE_MS, "release_ms", "Release", "ms",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            10.0, 1000.0, 5.0, false,
            { AT_VAL_FLOAT, { .float_val = 100.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_KNEE_DB, "knee_db", "Knee Width", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 20.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 6.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_MAKEUP_GAIN_DB, "makeup_gain_db", "Makeup Gain", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 24.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_GR_METER, "gr_meter", "Gain Reduction", "dB",
            AT_CTRL_LEVEL_METER, AT_STYLE_DEFAULT, AT_VAL_FLOAT,
            -30.0, 0.0, 0.1, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeInstance* AT_CDECL cmp_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new CompressorState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->comp.prepare(48000.0f);
    s->update_parameters();
    return reinterpret_cast<AT_NodeInstance*>(s);
}

void AT_CDECL cmp_destroy_instance(AT_NodeInstance* instance) {
    delete reinterpret_cast<CompressorState*>(instance);
}

uint32_t AT_CDECL cmp_get_controls(AT_NodeInstance* instance, const AT_ControlDescriptor** out_descriptors) {
    auto* s = reinterpret_cast<CompressorState*>(instance);
    if (out_descriptors) *out_descriptors = s->cached_controls.data();
    return static_cast<uint32_t>(s->cached_controls.size());
}

uint32_t AT_CDECL cmp_get_functions(AT_NodeInstance*, const AT_FunctionDescriptor** out_descriptors) {
    if (out_descriptors) *out_descriptors = nullptr;
    return 0;
}

AT_Result AT_CDECL cmp_get_control_value(AT_NodeInstance* instance, uint32_t control_id, AT_ControlValue* out_value) {
    if (!instance || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<CompressorState*>(instance);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_THRESHOLD_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->threshold_db; return AT_OK;
    case CTRL_RATIO:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->ratio; return AT_OK;
    case CTRL_ATTACK_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->attack_ms; return AT_OK;
    case CTRL_RELEASE_MS:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->release_ms; return AT_OK;
    case CTRL_KNEE_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->knee_db; return AT_OK;
    case CTRL_MAKEUP_GAIN_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->makeup_gain_db; return AT_OK;
    case CTRL_GR_METER:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->gr_meter_db; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL cmp_set_control_value(AT_NodeInstance* instance, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!instance || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<CompressorState*>(instance);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_THRESHOLD_DB:
        s->threshold_db = std::clamp(in_value->float_val, -60.0, 0.0);
        s->update_parameters(); return AT_OK;
    case CTRL_RATIO:
        s->ratio = std::clamp(in_value->float_val, 1.0, 20.0);
        s->update_parameters(); return AT_OK;
    case CTRL_ATTACK_MS:
        s->attack_ms = std::clamp(in_value->float_val, 0.1, 200.0);
        s->update_parameters(); return AT_OK;
    case CTRL_RELEASE_MS:
        s->release_ms = std::clamp(in_value->float_val, 10.0, 1000.0);
        s->update_parameters(); return AT_OK;
    case CTRL_KNEE_DB:
        s->knee_db = std::clamp(in_value->float_val, 0.0, 20.0);
        s->update_parameters(); return AT_OK;
    case CTRL_MAKEUP_GAIN_DB:
        s->makeup_gain_db = std::clamp(in_value->float_val, 0.0, 24.0);
        s->update_parameters(); return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL cmp_prepare_audio(AT_NodeInstance* instance, uint32_t sample_rate, uint32_t) {
    auto* s = reinterpret_cast<CompressorState*>(instance);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->comp.prepare(static_cast<float>(s->sample_rate));
    s->update_parameters();
    return AT_OK;
}

void AT_CDECL cmp_process_audio(AT_NodeInstance* instance, AT_AudioProcessContext* ctx) {
    auto* s = reinterpret_cast<CompressorState*>(instance);
    const uint32_t frames = ctx->num_frames;
    const uint32_t channels = ctx->output_bus.num_channels;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ctx->output_bus.channel_ptrs[ch], 0, frames * sizeof(float));
    }

    if (ctx->input_bus_count == 0) {
        s->gr_meter_db = 0.0;
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
        s->gr_meter_db = 0.0;
        return;
    }

    float* left = ctx->output_bus.channel_ptrs[0];
    float* right = (channels > 1) ? ctx->output_bus.channel_ptrs[1] : left;

    float min_gr = 0.0f;
    for (uint32_t n = 0; n < frames; ++n) {
        s->comp.process_stereo(left[n], (channels > 1) ? right[n] : left[n], left[n], right[n]);
        min_gr = std::min(min_gr, s->comp.current_gain_reduction_db());
    }
    s->gr_meter_db = static_cast<double>(min_gr);
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.compressor",
    "Dynamic Range Compressor",
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
    &cmp_create_instance,
    &cmp_destroy_instance,
    &cmp_get_controls,
    &cmp_get_functions,
    &cmp_get_control_value,
    &cmp_set_control_value,
    nullptr, nullptr, nullptr,
    &cmp_prepare_audio,
    &cmp_process_audio,
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
