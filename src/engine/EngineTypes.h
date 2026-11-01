#pragma once
// ============================================================================
// AudioTool (AT) — Engine configuration & state snapshot DTOs
//
// Snapshots are deep copies (std::string owned) taken under the engine mutex,
// so the GUI never holds pointers into plugin memory.
// ============================================================================

#include "at_sdk/at_abi.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace at {

struct EngineConfig {
    uint32_t sample_rate              = 48000;
    uint32_t buffer_size_frames       = 256;
    uint32_t max_connections_per_port = 4;
};

// Owned variant of AT_ControlValue
struct OwnedValue {
    AT_ValueType type      = AT_VAL_NONE;
    bool         bool_val  = false;
    int64_t      int_val   = 0;
    double       float_val = 0.0;
    std::string  string_val;

    static OwnedValue from(const AT_ControlValue& v) {
        OwnedValue o;
        o.type = v.type;
        switch (v.type) {
            case AT_VAL_BOOL:   o.bool_val = v.bool_val; break;
            case AT_VAL_INT:    o.int_val = v.int_val; o.float_val = static_cast<double>(v.int_val); break;
            case AT_VAL_FLOAT:  o.float_val = v.float_val; o.int_val = static_cast<int64_t>(v.float_val); break;
            case AT_VAL_STRING: o.string_val = v.string_val ? v.string_val : ""; break;
            default: break;
        }
        return o;
    }

    // NOTE: the returned view references string_val; keep this object alive while it is used
    AT_ControlValue view() const {
        AT_ControlValue v{};
        v.type = type;
        switch (type) {
            case AT_VAL_BOOL:   v.bool_val = bool_val; break;
            case AT_VAL_INT:    v.int_val = int_val; break;
            case AT_VAL_FLOAT:  v.float_val = float_val; break;
            case AT_VAL_STRING: v.string_val = string_val.c_str(); break;
            default: v.int_val = 0; break;
        }
        return v;
    }

    static OwnedValue make_bool(bool b) { OwnedValue o; o.type = AT_VAL_BOOL; o.bool_val = b; return o; }
    static OwnedValue make_int(int64_t i) { OwnedValue o; o.type = AT_VAL_INT; o.int_val = i; o.float_val = static_cast<double>(i); return o; }
    static OwnedValue make_float(double d) { OwnedValue o; o.type = AT_VAL_FLOAT; o.float_val = d; o.int_val = static_cast<int64_t>(d); return o; }
    static OwnedValue make_string(const std::string& s) { OwnedValue o; o.type = AT_VAL_STRING; o.string_val = s; return o; }
};

struct ControlSnapshot {
    uint32_t                 control_id = 0;
    std::string              identifier;
    std::string              display_name;
    std::string              unit_label;
    AT_ControlType           control_type    = AT_CTRL_SLIDER;
    AT_PreferredStyle        preferred_style = AT_STYLE_DEFAULT;
    AT_ValueType             value_type      = AT_VAL_NONE;
    double                   min_value  = 0.0;
    double                   max_value  = 1.0;
    double                   step_value = 0.0;
    bool                     is_logarithmic = false;
    OwnedValue               default_value;
    std::string              file_filters;
    uint32_t                 device_filter = 0;
    std::vector<std::string> predefined_values;
    OwnedValue               value; // current value
};

struct FunctionSnapshot {
    uint32_t          function_id = 0;
    std::string       identifier;
    std::string       display_name;
    std::string       description;
    AT_FunctionOpKind op_kind = AT_OP_ACTION_TRIGGER;
    uint32_t          target_control_id = 0;
    AT_ValueType      parameter_type = AT_VAL_NONE;
    std::string       parameter_name;
    std::string       parameter_unit;
    double            min_value = 0.0;
    double            max_value = 0.0;
    double            default_value = 0.0;
};

struct NodeUiState {
    float pos_x       = 0.0f;
    float pos_y       = 0.0f;
    bool  is_expanded = true;
};

struct NodeSnapshot {
    uint32_t                      node_id = 0;
    std::string                   plugin_id;
    std::string                   display_name;
    AT_NodePlaneType              plane_type     = AT_NODE_PLANE_AUDIO;
    AT_AudioNodeCategory          audio_category = AT_AUDIO_CAT_NONE;
    uint32_t                      flags          = 0;
    NodeUiState                   ui;
    uint32_t                      audio_input_count  = 0;
    uint32_t                      audio_output_count = 0;
    std::vector<ControlSnapshot>  controls;
    std::vector<FunctionSnapshot> functions;
};

struct AudioEdgeSnapshot {
    uint32_t edge_id     = 0;
    uint32_t src_node_id = 0;
    uint32_t dst_node_id = 0;
};

struct ControlEdgeSnapshot {
    uint32_t    edge_id            = 0;
    uint32_t    src_node_id        = 0;
    uint32_t    src_output_port    = 0;
    uint32_t    dst_node_id        = 0;
    uint32_t    target_function_id = 0;
    std::string target_function_identifier;
    std::string target_function_name;
    OwnedValue  bound_arg;
    double      seconds_since_last_fire = 1e9;
};

struct ProfileMetadata {
    std::string name = "Untitled";
    std::string file_path;
    float       canvas_pan_x = 0.0f;
    float       canvas_pan_y = 0.0f;
    float       canvas_zoom  = 1.0f;
    std::string active_plane_view = "AUDIO"; // "AUDIO" | "CONTROL"
};

struct ProfileSnapshot {
    uint32_t                         profile_id = 0;
    bool                             active     = true;
    ProfileMetadata                  meta;
    std::vector<NodeSnapshot>        nodes;
    std::vector<AudioEdgeSnapshot>   audio_edges;
    std::vector<ControlEdgeSnapshot> control_edges;
    std::map<std::string, std::string> app_routing; // process_name -> endpoint_id
};

struct PluginInfo {
    std::string          plugin_id;
    std::string          display_name;
    std::string          relative_folder_path;
    AT_NodePlaneType     plane_type     = AT_NODE_PLANE_AUDIO;
    AT_AudioNodeCategory audio_category = AT_AUDIO_CAT_NONE;
};

struct EngineStats {
    bool     running              = false;
    uint64_t frames_processed     = 0;
    float    dsp_load             = 0.0f; // fraction of tick period spent processing
    size_t   open_devices         = 0;
    size_t   profile_count        = 0;
};

} // namespace at

