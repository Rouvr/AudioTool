#pragma once
// ============================================================================
// AudioTool (AT) — Cross-DLL Node & Plugin C ABI Specification (at_abi.h)
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

#if defined(_WIN32)
    #define AT_CDECL __cdecl
    #if defined(AT_BUILDING_PLUGIN_DLL)
        #define AT_PLUGIN_EXPORT __declspec(dllexport)
    #else
        #define AT_PLUGIN_EXPORT __declspec(dllimport)
    #endif
#else
    #define AT_CDECL
    #define AT_PLUGIN_EXPORT
#endif

#define AT_ABI_VERSION 0x00010000u

typedef int32_t AT_Result;
#define AT_OK                    0
#define AT_ERR_INVALID_ARG      -1
#define AT_ERR_OUT_OF_MEMORY    -2
#define AT_ERR_NOT_FOUND        -3
#define AT_ERR_ABI_MISMATCH     -4
#define AT_ERR_DEVICE_FAILURE   -5
#define AT_ERR_STATE_INVALID    -6

// ----------------------------------------------------------------------------
// Node Plane & Category Enumerations
// ----------------------------------------------------------------------------

typedef enum AT_NodePlaneType {
    AT_NODE_PLANE_AUDIO   = 0, // Has Audio I/O + Control I/O (IAudioNode)
    AT_NODE_PLANE_CONTROL = 1  // Has Control I/O ONLY (IControlNode)
} AT_NodePlaneType;

typedef enum AT_AudioNodeCategory {
    AT_AUDIO_CAT_NONE       = 0, // Used by Control Nodes
    AT_AUDIO_CAT_SOURCE     = 1, // 0 Audio Inputs, 1..N Audio Outputs
    AT_AUDIO_CAT_PROCESSING = 2, // 1..N Audio Inputs, 1..N Audio Outputs
    AT_AUDIO_CAT_OUTPUT     = 3  // 1..N Audio Inputs, 0 Audio Outputs
} AT_AudioNodeCategory;

typedef enum AT_NodeFlags {
    AT_NODE_FLAG_NONE              = 0,
    AT_NODE_FLAG_AUTO_SUM_INPUTS   = (1 << 0),
    AT_NODE_FLAG_DYNAMIC_CONTROLS  = (1 << 1),
    AT_NODE_FLAG_PRODUCES_TAIL     = (1 << 2)
} AT_NodeFlags;

// ----------------------------------------------------------------------------
// Control & Preferred UI Style Enumerations
// ----------------------------------------------------------------------------

typedef enum AT_ValueType {
    AT_VAL_NONE   = 0,
    AT_VAL_BOOL   = 1,
    AT_VAL_INT    = 2,
    AT_VAL_FLOAT  = 3,
    AT_VAL_STRING = 4
} AT_ValueType;

typedef enum AT_ControlType {
    AT_CTRL_SLIDER              = 0,
    AT_CTRL_BUTTON              = 1,
    AT_CTRL_FILE_SELECTOR       = 2,
    AT_CTRL_AUDIO_DEVICE_SELECT = 3,
    AT_CTRL_VALUE_TEXT_INPUT    = 4,
    AT_CTRL_RANGE_TEXT_INPUT    = 5,
    AT_CTRL_LEVEL_METER         = 6,
    AT_CTRL_STATUS_LABEL        = 7
} AT_ControlType;

typedef enum AT_PreferredStyle {
    AT_STYLE_DEFAULT            = 0,
    AT_STYLE_HORIZONTAL_SLIDER  = 1,
    AT_STYLE_VERTICAL_SLIDER    = 2,
    AT_STYLE_ROTARY_KNOB        = 3,
    AT_STYLE_SELECTOR_SWITCH    = 4,
    AT_STYLE_PUSH_BUTTON        = 5,
    AT_STYLE_TOGGLE_BUTTON      = 6,
    AT_STYLE_CHECKBOX           = 7,
    AT_STYLE_TRIGGER_PAD        = 8,
    AT_STYLE_DROPDOWN_COMBO     = 9,
    AT_STYLE_RADIO_GROUP        = 10,
    AT_STYLE_AUTOCOMPLETE_TEXT  = 11,
    AT_STYLE_NUMERIC_BOX        = 12,
    AT_STYLE_SPINBOX            = 13,
    AT_STYLE_DRAG_SCALAR        = 14
} AT_PreferredStyle;

