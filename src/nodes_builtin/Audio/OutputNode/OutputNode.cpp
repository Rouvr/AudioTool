// ============================================================================
// AudioTool Plugin DLL: OutputNode (Audio/Outputs/OutputNode.dll)
// ============================================================================

#define AT_BUILDING_PLUGIN_DLL 1
#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "at_version.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

enum ControlIds : uint32_t {
    CTRL_DEVICE_ID      = 200,
    CTRL_OUTPUT_GAIN_DB = 201,
    CTRL_MUTE           = 202,
    CTRL_PEAK_METER_DB  = 203
};

enum FunctionIds : uint32_t {
    FUNC_TOGGLE_MUTE        = 2001,
    FUNC_SET_OUTPUT_GAIN_DB = 2002,
    FUNC_ADD_OUTPUT_GAIN_DB = 2003
};

struct OutputNodeState {
    const AT_HostServices* host        = nullptr;
    uint32_t               node_id     = 0;
    std::string            device_id   = "default_output";
    double                 gain_db     = 0.0;
    bool                   muted       = false;
    double                 peak_db     = -96.0;
    at::SmoothedFloat      gain_linear{1.0f};
    AT_DeviceStreamHandle  dev_stream  = nullptr;

    void rebind_device() {
        if (!host) return;
        if (dev_stream && host->release_hardware_device) {
            host->release_hardware_device(host->host_context, dev_stream);
            dev_stream = nullptr;
        }
        if (!device_id.empty() && host->acquire_hardware_device) {
            dev_stream = host->acquire_hardware_device(
                host->host_context,
                device_id.c_str(),
                false,
                false);
        }
    }

    void update_target_gain() {
        const float target = muted ? 0.0f : at::db_to_linear(static_cast<float>(gain_db));
        gain_linear.set_target(target);
    }
};

static const AT_ControlDescriptor kControls[] = {
    {
        CTRL_DEVICE_ID,
        "device_id",
        "Audio Output Device",
        "",
        AT_CTRL_AUDIO_DEVICE_SELECT,
        AT_STYLE_DROPDOWN_COMBO,
        AT_VAL_STRING,
        0.0, 0.0, 0.0, false,
        { AT_VAL_STRING, { .string_val = "default_output" } },
        nullptr,
        AT_DEV_FILTER_OUTPUT_RENDER,
        nullptr, 0
    },
    {
        CTRL_OUTPUT_GAIN_DB,
        "output_gain_db",
        "Output Volume",
        "dB",
        AT_CTRL_SLIDER,
        AT_STYLE_HORIZONTAL_SLIDER,
        AT_VAL_FLOAT,
        -60.0, 12.0, 0.1, false,
        { AT_VAL_FLOAT, { .float_val = 0.0 } },
        nullptr, 0, nullptr, 0
    },
    {
        CTRL_MUTE,
        "mute",
        "Mute",
        "",
        AT_CTRL_BUTTON,
        AT_STYLE_TOGGLE_BUTTON,
        AT_VAL_BOOL,
        0.0, 1.0, 1.0, false,
        { AT_VAL_BOOL, { .bool_val = false } },
        nullptr, 0, nullptr, 0
    },
    {
        CTRL_PEAK_METER_DB,
        "peak_meter_db",
        "Output Level",
        "dB",
        AT_CTRL_LEVEL_METER,
        AT_STYLE_DEFAULT,
        AT_VAL_FLOAT,
        -96.0, 12.0, 0.1, false,
        { AT_VAL_FLOAT, { .float_val = -96.0 } },
        nullptr, 0, nullptr, 0
    }
};

static const AT_FunctionDescriptor kFunctions[] = {
    {
        FUNC_TOGGLE_MUTE,
        "toggle_mute",
        "Toggle Mute",
        "Toggles the output mute state on or off.",
        AT_OP_ACTION_TRIGGER,
        CTRL_MUTE,
        AT_VAL_NONE,
        "", "", 0.0, 0.0, 0.0
    },
    {
        FUNC_SET_OUTPUT_GAIN_DB,
        "set_output_gain_db",
        "Set Output Volume (dB)...",
        "Sets the output volume to an exact decibel value.",
        AT_OP_SET_PARAMETER,
        CTRL_OUTPUT_GAIN_DB,
        AT_VAL_FLOAT,
        "Target Volume", "dB",
        -60.0, 12.0, 0.0
    },
    {
        FUNC_ADD_OUTPUT_GAIN_DB,
        "add_output_gain_db",
        "Change Output Volume By (dB)...",
        "Adds or subtracts a decibel delta to the output volume.",
        AT_OP_ADD_PARAMETER,
        CTRL_OUTPUT_GAIN_DB,
        AT_VAL_FLOAT,
        "Volume Delta", "dB",
        -72.0, 72.0, -3.0
    }
};

AT_NodeHandle AT_CDECL out_create_instance(const AT_HostServices* host_services, uint32_t runtime_node_id) {
    auto* state = new (std::nothrow) OutputNodeState();
    if (!state) return nullptr;
    state->host    = host_services;
    state->node_id = runtime_node_id;
    state->rebind_device();
    return reinterpret_cast<AT_NodeHandle>(state);
}

void AT_CDECL out_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    auto* state = reinterpret_cast<OutputNodeState*>(handle);
    if (state->dev_stream && state->host && state->host->release_hardware_device) {
        state->host->release_hardware_device(state->host->host_context, state->dev_stream);
        state->dev_stream = nullptr;
    }
    delete state;
}

const AT_ControlDescriptor* AT_CDECL out_get_controls(AT_NodeHandle /*handle*/, uint32_t* out_count) {
    if (out_count) *out_count = static_cast<uint32_t>(std::size(kControls));
    return kControls;
}

