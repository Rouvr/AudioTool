#pragma once
// ============================================================================
// AudioTool (AT) — Canvas Rendering & Interaction Module
// ============================================================================

#include "engine/AudioEngine.h"
#include "gui/CanvasState.h"
#include <string>

namespace at {

class CanvasRenderer {
public:
    CanvasRenderer();

    // Renders the full canvas for the given profile within the current ImGui window
    void render(ProfileGraph& profile, AudioEngine& engine);

private:
    void render_grid(ImDrawList* draw_list, const ImVec2& origin, const ImVec2& size, bool is_audio_view);
    void render_connections(ProfileGraph& profile, const ProfileSnapshot& snap, ImDrawList* draw_list, const ImVec2& origin, bool is_audio_view);
    void render_nodes(ProfileGraph& profile, const ProfileSnapshot& snap, ImDrawList* draw_list, const ImVec2& origin, bool is_audio_view, AudioEngine& engine);
    void render_context_menu(ProfileGraph& profile, const ImVec2& origin, AudioEngine& engine);
    void render_function_binding_modal(ProfileGraph& profile);

    CanvasView           view_;
    WireDragState        wire_drag_;
    FunctionBindingModal binding_modal_;

    // Context menu spawn position in world space
    ImVec2               spawn_world_pos_{0.0f, 0.0f};

    // Tracking active profile to save/restore pan & zoom
    uint32_t             last_profile_id_ = 0;
};

} // namespace at