typedef enum AT_AudioDeviceFilter {
    AT_DEV_FILTER_INPUT_CAPTURE  = (1 << 0),
    AT_DEV_FILTER_OUTPUT_RENDER  = (1 << 1),
    AT_DEV_FILTER_LOOPBACK       = (1 << 2)
} AT_AudioDeviceFilter;

typedef struct AT_ControlValue {
    AT_ValueType type;
    union {
        bool        bool_val;
        int64_t     int_val;
        double      float_val;
        const char* string_val; // UTF-8 null-terminated
    };
} AT_ControlValue;

typedef struct AT_ControlDescriptor {
    uint32_t          control_id;
    const char*       identifier;
    const char*       display_name;
    const char*       unit_label;
    AT_ControlType    control_type;
    AT_PreferredStyle preferred_style;
    AT_ValueType      value_type;
    double            min_value;
    double            max_value;
    double            step_value;
    bool              is_logarithmic;
    AT_ControlValue   default_value;
    const char*       file_filters;
    uint32_t          device_filter;
    const char* const* predefined_values;
    uint32_t          predefined_count;
} AT_ControlDescriptor;

// ----------------------------------------------------------------------------
// Function / Action Descriptors (Control Plane & GUI Event Binding)
// ----------------------------------------------------------------------------

typedef enum AT_FunctionOpKind {
    AT_OP_ACTION_TRIGGER = 0,
    AT_OP_SET_PARAMETER  = 1,
    AT_OP_ADD_PARAMETER  = 2
} AT_FunctionOpKind;

typedef struct AT_FunctionDescriptor {
    uint32_t          function_id;
    const char*       identifier;
    const char*       display_name;
    const char*       description;
    AT_FunctionOpKind op_kind;
    uint32_t          target_control_id;
    AT_ValueType      parameter_type;
    const char*       parameter_name;
    const char*       parameter_unit;
    double            min_value;
    double            max_value;
    double            default_value;
} AT_FunctionDescriptor;

// ----------------------------------------------------------------------------
// Audio Stream Buffers & Processing Context
// ----------------------------------------------------------------------------

typedef struct AT_AudioBusView {
    uint32_t      source_node_id;
    const char*   source_node_name;
    uint32_t      num_channels;
    uint32_t      num_frames;
    float* const* channel_ptrs; // Planar: channel_ptrs[ch][frame]
} AT_AudioBusView;

typedef struct AT_AudioProcessContext {
    uint32_t               sample_rate;
    uint32_t               num_frames;
    uint64_t               sample_timestamp;
    uint32_t               input_bus_count;
    const AT_AudioBusView* input_buses;
    AT_AudioBusView        output_bus;
} AT_AudioProcessContext;

// ----------------------------------------------------------------------------
// Host Services Table (Provided by Backend Engine to Plugins)
// ----------------------------------------------------------------------------

typedef struct AT_NodeInstance* AT_NodeHandle;
typedef struct AT_DeviceStreamHandle_t* AT_DeviceStreamHandle;

typedef void (AT_CDECL *AT_StateWriteCallback)(void* user_ctx, const char* utf8_data, size_t byte_len);

typedef struct AT_HostServices {
    uint32_t struct_size;
    void*    host_context;

    // Logging service
    void (AT_CDECL *log_message)(void* host_context, int32_t level, const char* plugin_id, const char* message);

    // Control Plane trigger output emission
    void (AT_CDECL *emit_control_output)(void* host_context, uint32_t runtime_node_id, uint32_t output_port_index);

    // Notify host that a dynamic node's controls list changed
    void (AT_CDECL *notify_controls_dirty)(void* host_context, uint32_t runtime_node_id);

    // Hardware Device Manager (Hidden Master Node) acquisition & I/O
    AT_DeviceStreamHandle (AT_CDECL *acquire_hardware_device)(
        void* host_context,
        const char* device_endpoint_id,
        bool is_input_capture,
        bool is_loopback);

    void (AT_CDECL *release_hardware_device)(
        void* host_context,
        AT_DeviceStreamHandle stream_handle);

    // Pulls captured planar stereo frames from Hidden Master Input Node
    AT_Result (AT_CDECL *read_hardware_input)(
        void* host_context,
        AT_DeviceStreamHandle stream_handle,
        float* const* out_planar_channels,
        uint32_t num_channels,
        uint32_t num_frames);

    // Accumulates (+=) planar stereo frames into Hidden Master Output Node
    AT_Result (AT_CDECL *write_hardware_output)(
        void* host_context,
        AT_DeviceStreamHandle stream_handle,
        const float* const* in_planar_channels,
        uint32_t num_channels,
        uint32_t num_frames);
} AT_HostServices;

