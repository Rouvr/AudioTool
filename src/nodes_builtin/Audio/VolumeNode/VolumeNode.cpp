// ============================================================================
// AudioTool Plugin DLL: VolumeNode (Audio/Processing/VolumeNode.dll)
//
// Features:
//   - Per-input channel strips (gain dB, mute) dynamically generated as inputs connect
//   - Input labels mirror upstream node display names via on_input_sources_changed
//   - Master volume slider (dB) and Master mute
//   - Auto-normalization across active inputs (RMS estimation with noise gating)
//   - Exposed functions: set/add master volume, toggle mute, set/add input gain, toggle auto-norm
// ============================================================================

#define AT_BUILDING_PLUGIN_DLL 1
#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "at_version.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kMaxSupportedInputs = 16;

// Fixed control IDs for master section
enum MasterControlIds : uint32_t {
    CTRL_MASTER_VOLUME_DB    = 100,
    CTRL_MASTER_MUTE         = 101,
    CTRL_AUTO_NORMALIZE      = 102,
    CTRL_TARGET_NORM_DB      = 103,
    CTRL_OUT_METER_DB        = 104
};

// Base control IDs for dynamic per-input strips:
// Input i has:
//   Gain ID: 1000 + i * 10 + 0
//   Mute ID: 1000 + i * 10 + 1
//   Meter ID: 1000 + i * 10 + 2
constexpr uint32_t kInputControlBase = 1000;
constexpr uint32_t kInputControlStride = 10;

enum MasterFunctionIds : uint32_t {
    FUNC_TOGGLE_MASTER_MUTE    = 2001,
    FUNC_SET_MASTER_VOLUME_DB  = 2002,
    FUNC_ADD_MASTER_VOLUME_DB  = 2003,
    FUNC_TOGGLE_AUTO_NORMALIZE = 2004,
    FUNC_SET_INPUT_GAIN_DB     = 2010, // function_id 2010 + i
    FUNC_ADD_INPUT_GAIN_DB     = 2030  // function_id 2030 + i
};

struct InputChannelStrip {
    uint32_t          source_node_id = 0;
    std::string       name = "Input";
    double            gain_db = 0.0;
    bool              muted = false;
    double            peak_db = -96.0;
    at::SmoothedFloat smoothed_gain{1.0f};

    // Auto-normalization RMS tracking
    float             filtered_rms_sq = 0.0f;
    float             auto_norm_gain = 1.0f;

    // Generated descriptors storage
    std::string       gain_ident;
    std::string       gain_name;
    std::string       mute_ident;
    std::string       mute_name;
    std::string       meter_ident;
    std::string       meter_name;
};

struct VolumeNodeState {
    const AT_HostServices*         host = nullptr;
    uint32_t                       runtime_node_id = 0;

    double                         master_volume_db = 0.0;
    bool                           master_muted = false;
    bool                           auto_normalize = false;
    double                         target_norm_db = -16.0;
    double                         master_peak_db = -96.0;
    at::SmoothedFloat              master_smoothed_gain{1.0f};

    uint32_t                       sample_rate = 48000;
    uint32_t                       connected_inputs = 0;
    std::vector<InputChannelStrip> input_strips;

    // Reflection caches
    std::vector<AT_ControlDescriptor>  cached_controls;
    std::vector<AT_FunctionDescriptor> cached_functions;
    std::vector<std::string>           string_storage; // holds strings for descriptors

