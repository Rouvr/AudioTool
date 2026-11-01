// ============================================================================
// AudioTool Plugin DLL: VirtualInputNode (Audio/Sources/VirtualInputNode.dll)
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
    CTRL_DEVICE_ID     = 100,
    CTRL_INPUT_GAIN_DB = 101,
    CTRL_MUTE          = 102,
    CTRL_PEAK_METER_DB = 103
};

enum FunctionIds : uint32_t {
    FUNC_TOGGLE_MUTE       = 1001,
    FUNC_SET_INPUT_GAIN_DB = 1002,
    FUNC_ADD_INPUT_GAIN_DB = 1003
};

struct VirtualInputNodeState {
    const AT_HostServices* host        = nullptr;
    uint32_t               node_id     = 0;
    std::string            device_id   = "default_input";
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
            const bool is_loopback = (device_id == "default_loopback" || device_id.rfind(AT_LOOPBACK_PREFIX, 0) == 0);
            dev_stream = host->acquire_hardware_device(
                host->host_context,
                device_id.c_str(),
                true,
                is_loopback);
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
        "Audio Input Device",
        "",
        AT_CTRL_AUDIO_DEVICE_SELECT,
        AT_STYLE_DROPDOWN_COMBO,
        AT_VAL_STRING,
        0.0, 0.0, 0.0, false,
        { AT_VAL_STRING, { .string_val = "default_input" } },
        nullptr,
        AT_DEV_FILTER_INPUT_CAPTURE | AT_DEV_FILTER_LOOPBACK,
        nullptr, 0
    },
    {
        CTRL_INPUT_GAIN_DB,
        "input_gain_db",
        "Input Gain",
        "dB",
        AT_CTRL_SLIDER,
        AT_STYLE_HORIZONTAL_SLIDER,
        AT_VAL_FLOAT,
        -60.0, 24.0, 0.1, false,
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
        "Input Level",
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
        "Toggles the microphone/input mute state on or off.",
        AT_OP_ACTION_TRIGGER,
        CTRL_MUTE,
        AT_VAL_NONE,
        "", "", 0.0, 0.0, 0.0
    },
    {
        FUNC_SET_INPUT_GAIN_DB,
        "set_input_gain_db",
        "Set Input Gain (dB)...",
        "Sets the input gain slider to an exact decibel value.",
        AT_OP_SET_PARAMETER,
        CTRL_INPUT_GAIN_DB,
        AT_VAL_FLOAT,
        "Target Gain", "dB",
        -60.0, 24.0, 0.0
    },
    {
        FUNC_ADD_INPUT_GAIN_DB,
        "add_input_gain_db",
        "Change Input Gain By (dB)...",
        "Adds or subtracts a decibel delta to the current input gain.",
        AT_OP_ADD_PARAMETER,
        CTRL_INPUT_GAIN_DB,
        AT_VAL_FLOAT,
        "Gain Delta", "dB",
        -84.0, 84.0, -3.0
    }
};

AT_NodeHandle AT_CDECL vi_create_instance(const AT_HostServices* host_services, uint32_t runtime_node_id) {
    auto* state = new (std::nothrow) VirtualInputNodeState();
    if (!state) return nullptr;
    state->host    = host_services;
    state->node_id = runtime_node_id;
    state->rebind_device();
    return reinterpret_cast<AT_NodeHandle>(state);
}

void AT_CDECL vi_destroy_instance(AT_NodeHandle handle) {
    if (!handle) return;
    auto* state = reinterpret_cast<VirtualInputNodeState*>(handle);
    if (state->dev_stream && state->host && state->host->release_hardware_device) {
        state->host->release_hardware_device(state->host->host_context, state->dev_stream);
        state->dev_stream = nullptr;
    }
    delete state;
}

const AT_ControlDescriptor* AT_CDECL vi_get_controls(AT_NodeHandle /*handle*/, uint32_t* out_count) {
    if (out_count) *out_count = static_cast<uint32_t>(std::size(kControls));
    return kControls;
}

const AT_FunctionDescriptor* AT_CDECL vi_get_functions(AT_NodeHandle /*handle*/, uint32_t* out_count) {
    if (out_count) *out_count = static_cast<uint32_t>(std::size(kFunctions));
    return kFunctions;
}

AT_Result AT_CDECL vi_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_val) {
    if (!handle || !out_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VirtualInputNodeState*>(handle);
    switch (control_id) {
        case CTRL_DEVICE_ID:
            out_val->type       = AT_VAL_STRING;
            out_val->string_val = state->device_id.c_str();
            return AT_OK;
        case CTRL_INPUT_GAIN_DB:
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

AT_Result AT_CDECL vi_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_val) {
    if (!handle || !in_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VirtualInputNodeState*>(handle);
    switch (control_id) {
        case CTRL_DEVICE_ID:
            if (in_val->type != AT_VAL_STRING || !in_val->string_val) return AT_ERR_INVALID_ARG;
            state->device_id = in_val->string_val;
            state->rebind_device();
            return AT_OK;
        case CTRL_INPUT_GAIN_DB:
            if (in_val->type != AT_VAL_FLOAT && in_val->type != AT_VAL_INT) return AT_ERR_INVALID_ARG;
            state->gain_db = std::clamp(
                (in_val->type == AT_VAL_FLOAT) ? in_val->float_val : static_cast<double>(in_val->int_val),
                -60.0, 24.0);
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

AT_Result AT_CDECL vi_invoke_function(AT_NodeHandle handle, uint32_t function_id, const AT_ControlValue* param_val) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VirtualInputNodeState*>(handle);
    switch (function_id) {
        case FUNC_TOGGLE_MUTE:
            state->muted = !state->muted;
            state->update_target_gain();
            return AT_OK;
        case FUNC_SET_INPUT_GAIN_DB:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->gain_db = std::clamp(
                (param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val),
                -60.0, 24.0);
            state->update_target_gain();
            return AT_OK;
        case FUNC_ADD_INPUT_GAIN_DB:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->gain_db = std::clamp(
                state->gain_db + ((param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val)),
                -60.0, 24.0);
            state->update_target_gain();
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL vi_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t /*max_buffer_size*/, uint32_t /*max_connections*/) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VirtualInputNodeState*>(handle);
    state->gain_linear.prepare(sample_rate, 2.0f);
    const float target = state->muted ? 0.0f : at::db_to_linear(static_cast<float>(state->gain_db));
    state->gain_linear.set_immediate(target);
    return AT_OK;
}

void AT_CDECL vi_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* state = reinterpret_cast<VirtualInputNodeState*>(handle);

    const uint32_t channels = ctx->output_bus.num_channels;
    const uint32_t frames   = ctx->num_frames;

    if (state->dev_stream && state->host && state->host->read_hardware_input) {
        state->host->read_hardware_input(
            state->host->host_context,
            state->dev_stream,
            ctx->output_bus.channel_ptrs,
            channels,
            frames);
    }

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
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.virtual_input",
    "Virtual Audio Input",
    "Audio/Sources",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_SOURCE,
    AT_NODE_FLAG_NONE
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &vi_create_instance,
    &vi_destroy_instance,
    &vi_get_controls,
    &vi_get_functions,
    &vi_get_control_value,
    &vi_set_control_value,
    &vi_invoke_function,
    nullptr,
    nullptr,
    &vi_prepare_audio,
    &vi_process_audio,
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

