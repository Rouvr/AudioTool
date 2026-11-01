#pragma once
// ============================================================================
// AudioTool (AT) — Canvas & Interaction State
// ============================================================================

#include "engine/EngineTypes.h"
#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace at {

enum class WireDragSource {
    None,
    AudioOutput,   // started from audio right dot
    AudioInput,    // started from audio left dot
    ControlOutput, // started from control right dot
    ControlInput   // started from control left dot
};

struct WireDragState {
    bool           active      = false;
    bool           click_mode  = false; // true after click-release on first snap dot (click-then-click mode)
    WireDragSource source_kind = WireDragSource::None;
    uint32_t       node_id     = 0;
    uint32_t       port_index  = 0;
    ImVec2         start_pos{0.0f, 0.0f};
    ImVec2         press_screen_pos{0.0f, 0.0f};

    void reset() {
        active      = false;
        click_mode  = false;
        source_kind = WireDragSource::None;
        node_id     = 0;
        port_index  = 0;
    }
};

struct NodeDragState {
    bool     active  = false;
    uint32_t node_id = 0;
    ImVec2   world_offset{0.0f, 0.0f}; // node_pos - mouse_world at drag start

    void reset() {
        active  = false;
        node_id = 0;
    }
};

struct FunctionPickerPopupState {
    bool                          pending_open   = false;
    uint32_t                      src_node_id    = 0;
    uint32_t                      src_port_index = 0;
    uint32_t                      dst_node_id    = 0;
    std::string                   dst_node_name;
    std::vector<FunctionSnapshot> functions;
};

struct FunctionBindingModal {
    bool             pending_open   = false;
    uint32_t         src_node_id    = 0;
    uint32_t         src_port_index = 0;
    uint32_t         dst_node_id    = 0;
    FunctionSnapshot function;
    double           entered_number = 0.0;
    bool             entered_bool   = false;
    char             entered_string[256] = {};
};

struct NodeLayoutMetrics {
    float  width            = 280.0f;
    float  header_height    = 32.0f;
    float  row_height       = 30.0f;
    float  total_height     = 36.0f;
    float  label_col_width  = 115.0f;
    float  widget_col_width = 165.0f;
    ImVec2 screen_min{0.0f, 0.0f};
    ImVec2 screen_max{0.0f, 0.0f};
    ImVec2 in_snap_screen{0.0f, 0.0f};
    ImVec2 out_snap_screen{0.0f, 0.0f};
};

struct CanvasView {
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    float zoom  = 1.0f;

    ImVec2 screen_to_world(const ImVec2& scr, const ImVec2& canvas_origin) const {
        return ImVec2((scr.x - canvas_origin.x - pan_x) / zoom,
                      (scr.y - canvas_origin.y - pan_y) / zoom);
    }

    ImVec2 world_to_screen(const ImVec2& w, const ImVec2& canvas_origin) const {
        return ImVec2(canvas_origin.x + pan_x + w.x * zoom,
                      canvas_origin.y + pan_y + w.y * zoom);
    }
};

} // namespace at