    void rebuild_descriptors() {
        cached_controls.clear();
        cached_functions.clear();
        string_storage.clear();

        // 1. Master controls
        cached_controls.push_back({
            CTRL_MASTER_VOLUME_DB,
            "master_volume_db",
            "Master Volume",
            "dB",
            AT_CTRL_SLIDER,
            AT_STYLE_HORIZONTAL_SLIDER,
            AT_VAL_FLOAT,
            -60.0, 12.0, 0.1, false,
            { AT_VAL_FLOAT, { .float_val = 0.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_MASTER_MUTE,
            "master_mute",
            "Master Mute",
            "",
            AT_CTRL_BUTTON,
            AT_STYLE_TOGGLE_BUTTON,
            AT_VAL_BOOL,
            0.0, 1.0, 1.0, false,
            { AT_VAL_BOOL, { .bool_val = false } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_AUTO_NORMALIZE,
            "auto_normalize",
            "Auto Normalize",
            "",
            AT_CTRL_BUTTON,
            AT_STYLE_TOGGLE_BUTTON,
            AT_VAL_BOOL,
            0.0, 1.0, 1.0, false,
            { AT_VAL_BOOL, { .bool_val = false } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_TARGET_NORM_DB,
            "target_norm_db",
            "Target LUFS/RMS",
            "dB",
            AT_CTRL_RANGE_TEXT_INPUT,
            AT_STYLE_NUMERIC_BOX,
            AT_VAL_FLOAT,
            -40.0, 0.0, 0.5, false,
            { AT_VAL_FLOAT, { .float_val = -16.0 } },
            nullptr, 0, nullptr, 0
        });

        cached_controls.push_back({
            CTRL_OUT_METER_DB,
            "output_meter_db",
            "Output Level",
            "dB",
            AT_CTRL_LEVEL_METER,
            AT_STYLE_DEFAULT,
            AT_VAL_FLOAT,
            -96.0, 12.0, 0.1, false,
            { AT_VAL_FLOAT, { .float_val = -96.0 } },
            nullptr, 0, nullptr, 0
        });

        // 2. Per-input channel strips
        for (uint32_t i = 0; i < input_strips.size(); ++i) {
            auto& strip = input_strips[i];
            strip.gain_ident = "input_" + std::to_string(i) + "_gain_db";
            strip.gain_name  = strip.name + " Gain";
            strip.mute_ident = "input_" + std::to_string(i) + "_mute";
            strip.mute_name  = strip.name + " Mute";
            strip.meter_ident = "input_" + std::to_string(i) + "_meter_db";
            strip.meter_name = strip.name + " Level";

            uint32_t gid = kInputControlBase + i * kInputControlStride + 0;
            uint32_t mid = kInputControlBase + i * kInputControlStride + 1;
            uint32_t tid = kInputControlBase + i * kInputControlStride + 2;

            cached_controls.push_back({
                gid,
                strip.gain_ident.c_str(),
                strip.gain_name.c_str(),
                "dB",
                AT_CTRL_SLIDER,
                AT_STYLE_HORIZONTAL_SLIDER,
                AT_VAL_FLOAT,
                -60.0, 24.0, 0.1, false,
                { AT_VAL_FLOAT, { .float_val = 0.0 } },
                nullptr, 0, nullptr, 0
            });

            cached_controls.push_back({
                mid,
                strip.mute_ident.c_str(),
                strip.mute_name.c_str(),
                "",
                AT_CTRL_BUTTON,
                AT_STYLE_TOGGLE_BUTTON,
                AT_VAL_BOOL,
                0.0, 1.0, 1.0, false,
                { AT_VAL_BOOL, { .bool_val = false } },
                nullptr, 0, nullptr, 0
            });

            cached_controls.push_back({
                tid,
                strip.meter_ident.c_str(),
                strip.meter_name.c_str(),
                "dB",
                AT_CTRL_LEVEL_METER,
                AT_STYLE_DEFAULT,
                AT_VAL_FLOAT,
                -96.0, 12.0, 0.1, false,
                { AT_VAL_FLOAT, { .float_val = -96.0 } },
                nullptr, 0, nullptr, 0
            });
        }

        // 3. Master functions
        cached_functions.push_back({
            FUNC_TOGGLE_MASTER_MUTE,
            "toggle_master_mute",
            "Toggle Master Mute",
            "Toggles the mixer master mute state on/off.",
            AT_OP_ACTION_TRIGGER,
            CTRL_MASTER_MUTE,
            AT_VAL_NONE,
            "", "", 0.0, 0.0, 0.0
        });

        cached_functions.push_back({
            FUNC_SET_MASTER_VOLUME_DB,
            "set_master_volume_db",
            "Set Master Volume (dB)...",
            "Sets the master volume to an exact decibel value.",
            AT_OP_SET_PARAMETER,
            CTRL_MASTER_VOLUME_DB,
            AT_VAL_FLOAT,
            "Volume", "dB",
            -60.0, 12.0, 0.0
        });

        cached_functions.push_back({
            FUNC_ADD_MASTER_VOLUME_DB,
            "add_master_volume_db",
            "Change Master Volume By (dB)...",
            "Adds/subtracts a decibel delta to the master volume.",
            AT_OP_ADD_PARAMETER,
            CTRL_MASTER_VOLUME_DB,
            AT_VAL_FLOAT,
            "Delta", "dB",
            -72.0, 72.0, -3.0
        });

        cached_functions.push_back({
            FUNC_TOGGLE_AUTO_NORMALIZE,
            "toggle_auto_normalize",
            "Toggle Auto Normalize",
            "Enables or disables automatic loudness normalization.",
            AT_OP_ACTION_TRIGGER,
            CTRL_AUTO_NORMALIZE,
            AT_VAL_NONE,
            "", "", 0.0, 0.0, 0.0
        });

        // 4. Per-input functions (up to first 8 inputs)
        for (uint32_t i = 0; i < std::min<uint32_t>(static_cast<uint32_t>(input_strips.size()), 8u); ++i) {
            uint32_t gid = kInputControlBase + i * kInputControlStride + 0;
            auto& strip = input_strips[i];

            std::string set_ident = "set_input_" + std::to_string(i) + "_gain_db";
            std::string set_name  = "Set " + strip.name + " Gain (dB)...";
            std::string add_ident = "add_input_" + std::to_string(i) + "_gain_db";
            std::string add_name  = "Change " + strip.name + " Gain By (dB)...";

            string_storage.push_back(set_ident);
            const char* s_id_ptr = string_storage.back().c_str();
            string_storage.push_back(set_name);
            const char* s_nm_ptr = string_storage.back().c_str();

            cached_functions.push_back({
                FUNC_SET_INPUT_GAIN_DB + i,
                s_id_ptr,
                s_nm_ptr,
                "Sets channel gain to an exact decibel value.",
                AT_OP_SET_PARAMETER,
                gid,
                AT_VAL_FLOAT,
                "Gain", "dB",
                -60.0, 24.0, 0.0
            });

            string_storage.push_back(add_ident);
            const char* a_id_ptr = string_storage.back().c_str();
            string_storage.push_back(add_name);
            const char* a_nm_ptr = string_storage.back().c_str();

            cached_functions.push_back({
                FUNC_ADD_INPUT_GAIN_DB + i,
                a_id_ptr,
                a_nm_ptr,
                "Adjusts channel gain by a delta.",
                AT_OP_ADD_PARAMETER,
                gid,
                AT_VAL_FLOAT,
                "Delta", "dB",
                -84.0, 84.0, -3.0
            });
        }
    }

    void update_target_gains() {
        float mg = master_muted ? 0.0f : at::db_to_linear(static_cast<float>(master_volume_db));
        master_smoothed_gain.set_target(mg);
        for (auto& s : input_strips) {
            float g = s.muted ? 0.0f : at::db_to_linear(static_cast<float>(s.gain_db));
            s.smoothed_gain.set_target(g);
        }
    }
};

AT_NodeHandle AT_CDECL vol_create_instance(const AT_HostServices* host_services, uint32_t runtime_node_id) {
    auto* state = new (std::nothrow) VolumeNodeState();
    if (!state) return nullptr;
    state->host            = host_services;
    state->runtime_node_id = runtime_node_id;
    state->rebuild_descriptors();
    return reinterpret_cast<AT_NodeHandle>(state);
}

void AT_CDECL vol_destroy_instance(AT_NodeHandle handle) {
    delete reinterpret_cast<VolumeNodeState*>(handle);
}

const AT_ControlDescriptor* AT_CDECL vol_get_controls(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) {
        if (out_count) *out_count = 0;
        return nullptr;
    }
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(state->cached_controls.size());
    return state->cached_controls.data();
}

const AT_FunctionDescriptor* AT_CDECL vol_get_functions(AT_NodeHandle handle, uint32_t* out_count) {
    if (!handle) {
        if (out_count) *out_count = 0;
        return nullptr;
    }
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);
    if (out_count) *out_count = static_cast<uint32_t>(state->cached_functions.size());
    return state->cached_functions.data();
}

AT_Result AT_CDECL vol_get_control_value(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_val) {
    if (!handle || !out_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);

    switch (control_id) {
        case CTRL_MASTER_VOLUME_DB:
            out_val->type      = AT_VAL_FLOAT;
            out_val->float_val = state->master_volume_db;
            return AT_OK;
        case CTRL_MASTER_MUTE:
            out_val->type     = AT_VAL_BOOL;
            out_val->bool_val = state->master_muted;
            return AT_OK;
        case CTRL_AUTO_NORMALIZE:
            out_val->type     = AT_VAL_BOOL;
            out_val->bool_val = state->auto_normalize;
            return AT_OK;
        case CTRL_TARGET_NORM_DB:
            out_val->type      = AT_VAL_FLOAT;
            out_val->float_val = state->target_norm_db;
            return AT_OK;
        case CTRL_OUT_METER_DB:
            out_val->type      = AT_VAL_FLOAT;
            out_val->float_val = state->master_peak_db;
            return AT_OK;
        default:
            break;
    }

    if (control_id >= kInputControlBase) {
        uint32_t idx = (control_id - kInputControlBase) / kInputControlStride;
        uint32_t sub = (control_id - kInputControlBase) % kInputControlStride;
        if (idx < state->input_strips.size()) {
            if (sub == 0) {
                out_val->type      = AT_VAL_FLOAT;
                out_val->float_val = state->input_strips[idx].gain_db;
                return AT_OK;
            } else if (sub == 1) {
                out_val->type     = AT_VAL_BOOL;
                out_val->bool_val = state->input_strips[idx].muted;
                return AT_OK;
            } else if (sub == 2) {
                out_val->type      = AT_VAL_FLOAT;
                out_val->float_val = state->input_strips[idx].peak_db;
                return AT_OK;
            }
        }
    }

    return AT_ERR_NOT_FOUND;
}

AT_Result AT_CDECL vol_set_control_value(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_val) {
    if (!handle || !in_val) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);

    switch (control_id) {
        case CTRL_MASTER_VOLUME_DB:
            if (in_val->type != AT_VAL_FLOAT && in_val->type != AT_VAL_INT) return AT_ERR_INVALID_ARG;
            state->master_volume_db = std::clamp(
                (in_val->type == AT_VAL_FLOAT) ? in_val->float_val : static_cast<double>(in_val->int_val),
                -60.0, 12.0);
            state->update_target_gains();
            return AT_OK;
        case CTRL_MASTER_MUTE:
            if (in_val->type != AT_VAL_BOOL) return AT_ERR_INVALID_ARG;
            state->master_muted = in_val->bool_val;
            state->update_target_gains();
            return AT_OK;
        case CTRL_AUTO_NORMALIZE:
            if (in_val->type != AT_VAL_BOOL) return AT_ERR_INVALID_ARG;
            state->auto_normalize = in_val->bool_val;
            return AT_OK;
        case CTRL_TARGET_NORM_DB:
            if (in_val->type != AT_VAL_FLOAT && in_val->type != AT_VAL_INT) return AT_ERR_INVALID_ARG;
            state->target_norm_db = std::clamp(
                (in_val->type == AT_VAL_FLOAT) ? in_val->float_val : static_cast<double>(in_val->int_val),
                -40.0, 0.0);
            return AT_OK;
        default:
            break;
    }

    if (control_id >= kInputControlBase) {
        uint32_t idx = (control_id - kInputControlBase) / kInputControlStride;
        uint32_t sub = (control_id - kInputControlBase) % kInputControlStride;
        if (idx < state->input_strips.size()) {
            if (sub == 0) {
                if (in_val->type != AT_VAL_FLOAT && in_val->type != AT_VAL_INT) return AT_ERR_INVALID_ARG;
                state->input_strips[idx].gain_db = std::clamp(
                    (in_val->type == AT_VAL_FLOAT) ? in_val->float_val : static_cast<double>(in_val->int_val),
                    -60.0, 24.0);
                state->update_target_gains();
                return AT_OK;
            } else if (sub == 1) {
                if (in_val->type != AT_VAL_BOOL) return AT_ERR_INVALID_ARG;
                state->input_strips[idx].muted = in_val->bool_val;
                state->update_target_gains();
                return AT_OK;
            }
        }
    }

    return AT_ERR_NOT_FOUND;
}

AT_Result AT_CDECL vol_invoke_function(AT_NodeHandle handle, uint32_t function_id, const AT_ControlValue* param_val) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);

    switch (function_id) {
        case FUNC_TOGGLE_MASTER_MUTE:
            state->master_muted = !state->master_muted;
            state->update_target_gains();
            return AT_OK;
        case FUNC_SET_MASTER_VOLUME_DB:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->master_volume_db = std::clamp(
                (param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val),
                -60.0, 12.0);
            state->update_target_gains();
            return AT_OK;
        case FUNC_ADD_MASTER_VOLUME_DB:
            if (!param_val) return AT_ERR_INVALID_ARG;
            state->master_volume_db = std::clamp(
                state->master_volume_db + ((param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val)),
                -60.0, 12.0);
            state->update_target_gains();
            return AT_OK;
        case FUNC_TOGGLE_AUTO_NORMALIZE:
            state->auto_normalize = !state->auto_normalize;
            return AT_OK;
        default:
            break;
    }

    if (function_id >= FUNC_SET_INPUT_GAIN_DB && function_id < FUNC_SET_INPUT_GAIN_DB + kMaxSupportedInputs) {
        uint32_t idx = function_id - FUNC_SET_INPUT_GAIN_DB;
        if (idx < state->input_strips.size() && param_val) {
            state->input_strips[idx].gain_db = std::clamp(
                (param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val),
                -60.0, 24.0);
            state->update_target_gains();
            return AT_OK;
        }
    } else if (function_id >= FUNC_ADD_INPUT_GAIN_DB && function_id < FUNC_ADD_INPUT_GAIN_DB + kMaxSupportedInputs) {
        uint32_t idx = function_id - FUNC_ADD_INPUT_GAIN_DB;
        if (idx < state->input_strips.size() && param_val) {
            state->input_strips[idx].gain_db = std::clamp(
                state->input_strips[idx].gain_db + ((param_val->type == AT_VAL_FLOAT) ? param_val->float_val : static_cast<double>(param_val->int_val)),
                -60.0, 24.0);
            state->update_target_gains();
            return AT_OK;
        }
    }

    return AT_ERR_NOT_FOUND;
}

