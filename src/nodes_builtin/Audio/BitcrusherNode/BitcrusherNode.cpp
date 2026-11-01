// ============================================================================
// AudioTool Plugin DLL: BitcrusherNode (Audio/Distortion/BitcrusherNode.dll)
// Features: Bit Depth Reduction & Sample-and-Hold Downsampling
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
    CTRL_BYPASS        = 100,
    CTRL_BIT_DEPTH     = 101,
    CTRL_DOWNSAMPLE    = 102,
    CTRL_OUTPUT_DB     = 103,
    CTRL_DRY_WET_MIX   = 104
};

struct BitcrusherState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool   bypass = false;
    double bit_depth = 8.0;
    double downsample_factor = 4.0;
    double output_db = 0.0;
    double mix_pct = 100.0;

    at::dsp::BitcrusherProcessor crusher;
    std::vector<AT_ControlDescriptor> cached_controls;

    void update_parameters() {
        crusher.set_parameters(
            static_cast<float>(bit_depth),
            static_cast<float>(downsample_factor),
            0.0f,
            static_cast<float>(output_db),
            static_cast<float>(mix_pct * 0.01));
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
            CTRL_BIT_DEPTH, "bit_depth", "Bit Depth", "bits",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            1.0, 24.0, 1.0, false,
            { AT_VAL_FLOAT, { .float_val = 8.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_DOWNSAMPLE, "downsample", "Downsample", "x",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            1.0, 50.0, 1.0, false,
            { AT_VAL_FLOAT, { .float_val = 4.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_OUTPUT_DB, "output_db", "Output Level", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -18.0, 6.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_DRY_WET_MIX, "mix", "Mix", "%",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 100.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 100.0 } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeInstance* AT_CDECL bit_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new BitcrusherState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->crusher.prepare(48000.0f);
    s->update_parameters();
    return reinterpret_cast<AT_NodeInstance*>(s);
}

void AT_CDECL bit_destroy_instance(AT_NodeInstance* instance) {
    delete reinterpret_cast<BitcrusherState*>(instance);
}

uint32_t AT_CDECL bit_get_controls(AT_NodeInstance* instance, const AT_ControlDescriptor** out_descriptors) {
    auto* s = reinterpret_cast<BitcrusherState*>(instance);
    if (out_descriptors) *out_descriptors = s->cached_controls.data();
    return static_cast<uint32_t>(s->cached_controls.size());
}

uint32_t AT_CDECL bit_get_functions(AT_NodeInstance*, const AT_FunctionDescriptor** out_descriptors) {
    if (out_descriptors) *out_descriptors = nullptr;
    return 0;
}

AT_Result AT_CDECL bit_get_control_value(AT_NodeInstance* instance, uint32_t control_id, AT_ControlValue* out_value) {
    if (!instance || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<BitcrusherState*>(instance);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_BIT_DEPTH:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->bit_depth; return AT_OK;
    case CTRL_DOWNSAMPLE:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->downsample_factor; return AT_OK;
    case CTRL_OUTPUT_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->output_db; return AT_OK;
    case CTRL_DRY_WET_MIX:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->mix_pct; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL bit_set_control_value(AT_NodeInstance* instance, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!instance || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<BitcrusherState*>(instance);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_BIT_DEPTH:
        s->bit_depth = std::clamp(in_value->float_val, 1.0, 24.0);
        s->update_parameters(); return AT_OK;
    case CTRL_DOWNSAMPLE:
        s->downsample_factor = std::clamp(in_value->float_val, 1.0, 50.0);
        s->update_parameters(); return AT_OK;
    case CTRL_OUTPUT_DB:
        s->output_db = std::clamp(in_value->float_val, -18.0, 6.0);
        s->update_parameters(); return AT_OK;
    case CTRL_DRY_WET_MIX:
        s->mix_pct = std::clamp(in_value->float_val, 0.0, 100.0);
        s->update_parameters(); return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL bit_prepare_audio(AT_NodeInstance* instance, uint32_t sample_rate, uint32_t) {
    auto* s = reinterpret_cast<BitcrusherState*>(instance);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->crusher.prepare(static_cast<float>(s->sample_rate));
    s->update_parameters();
    return AT_OK;
}

void AT_CDECL bit_process_audio(AT_NodeInstance* instance, AT_AudioProcessContext* ctx) {
    auto* s = reinterpret_cast<BitcrusherState*>(instance);
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

    for (uint32_t n = 0; n < frames; ++n) {
        s->crusher.process_stereo(left[n], (channels > 1) ? right[n] : left[n], left[n], right[n]);
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.bitcrusher",
    "Bitcrusher & Decimator",
    "Audio/Distortion",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_PROCESSING,
    AT_NODE_FLAG_NONE
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &bit_create_instance,
    &bit_destroy_instance,
    &bit_get_controls,
    &bit_get_functions,
    &bit_get_control_value,
    &bit_set_control_value,
    nullptr, nullptr, nullptr,
    &bit_prepare_audio,
    &bit_process_audio,
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