const AT_FunctionDescriptor* AT_CDECL out_get_functions(AT_NodeHandle /*handle*/, uint32_t* out_count) {
    if (out_count) *out_count = static_cast<uint32_t>(std::size(kFunctions));
    return kFunctions;
}

AT_Result AT_CDECL out_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_val) {
    if (!handle || !out_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<OutputNodeState*>(handle);
    switch (control_id) {
        case CTRL_DEVICE_ID:
            out_val->type       = AT_VAL_STRING;
            out_val->string_val = state->device_id.c_str();
            return AT_OK;
        case CTRL_OUTPUT_GAIN_DB:
            out_val->type      = AT_VAL_FLOAT;
            out_val->float_val = state->gain_db;
            return AT_OK;
        case CTRL_MUTE:
            out_val->type     = AT_VAL_BOOL;
            out_val->bool_val = state->muted;
            return AT_OK;
        case CTRL_PEAK_METER_DB:
            out_val->type      = AT_VAL_FLOAT;
            out_val->float_val = state->peak_db;
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL out_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_val) {
    if (!handle || !in_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<OutputNodeState*>(handle);
    switch (control_id) {
        case CTRL_DEVICE_ID:
            if (in_val->type != AT_VAL_STRING || !in_val->string_val) return AT_ERR_INVALID_ARG;
            state->device_id = in_val->string_val;
            state->rebind_device();
            return AT_OK;
        case CTRL_OUTPUT_GAIN_DB:
            if (in_val->type != AT_VAL_FLOAT && in_val->type != AT_VAL_INT) return AT_ERR_INVALID_ARG;
            state->gain_db = std::clamp(
                (in_val->type == AT_VAL_FLOAT) ? in_val->float_val : static_cast<double>(in_val->int_val),
                -60.0, 12.0);
            state->update_target_gain();
            return AT_OK;
        case CTRL_MUTE:
            if (in_val->type != AT_VAL_BOOL) return AT_ERR_INVALID_ARG;
            state->muted = in_val->bool_val;
            state->update_target_gain();
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL out_invoke_function(AT_NodeHandle handle, uint32_t function_id, const AT_ControlValue* param_val) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<OutputNodeState*>(handle);
    switch (function_id) {
        case FUNC_TOGGLE_MUTE:
            state->muted = !state->muted;
            state->update_target_gain();
            return AT_OK;
        case FUNC_SET_OUTPUT_GAIN_DB:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->gain_db = std::clamp(
                (param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val),
                -60.0, 12.0);
            state->update_target_gain();
            return AT_OK;
        case FUNC_ADD_OUTPUT_GAIN_DB:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->gain_db = std::clamp(
                state->gain_db + ((param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val)),
                -60.0, 12.0);
            state->update_target_gain();
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL out_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t /*max_buffer_size*/, uint32_t /*max_connections*/) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<OutputNodeState*>(handle);
    state->gain_linear.prepare(sample_rate, 2.0f);
    const float target = state->muted ? 0.0f : at::db_to_linear(static_cast<float>(state->gain_db));
    state->gain_linear.set_immediate(target);
    return AT_OK;
}

void AT_CDECL out_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* state = reinterpret_cast<OutputNodeState*>(handle);

    const uint32_t channels = ctx->output_bus.num_channels;
    const uint32_t frames   = ctx->num_frames;

    // 1. Sum all connected upstream audio input buses into our scratch output_bus
    for (uint32_t b = 0; b < ctx->input_bus_count; ++b) {
        const AT_AudioBusView& in_bus = ctx->input_buses[b];
        if (!in_bus.channel_ptrs || in_bus.num_channels == 0) continue;

        for (uint32_t ch = 0; ch < channels; ++ch) {
            const uint32_t src_ch = std::min(ch, in_bus.num_channels - 1);
            const float* src = in_bus.channel_ptrs[src_ch];
            float* dst = ctx->output_bus.channel_ptrs[ch];
            for (uint32_t f = 0; f < frames; ++f) {
                dst[f] += src[f];
            }
        }
    }

    // 2. Apply smoothed output gain & compute peak meter
    float peak = 0.0f;
    for (uint32_t f = 0; f < frames; ++f) {
        const float g = state->gain_linear.next_value();
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float sample = ctx->output_bus.channel_ptrs[ch][f] * g;
            ctx->output_bus.channel_ptrs[ch][f] = sample;
            peak = std::max(peak, std::abs(sample));
        }
    }
    state->peak_db = static_cast<double>(at::linear_to_db(peak));

    // 3. Accumulate into Hidden Master Output Device
    if (state->dev_stream && state->host && state->host->write_hardware_output) {
        state->host->write_hardware_output(
            state->host->host_context,
            state->dev_stream,
            ctx->output_bus.channel_ptrs,
            channels,
            frames);
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.output",
    "Audio Device Output",
    "Audio/Outputs",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_OUTPUT,
    AT_NODE_FLAG_AUTO_SUM_INPUTS
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &out_create_instance,
    &out_destroy_instance,
    &out_get_controls,
    &out_get_functions,
    &out_get_control_value,
    &out_set_control_value,
    &out_invoke_function,
    nullptr,
    nullptr,
    &out_prepare_audio,
    &out_process_audio,
    nullptr,
    nullptr
};

static const AT_PluginExportBundle kBundle = {
    sizeof(AT_PluginExportBundle),
    AT_ABI_VERSION,
    &kDescriptor,
    &kVTable
};

} // namespace

extern "C" AT_PLUGIN_EXPORT const AT_PluginExportBundle* AT_CDECL at_get_plugin_bundle(uint32_t host_abi_version) {
    if (host_abi_version != AT_ABI_VERSION) {
        return nullptr;
    }
    return &kBundle;
}
