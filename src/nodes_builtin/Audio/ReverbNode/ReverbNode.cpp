// ============================================================================
// AudioTool Plugin DLL: ReverbNode (Audio/Effects/ReverbNode.dll)
// Features: Digital Reverberator based on Freeverb (Jezar at Dreampoint)
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
    CTRL_ROOM_SIZE    = 101,
    CTRL_DAMPING      = 102,
    CTRL_WIDTH        = 103,
    CTRL_WET_DB       = 104,
    CTRL_DRY_DB       = 105,
    CTRL_FREEZE       = 106
};

struct ReverbState {
    const AT_HostServices* host = nullptr;
    uint32_t runtime_node_id = 0;
    uint32_t sample_rate = 48000;

    bool   bypass = false;
    double room_size = 0.6;
    double damping = 0.4;
    double width = 1.0;
    double wet_db = -12.0;
    double dry_db = 0.0;
    bool   freeze = false;

    at::dsp::FreeverbEngine reverb;
    std::vector<AT_ControlDescriptor> cached_controls;

    void update_parameters() {
        reverb.set_room_size(static_cast<float>(room_size));
        reverb.set_damping(static_cast<float>(damping));
        reverb.set_width(static_cast<float>(width));
        reverb.set_wet(at::db_to_linear(static_cast<float>(wet_db)));
        reverb.set_dry(at::db_to_linear(static_cast<float>(dry_db)));
        reverb.set_freeze(freeze);
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
            CTRL_ROOM_SIZE, "room_size", "Room Size", "",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 1.0, 0.01, false,
            { AT_VAL_FLOAT, { .float_val = 0.6 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_DAMPING, "damping", "HF Damping", "",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 1.0, 0.01, false,
            { AT_VAL_FLOAT, { .float_val = 0.4 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_WIDTH, "width", "Stereo Width", "",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            0.0, 1.0, 0.01, false,
            { AT_VAL_FLOAT, { .float_val = 1.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_WET_DB, "wet_db", "Wet Mix", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -60.0, 0.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = -12.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_DRY_DB, "dry_db", "Dry Mix", "dB",
            AT_CTRL_SLIDER, AT_STYLE_HORIZONTAL_SLIDER, AT_VAL_FLOAT,
            -60.0, 0.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_FREEZE, "freeze", "Freeze", "",
            AT_CTRL_BUTTON, AT_STYLE_TOGGLE_BUTTON, AT_VAL_BOOL,
            0.0, 1.0, 1.0, false,
            { AT_VAL_BOOL, { .bool_val = false } },
            nullptr, 0, nullptr, 0
        });
    }
};

AT_NodeInstance* AT_CDECL rev_create_instance(const AT_HostServices* host, uint32_t runtime_node_id) {
    auto* s = new ReverbState();
    s->host = host;
    s->runtime_node_id = runtime_node_id;
    s->init_descriptors();
    s->reverb.prepare(48000.0f);
    s->update_parameters();
    return reinterpret_cast<AT_NodeInstance*>(s);
}

void AT_CDECL rev_destroy_instance(AT_NodeInstance* instance) {
    delete reinterpret_cast<ReverbState*>(instance);
}

uint32_t AT_CDECL rev_get_controls(AT_NodeInstance* instance, const AT_ControlDescriptor** out_descriptors) {
    auto* s = reinterpret_cast<ReverbState*>(instance);
    if (out_descriptors) *out_descriptors = s->cached_controls.data();
    return static_cast<uint32_t>(s->cached_controls.size());
}

uint32_t AT_CDECL rev_get_functions(AT_NodeInstance*, const AT_FunctionDescriptor** out_descriptors) {
    if (out_descriptors) *out_descriptors = nullptr;
    return 0;
}

AT_Result AT_CDECL rev_get_control_value(AT_NodeInstance* instance, uint32_t control_id, AT_ControlValue* out_value) {
    if (!instance || !out_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<ReverbState*>(instance);
    switch (control_id) {
    case CTRL_BYPASS:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->bypass; return AT_OK;
    case CTRL_ROOM_SIZE:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->room_size; return AT_OK;
    case CTRL_DAMPING:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->damping; return AT_OK;
    case CTRL_WIDTH:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->width; return AT_OK;
    case CTRL_WET_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->wet_db; return AT_OK;
    case CTRL_DRY_DB:
        out_value->type = AT_VAL_FLOAT; out_value->float_val = s->dry_db; return AT_OK;
    case CTRL_FREEZE:
        out_value->type = AT_VAL_BOOL; out_value->bool_val = s->freeze; return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL rev_set_control_value(AT_NodeInstance* instance, uint32_t control_id, const AT_ControlValue* in_value) {
    if (!instance || !in_value) return AT_ERR_INVALID_ARG;
    auto* s = reinterpret_cast<ReverbState*>(instance);
    switch (control_id) {
    case CTRL_BYPASS:
        s->bypass = in_value->bool_val; return AT_OK;
    case CTRL_ROOM_SIZE:
        s->room_size = std::clamp(in_value->float_val, 0.0, 1.0);
        s->update_parameters(); return AT_OK;
    case CTRL_DAMPING:
        s->damping = std::clamp(in_value->float_val, 0.0, 1.0);
        s->update_parameters(); return AT_OK;
    case CTRL_WIDTH:
        s->width = std::clamp(in_value->float_val, 0.0, 1.0);
        s->update_parameters(); return AT_OK;
    case CTRL_WET_DB:
        s->wet_db = std::clamp(in_value->float_val, -60.0, 0.0);
        s->update_parameters(); return AT_OK;
    case CTRL_DRY_DB:
        s->dry_db = std::clamp(in_value->float_val, -60.0, 0.0);
        s->update_parameters(); return AT_OK;
    case CTRL_FREEZE:
        s->freeze = in_value->bool_val;
        s->update_parameters(); return AT_OK;
    default:
        return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL rev_prepare_audio(AT_NodeInstance* instance, uint32_t sample_rate, uint32_t) {
    auto* s = reinterpret_cast<ReverbState*>(instance);
    s->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    s->reverb.prepare(static_cast<float>(s->sample_rate));
    s->update_parameters();
    return AT_OK;
}

void AT_CDECL rev_process_audio(AT_NodeInstance* instance, AT_AudioProcessContext* ctx) {
    auto* s = reinterpret_cast<ReverbState*>(instance);
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
        s->reverb.process_stereo(left[n], (channels > 1) ? right[n] : left[n], left[n], right[n]);
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.reverb",
    "Digital Reverb (Freeverb)",
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
    &rev_create_instance,
    &rev_destroy_instance,
    &rev_get_controls,
    &rev_get_functions,
    &rev_get_control_value,
    &rev_set_control_value,
    nullptr, nullptr, nullptr,
    &rev_prepare_audio,
    &rev_process_audio,
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