AT_Result AT_CDECL vol_prepare_audio(AT_NodeHandle handle, uint32_t sample_rate, uint32_t /*max_buffer_size*/, uint32_t /*max_connections*/) {
    if (!handle) return AT_ERR_INVALID_ARG;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);
    state->sample_rate = sample_rate;
    state->master_smoothed_gain.prepare(sample_rate, 4.0f);
    float mg = state->master_muted ? 0.0f : at::db_to_linear(static_cast<float>(state->master_volume_db));
    state->master_smoothed_gain.set_immediate(mg);

    for (auto& s : state->input_strips) {
        s.smoothed_gain.prepare(sample_rate, 4.0f);
        float g = s.muted ? 0.0f : at::db_to_linear(static_cast<float>(s.gain_db));
        s.smoothed_gain.set_immediate(g);
    }
    return AT_OK;
}

void AT_CDECL vol_on_input_sources_changed(
    AT_NodeHandle handle,
    uint32_t input_count,
    const uint32_t* source_node_ids,
    const char* const* source_node_names) {
    if (!handle) return;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);

    std::vector<InputChannelStrip> new_strips;
    new_strips.reserve(input_count);

    for (uint32_t i = 0; i < input_count; ++i) {
        uint32_t src_id = source_node_ids ? source_node_ids[i] : 0;
        const char* name_str = (source_node_names && source_node_names[i]) ? source_node_names[i] : "Input";

        // Try to match existing strip by source_node_id to preserve volume/mute settings
        InputChannelStrip strip;
        strip.source_node_id = src_id;
        strip.name = name_str;

        bool found_existing = false;
        for (const auto& existing : state->input_strips) {
            if (existing.source_node_id == src_id && src_id != 0) {
                strip.gain_db = existing.gain_db;
                strip.muted = existing.muted;
                strip.peak_db = existing.peak_db;
                found_existing = true;
                break;
            }
        }
        if (!found_existing && i < state->input_strips.size()) {
            strip.gain_db = state->input_strips[i].gain_db;
            strip.muted   = state->input_strips[i].muted;
        }

        strip.smoothed_gain.prepare(state->sample_rate, 4.0f);
        float g = strip.muted ? 0.0f : at::db_to_linear(static_cast<float>(strip.gain_db));
        strip.smoothed_gain.set_immediate(g);

        new_strips.push_back(std::move(strip));
    }

    state->input_strips = std::move(new_strips);
    state->connected_inputs = input_count;
    state->rebuild_descriptors();

    if (state->host && state->host->notify_controls_dirty) {
        state->host->notify_controls_dirty(state->host->host_context, state->runtime_node_id);
    }
}

