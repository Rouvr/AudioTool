#pragma once
// ============================================================================
// AudioTool (AT) — Canvas & Interaction State
// ============================================================================

#include "engine/EngineTypes.h"
#include "imgui.h"

#include <cstdint>
#include <string>

namespace at {

enum class WireDragSource {
    None,
    AudioOutput,  // dragged from audio right dot
    AudioInput,   // dragged from audio left dot
    ControlOutput,// dragged from control right dot
    ControlInput  // dragged from control left dot
};

struct WireDragState {
    bool           active = false;
    WireDragSource source_kind = WireDragSource::None;
    uint32_t       node_id = 0;
    uint32_t       port_index = 0;
    ImVec2         start_pos{0.0f, 0.0f};
};

struct FunctionBindingModal {
    bool             open = false;
    uint32_t         src_node_id = 0;
    uint32_t         src_port_index = 0;
    uint32_t         dst_node_id = 0;
    FunctionSnapshot function;
    double           entered_number = 0.0;
    bool             entered_bool = false;
    char             entered_string[256] = {};
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
