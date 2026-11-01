#pragma once
// ============================================================================
// AudioTool (AT) — Canvas Rendering & Interaction Module
// ============================================================================

#include "engine/AudioEngine.h"
#include "gui/CanvasState.h"

#include <map>
#include <string>
#include <vector>

namespace at {

class CanvasRenderer {
public:
    CanvasRenderer();

    // Renders the full canvas for the given profile within the current ImGui region
    void render(ProfileGraph& profile, AudioEngine& engine, float reserved_bottom_height = 28.0f);

    // Forces a refresh of the cached hardware audio device list
    void invalidate_device_cache() { devices_initialized_ = false; }

private:
    NodeLayoutMetrics compute_node_layout(const NodeSnapshot& node, const ImVec2& origin) const;

    void render_grid(ImDrawList* draw_list, const ImVec2& origin, const ImVec2& size, bool is_audio_view);
    bool render_connections(
        ProfileGraph& profile,
        const ProfileSnapshot& snap,
        const std::map<uint32_t, NodeLayoutMetrics>& layouts,
        ImDrawList* draw_list,
        bool is_audio_view);
    bool render_nodes(
        ProfileGraph& profile,
        const ProfileSnapshot& snap,
        const std::map<uint32_t, NodeLayoutMetrics>& layouts,
        ImDrawList* draw_list,
        const ImVec2& origin,
        bool is_audio_view,
        AudioEngine& engine);
    void render_context_menu(ProfileGraph& profile, AudioEngine& engine);
    void render_function_picker_popup(ProfileGraph& profile);
    void render_function_binding_modal(ProfileGraph& profile);

    CanvasView               view_;
    WireDragState            wire_drag_;
    NodeDragState            node_drag_;
    FunctionPickerPopupState func_picker_;
    FunctionBindingModal     binding_modal_;

    // Context menu spawn position in world space
    ImVec2                   spawn_world_pos_{0.0f, 0.0f};

    // Cached audio device list (avoids hammering WASAPI COM on every GUI frame)
    bool                          devices_initialized_ = false;
    std::vector<AudioDeviceInfo>  cached_devices_;

    // Last connection error toast
    std::string              status_toast_;
    double                   status_toast_time_ = 0.0;

    // Tracking active profile to save/restore pan & zoom
    uint32_t                 last_profile_id_ = 0;
};

} // namespace at