void AT_CDECL vol_on_connections_changed(AT_NodeHandle handle, uint32_t input_count, uint32_t /*output_count*/) {
    if (!handle) return;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);
    if (state->input_strips.size() != input_count) {
        vol_on_input_sources_changed(handle, input_count, nullptr, nullptr);
    }
}

void AT_CDECL vol_process_audio(AT_NodeHandle handle, const AT_AudioProcessContext* ctx) {
    if (!handle || !ctx) return;
    auto* state = reinterpret_cast<VolumeNodeState*>(handle);

    const uint32_t channels = ctx->output_bus.num_channels;
    const uint32_t frames   = ctx->num_frames;

    // Zero output bus first
    for (uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ctx->output_bus.channel_ptrs[ch], 0, frames * sizeof(float));
    }

    const uint32_t bus_count = std::min(ctx->input_bus_count, static_cast<uint32_t>(state->input_strips.size()));
    const float target_rms = at::db_to_linear(static_cast<float>(state->target_norm_db));

    for (uint32_t b = 0; b < bus_count; ++b) {
        auto& strip = state->input_strips[b];
        const AT_AudioBusView& in_bus = ctx->input_buses[b];
        if (!in_bus.channel_ptrs || in_bus.num_channels == 0) continue;

        // Measure input energy (sum of squares)
        float energy = 0.0f;
        float in_peak = 0.0f;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const uint32_t src_ch = std::min(ch, in_bus.num_channels - 1);
            const float* src = in_bus.channel_ptrs[src_ch];
            for (uint32_t f = 0; f < frames; ++f) {
                float v = src[f];
                energy += v * v;
                in_peak = std::max(in_peak, std::abs(v));
            }
        }
        strip.peak_db = static_cast<double>(at::linear_to_db(in_peak));

        // Auto-normalization gain follower (EBU R128 gated ballistic)
        float auto_gain = 1.0f;
        if (state->auto_normalize && in_peak > 1e-4f) {
            float mean_sq = energy / static_cast<float>(frames * channels);
            // EMA filter with ~300ms time constant
            strip.filtered_rms_sq = 0.95f * strip.filtered_rms_sq + 0.05f * mean_sq;
            float current_rms = std::sqrt(std::max(1e-8f, strip.filtered_rms_sq));

            // Only normalize if signal is above -45 dBFS noise gate
            if (current_rms > 0.0056f) {
                float desired_gain = target_rms / current_rms;
                desired_gain = std::clamp(desired_gain, 0.1f, 8.0f); // -20 dB to +18 dB
                strip.auto_norm_gain = 0.98f * strip.auto_norm_gain + 0.02f * desired_gain;
            }
            auto_gain = strip.auto_norm_gain;
        }

        // Accumulate this input into output_bus
        for (uint32_t f = 0; f < frames; ++f) {
            float manual_gain = strip.smoothed_gain.next_value();
            float eff_gain = manual_gain * auto_gain;
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const uint32_t src_ch = std::min(ch, in_bus.num_channels - 1);
                ctx->output_bus.channel_ptrs[ch][f] += in_bus.channel_ptrs[src_ch][f] * eff_gain;
            }
        }
    }

    // Apply master volume and compute master peak
    float master_peak = 0.0f;
    for (uint32_t f = 0; f < frames; ++f) {
        float mg = state->master_smoothed_gain.next_value();
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float sample = ctx->output_bus.channel_ptrs[ch][f] * mg;
            ctx->output_bus.channel_ptrs[ch][f] = sample;
            master_peak = std::max(master_peak, std::abs(sample));
        }
    }
    state->master_peak_db = static_cast<double>(at::linear_to_db(master_peak));
}

static const AT_NodeDescriptor kDescriptor = {
    sizeof(AT_NodeDescriptor),
    "at.builtin.audio.volume",
    "Volume & Multi-Channel Mixer",
    "Audio/Processing",
    "Rouvr",
    AT_VERSION_STRING,
    AT_BUILD_ID,
    AT_NODE_PLANE_AUDIO,
    AT_AUDIO_CAT_PROCESSING,
    AT_NODE_FLAG_DYNAMIC_CONTROLS
};

static const AT_NodeVTable kVTable = {
    sizeof(AT_NodeVTable),
    &vol_create_instance,
    &vol_destroy_instance,
    &vol_get_controls,
    &vol_get_functions,
    &vol_get_control_value,
    &vol_set_control_value,
    &vol_invoke_function,
    nullptr,
    nullptr,
    &vol_prepare_audio,
    &vol_process_audio,
    &vol_on_connections_changed,
    nullptr,
    &vol_on_input_sources_changed
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
