// ============================================================================
// AudioTool Plugin DLL: TimerNode (Control/Triggers/TimerNode.dll)
// ============================================================================

#define AT_BUILDING_PLUGIN_DLL 1
#include "at_sdk/at_abi.h"
#include "at_version.h"

#include <algorithm>

namespace {

enum ControlIds : uint32_t {
    CTRL_INTERVAL_MS = 300,
    CTRL_ENABLED     = 301
};

enum FunctionIds : uint32_t {
    FUNC_TOGGLE_ENABLED  = 3001,
    FUNC_SET_INTERVAL_MS = 3002
};

struct TimerNodeState {
    const AT_HostServices* host         = nullptr;
    uint32_t               node_id      = 0;
    int64_t                interval_ms  = 1000;
    bool                   enabled      = true;
    uint64_t               last_fire_ns = 0;
    bool                   initialized  = false;
};

static const AT_ControlDescriptor kControls[] = {
    {
        CTRL_INTERVAL_MS,
        "interval_ms",
        "Pulse Interval",
        "ms",
        AT_CTRL_RANGE_TEXT_INPUT,
        AT_STYLE_NUMERIC_BOX,
        AT_VAL_INT,
        1.0, 3600000.0, 1.0, false,
        { AT_VAL_INT, { .int_val = 1000 } },
        nullptr, 0, nullptr, 0
    },
    {
        CTRL_ENABLED,
        "enabled",
        "Timer Enabled",
        "",
        AT_CTRL_BUTTON,
        AT_STYLE_TOGGLE_BUTTON,
        AT_VAL_BOOL,
        0.0, 1.0, 1.0, false,
        { AT_VAL_BOOL, { .bool_val = true } },
        nullptr, 0, nullptr, 0
    }
};

static const AT_FunctionDescriptor kFunctions[] = {
    {
        FUNC_TOGGLE_ENABLED,
        "toggle_enabled",
        "Toggle Timer Enabled",
        "Starts or pauses the periodic pulse timer.",
        AT_OP_ACTION_TRIGGER,
        CTRL_ENABLED,
        AT_VAL_NONE,
        "", "", 0.0, 0.0, 0.0
    },
    {
        FUNC_SET_INTERVAL_MS,
        "set_interval_ms",
        "Set Interval (ms)...",
        "Sets the timer period in milliseconds.",
        AT_OP_SET_PARAMETER,
        CTRL_INTERVAL_MS,
        AT_VAL_INT,
        "Interval", "ms",
        1.0, 3600000.0, 1000.0
    }
};

AT_NodeHandle AT_CDECL tmr_create_instance(const AT_HostServices* host_services, uint32_t runtime_node_id) {
    auto* state = new (std::nothrow) TimerNodeState();
    if (!state) return nullptr;
    state->host    = host_services;
    state->node_id = runtime_node_id;
    return reinterpret_cast<AT_NodeHandle>(state);
}

void AT_CDECL tmr_destroy_instance(AT_NodeHandle handle) {
    delete reinterpret_cast<TimerNodeState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL tmr_get_controls(AT_NodeHandle /*handle*/, uint32_t* out_count) {
    if (out_count) *out_count = static_cast<uint32_t>(std::size(kControls));
    return kControls;
}

const AT_FunctionDescriptor* AT_CDECL tmr_get_functions(AT_NodeHandle /*handle*/, uint32_t* out_count) {
    if (out_count) *out_count = static_cast<uint32_t>(std::size(kFunctions));
    return kFunctions;
}

AT_Result AT_CDECL tmr_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_val) {
    if (!handle || !out_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<TimerNodeState*>(handle);
    switch (control_id) {
        case CTRL_INTERVAL_MS:
            out_val->type    = AT_VAL_INT;
            out_val->int_val = state->interval_ms;
            return AT_OK;
        case CTRL_ENABLED:
            out_val->type     = AT_VAL_BOOL;
            out_val->bool_val = state->enabled;
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL tmr_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_val) {
    if (!handle || !in_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<TimerNodeState*>(handle);
    switch (control_id) {
        case CTRL_INTERVAL_MS:
            if (in_val->type != AT_VAL_INT && in_val->type != AT_VAL_FLOAT) return AT_ERR_INVALID_ARG;
            state->interval_ms = std::clamp<int64_t>(
                (in_val->type == AT_VAL_INT) ? in_val->int_val : static_cast<int64_t>(in_val->float_val),
                1LL, 3600000LL);
            return AT_OK;
        case CTRL_ENABLED:
            if (in_val->type != AT_VAL_BOOL) return AT_ERR_INVALID_ARG;
            state->enabled = in_val->bool_val;
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

AT_Result AT_CDECL tmr_invoke_function(AT_NodeHandle handle, uint32_t function_id, const AT_ControlValue* param_val) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<TimerNodeState*>(handle);
    switch (function_id) {
        case FUNC_TOGGLE_ENABLED:
            state->enabled = !state->enabled;
            return AT_OK;
        case FUNC_SET_INTERVAL_MS:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->interval_ms = std::clamp<int64_t>(
                (param_val->type == AT_VAL_INT) ? param_val->int_val : static_cast<int64_t>(param_val->float_val),
                1LL, 3600000LL);
            return AT_OK;
        default:
            return AT_ERR_NOT_FOUND;
    }
}

void AT_CDECL tmr_tick_control(AT_NodeHandle handle, uint64_t timestamp_ns) {
    if (!handle) return;
    auto* state = reinterpret_cast<TimerNodeState*>(handle);
    if (!state->enabled) return;

    if (!state->initialized) {
        state->last_fire_ns = timestamp_ns;
        state->initialized  = true;
        return;
    }

    const uint64_t period_ns = static_cast<uint64_t>(state->interval_ms) * 1000000ULL;
    if (timestamp_ns - state->last_fire_ns >= period_ns) {
        state->last_fire_ns = timestamp_ns;
        if (state->host && state->host->emit_control_output) {
            state->host->emit_control_output(state->host->host_context, state->node_id, 0);
        }
    }
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.control.timer",
    "Periodic Pulse Timer",
    "Control/Triggers",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_CONTROL,
    AT_AUDIO_CAT_NONE,
    AT_NODE_FLAG_NONE
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &tmr_create_instance,
    &tmr_destroy_instance,
    &tmr_get_controls,
    &tmr_get_functions,
    &tmr_get_control_value,
    &tmr_set_control_value,
    &tmr_invoke_function,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &tmr_tick_control
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