// ----------------------------------------------------------------------------
// Node Instance VTable & Plugin Export Bundle
// ----------------------------------------------------------------------------

typedef struct AT_NodeDescriptor {
    uint32_t             struct_size;
    const char*          plugin_id;      // Unique ID, e.g. "at.builtin.audio.virtual_input"
    const char*          display_name;   // e.g. "Virtual Audio Input"
    const char*          category_path;  // e.g. "Audio/Sources"
    const char*          vendor_name;    // e.g. "Rouvr"
    const char*          version_str;    // e.g. "0.1.0"
    const char*          build_id_str;   // Build ID baked at compile time
    AT_NodePlaneType     plane_type;     // AT_NODE_PLANE_AUDIO or AT_NODE_PLANE_CONTROL
    AT_AudioNodeCategory audio_category; // SOURCE, PROCESSING, OUTPUT, or NONE
    uint32_t             flags;          // Bitmask of AT_NodeFlags
} AT_NodeDescriptor;

typedef struct AT_NodeVTable {
    uint32_t struct_size;

    // Lifecycle
    AT_NodeHandle (AT_CDECL *create_instance)(const AT_HostServices* host_services, uint32_t runtime_node_id);
    void          (AT_CDECL *destroy_instance)(AT_NodeHandle handle);

    // Metadata Reflection
    const AT_ControlDescriptor*  (AT_CDECL *get_controls)(AT_NodeHandle handle, uint32_t* out_count);
    const AT_FunctionDescriptor* (AT_CDECL *get_functions)(AT_NodeHandle handle, uint32_t* out_count);

    // Control Value Accessors
    AT_Result (AT_CDECL *get_control_value)(AT_NodeHandle handle, uint32_t control_id, AT_ControlValue* out_val);
    AT_Result (AT_CDECL *set_control_value)(AT_NodeHandle handle, uint32_t control_id, const AT_ControlValue* in_val);

    // Function / Action Invocation (from Control Plane or GUI)
    AT_Result (AT_CDECL *invoke_function)(AT_NodeHandle handle, uint32_t function_id, const AT_ControlValue* param_val);

    // Serialization
    AT_Result (AT_CDECL *save_custom_state)(AT_NodeHandle handle, AT_StateWriteCallback writer_cb, void* writer_ctx);
    AT_Result (AT_CDECL *load_custom_state)(AT_NodeHandle handle, const char* json_utf8, size_t byte_len);

    // Audio Plane Callbacks (NULL if AT_NODE_PLANE_CONTROL)
    AT_Result (AT_CDECL *prepare_audio)(AT_NodeHandle handle, uint32_t sample_rate, uint32_t max_buffer_size, uint32_t max_connections);
    void      (AT_CDECL *process_audio)(AT_NodeHandle handle, const AT_AudioProcessContext* ctx);
    void      (AT_CDECL *on_connections_changed)(AT_NodeHandle handle, uint32_t input_count, uint32_t output_count);

    // Control Plane Tick Callback (Used by Control Nodes like TimerNode)
    void      (AT_CDECL *tick_control)(AT_NodeHandle handle, uint64_t timestamp_ns);
} AT_NodeVTable;

typedef struct AT_PluginExportBundle {
    uint32_t                 struct_size;
    uint32_t                 abi_version;
    const AT_NodeDescriptor* descriptor;
    const AT_NodeVTable*     vtable;
} AT_PluginExportBundle;

typedef const AT_PluginExportBundle* (AT_CDECL *AT_GetPluginBundleFn)(uint32_t host_abi_version);

#define AT_PLUGIN_ENTRY_SYMBOL_NAME "at_get_plugin_bundle"

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
