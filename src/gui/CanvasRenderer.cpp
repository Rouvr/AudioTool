#include "gui/CanvasRenderer.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace at {

namespace {

constexpr float kSnapRadius = 6.0f;
constexpr float kSnapHitRadius = 14.0f;
constexpr float kDeleteCrossRadius = 9.0f;

// Color schemes per specification (04_GUI_Specification.md)
// Audio Plane: Cyan / Cool Slate
constexpr ImU32 kColAudioBg         = IM_COL32(24, 26, 31, 255);
constexpr ImU32 kColAudioGrid       = IM_COL32(37, 40, 48, 255);
constexpr ImU32 kColAudioHeader     = IM_COL32(43, 108, 176, 255);
constexpr ImU32 kColAudioWire       = IM_COL32(0, 229, 255, 255);
constexpr ImU32 kColAudioDot        = IM_COL32(0, 229, 255, 255);

// Control / Data Plane: Amber / Warm Indigo
constexpr ImU32 kColControlBg       = IM_COL32(20, 29, 36, 255);
constexpr ImU32 kColControlGrid     = IM_COL32(31, 46, 56, 255);
constexpr ImU32 kColControlHeader   = IM_COL32(217, 119, 6, 255);
constexpr ImU32 kColControlWire     = IM_COL32(255, 179, 0, 255);
constexpr ImU32 kColControlDot      = IM_COL32(255, 179, 0, 255);

// Common colors
constexpr ImU32 kColNodeBg          = IM_COL32(32, 34, 40, 245);
constexpr ImU32 kColNodeBorder      = IM_COL32(60, 64, 75, 255);
constexpr ImU32 kColRowBorder       = IM_COL32(45, 48, 56, 255);
constexpr ImU32 kColTextMuted       = IM_COL32(160, 165, 180, 255);

void draw_arrow_head(ImDrawList* dl, const ImVec2& tip, const ImVec2& dir, float size, ImU32 color) {
    ImVec2 d = dir;
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-4f) d = ImVec2(1.0f, 0.0f);
    else { d.x /= len; d.y /= len; }

    ImVec2 normal(-d.y, d.x);
    ImVec2 p1 = ImVec2(tip.x - d.x * size + normal.x * (size * 0.55f),
                       tip.y - d.y * size + normal.y * (size * 0.55f));
    ImVec2 p2 = ImVec2(tip.x - d.x * size - normal.x * (size * 0.55f),
                       tip.y - d.y * size - normal.y * (size * 0.55f));

    dl->AddTriangleFilled(tip, p1, p2, color);
}

} // namespace

CanvasRenderer::CanvasRenderer() = default;

void CanvasRenderer::render(ProfileGraph& profile, AudioEngine& engine) {
    ProfileSnapshot snap = profile.snapshot();
    bool is_audio_view = (snap.meta.active_plane_view == "AUDIO");

    if (profile.id() != last_profile_id_) {
        view_.pan_x = snap.meta.canvas_pan_x;
        view_.pan_y = snap.meta.canvas_pan_y;
        view_.zoom  = (snap.meta.canvas_zoom > 0.1f) ? snap.meta.canvas_zoom : 1.0f;
        last_profile_id_ = profile.id();
    }

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.x < 50.0f) canvas_size.x = 50.0f;
    if (canvas_size.y < 50.0f) canvas_size.y = 50.0f;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Canvas background
    ImU32 bg_col = is_audio_view ? kColAudioBg : kColControlBg;
    draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), bg_col);

    // Canvas panning with middle mouse button or right click drag
    ImGui::InvisibleButton("CanvasHitBox", canvas_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool is_hovered = ImGui::IsItemHovered();
    const bool is_active  = ImGui::IsItemActive();

    if (is_active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f) ||
                     (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f) && !wire_drag_.active))) {
        view_.pan_x += io.MouseDelta.x;
        view_.pan_y += io.MouseDelta.y;
        profile.set_canvas_state(view_.pan_x, view_.pan_y, snap.meta.active_plane_view);
    }

    // Canvas zoom (mouse wheel)
    if (is_hovered && io.MouseWheel != 0.0f) {
        ImVec2 mouse_world_before = view_.screen_to_world(io.MousePos, canvas_pos);
        float zoom_factor = (io.MouseWheel > 0) ? 1.1f : 0.9f;
        view_.zoom = std::clamp(view_.zoom * zoom_factor, 0.4f, 2.0f);
        ImVec2 mouse_world_after = view_.screen_to_world(io.MousePos, canvas_pos);
        view_.pan_x += (mouse_world_after.x - mouse_world_before.x) * view_.zoom;
        view_.pan_y += (mouse_world_after.y - mouse_world_before.y) * view_.zoom;
        profile.set_canvas_state(view_.pan_x, view_.pan_y, snap.meta.active_plane_view);
    }

    // 1. Grid
    render_grid(draw_list, canvas_pos, canvas_size, is_audio_view);

    // 2. Connections
    render_connections(profile, snap, draw_list, canvas_pos, is_audio_view);

    // 3. Nodes
    render_nodes(profile, snap, draw_list, canvas_pos, is_audio_view, engine);

    // 4. Pending wire drag
    if (wire_drag_.active) {
        ImVec2 cur = io.MousePos;
        ImU32 wcol = (wire_drag_.source_kind == WireDragSource::AudioInput || wire_drag_.source_kind == WireDragSource::AudioOutput) ? kColAudioWire : kColControlWire;
        draw_list->AddLine(wire_drag_.start_pos, cur, wcol, 2.5f);
        draw_arrow_head(draw_list, cur, ImVec2(cur.x - wire_drag_.start_pos.x, cur.y - wire_drag_.start_pos.y), 10.0f, wcol);

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            wire_drag_.active = false;
        }
    }

    // 5. Right-click context menu (node spawner)
    if (is_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f)) {
        spawn_world_pos_ = view_.screen_to_world(io.MousePos, canvas_pos);
        ImGui::OpenPopup("CanvasContextMenu");
    }
    render_context_menu(profile, canvas_pos, engine);

    // 6. Function binding modal popup
    render_function_binding_modal(profile);
}

void CanvasRenderer::render_grid(ImDrawList* draw_list, const ImVec2& origin, const ImVec2& size, bool is_audio_view) {
    ImU32 grid_col = is_audio_view ? kColAudioGrid : kColControlGrid;
    float step = 32.0f * view_.zoom;
    if (step < 8.0f) step = 8.0f;

    float x_start = std::fmod(view_.pan_x, step);
    if (x_start < 0.0f) x_start += step;
    for (float x = x_start; x < size.x; x += step) {
        draw_list->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), grid_col, 1.0f);
    }

    float y_start = std::fmod(view_.pan_y, step);
    if (y_start < 0.0f) y_start += step;
    for (float y = y_start; y < size.y; y += step) {
        draw_list->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), grid_col, 1.0f);
    }
}

void CanvasRenderer::render_connections(
    ProfileGraph& profile,
    const ProfileSnapshot& snap,
    ImDrawList* draw_list,
    const ImVec2& origin,
    bool is_audio_view) {
    ImGuiIO& io = ImGui::GetIO();

    // Map node id -> NodeSnapshot for coordinate resolution
    std::map<uint32_t, const NodeSnapshot*> node_map;
    for (const auto& n : snap.nodes) {
        node_map[n.node_id] = &n;
    }

    auto get_node_snap_pos = [&](uint32_t nid, bool is_output) -> ImVec2 {
        auto it = node_map.find(nid);
        if (it == node_map.end()) return ImVec2(0, 0);
        const auto* n = it->second;

        float width = n->ui.is_expanded ? 280.0f : 160.0f;
        float height = n->ui.is_expanded ? (40.0f + n->controls.size() * 28.0f) : 36.0f;

        float world_x = is_output ? (n->ui.pos_x + width) : n->ui.pos_x;
        float world_y = n->ui.pos_y + height * 0.5f;
        return view_.world_to_screen(ImVec2(world_x, world_y), origin);
    };

    if (is_audio_view) {
        for (const auto& edge : snap.audio_edges) {
            ImVec2 p_out = get_node_snap_pos(edge.src_node_id, true);
            ImVec2 p_in  = get_node_snap_pos(edge.dst_node_id, false);
            if (p_out.x == 0 && p_out.y == 0) continue;

            // Draw wire line
            draw_list->AddLine(p_out, p_in, kColAudioWire, 2.5f);

            // Draw directional arrow pointing to target input
            draw_arrow_head(draw_list, p_in, ImVec2(p_in.x - p_out.x, p_in.y - p_out.y), 10.0f, kColAudioWire);

            // Midpoint delete cross
            ImVec2 mid = ImVec2((p_out.x + p_in.x) * 0.5f, (p_out.y + p_in.y) * 0.5f);
            float d_mouse = std::hypot(io.MousePos.x - mid.x, io.MousePos.y - mid.y);
            bool hover = (d_mouse <= kDeleteCrossRadius);

            ImU32 badge_bg = hover ? IM_COL32(220, 50, 50, 255) : IM_COL32(40, 44, 52, 220);
            draw_list->AddCircleFilled(mid, kDeleteCrossRadius, badge_bg);
            draw_list->AddCircle(mid, kDeleteCrossRadius, hover ? IM_COL32(255, 120, 120, 255) : kColAudioWire, 0, 1.5f);

            // Draw 'X' inside badge
            float s = 3.5f;
            draw_list->AddLine(ImVec2(mid.x - s, mid.y - s), ImVec2(mid.x + s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);
            draw_list->AddLine(ImVec2(mid.x + s, mid.y - s), ImVec2(mid.x - s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);

            if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                profile.disconnect_audio(edge.edge_id);
            }
        }
    } else {
        // Control Plane View
        for (const auto& edge : snap.control_edges) {
            ImVec2 p_out = get_node_snap_pos(edge.src_node_id, true);
            ImVec2 p_in  = get_node_snap_pos(edge.dst_node_id, false);
            if (p_out.x == 0 && p_out.y == 0) continue;

            // Flash line on recent pulse (< 0.2s)
            bool pulse_active = (edge.seconds_since_last_fire < 0.2);
            ImU32 wire_col = pulse_active ? IM_COL32(255, 255, 200, 255) : kColControlWire;
            float thickness = pulse_active ? 4.0f : 2.5f;

            draw_list->AddLine(p_out, p_in, wire_col, thickness);
            draw_arrow_head(draw_list, p_in, ImVec2(p_in.x - p_out.x, p_in.y - p_out.y), 10.0f, wire_col);

            // Midpoint delete cross & function label badge
            ImVec2 mid = ImVec2((p_out.x + p_in.x) * 0.5f, (p_out.y + p_in.y) * 0.5f);
            float d_mouse = std::hypot(io.MousePos.x - mid.x, io.MousePos.y - mid.y);
            bool hover = (d_mouse <= kDeleteCrossRadius);

            ImU32 badge_bg = hover ? IM_COL32(220, 50, 50, 255) : IM_COL32(40, 44, 52, 220);
            draw_list->AddCircleFilled(mid, kDeleteCrossRadius, badge_bg);
            draw_list->AddCircle(mid, kDeleteCrossRadius, hover ? IM_COL32(255, 120, 120, 255) : kColControlWire, 0, 1.5f);

            float s = 3.5f;
            draw_list->AddLine(ImVec2(mid.x - s, mid.y - s), ImVec2(mid.x + s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);
            draw_list->AddLine(ImVec2(mid.x + s, mid.y - s), ImVec2(mid.x - s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);

            if (hover) {
                ImGui::SetTooltip("Bound: %s", edge.target_function_name.empty() ? edge.target_function_identifier.c_str() : edge.target_function_name.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    profile.disconnect_control(edge.edge_id);
                }
            }
        }
    }
}

void CanvasRenderer::render_nodes(
    ProfileGraph& profile,
    const ProfileSnapshot& snap,
    ImDrawList* draw_list,
    const ImVec2& origin,
    bool is_audio_view,
    AudioEngine& engine) {
    ImGuiIO& io = ImGui::GetIO();

    // Query available audio devices once per frame for device selector combo boxes
    std::vector<AudioDeviceInfo> dev_list = engine.device_manager().enumerate_devices();

    for (const auto& node : snap.nodes) {
        // If in Audio View, Pure Control Nodes are drawn with lowered opacity
        bool is_control_node = (node.plane_type == AT_NODE_PLANE_CONTROL);
        if (is_audio_view && is_control_node) {
            // Draw dimmed in Audio view
        }

        float width = node.ui.is_expanded ? 280.0f : 170.0f;
        float header_height = 32.0f;
        float row_height = 28.0f;
        float total_height = node.ui.is_expanded ? (header_height + node.controls.size() * row_height + 4.0f) : 36.0f;

        ImVec2 screen_min = view_.world_to_screen(ImVec2(node.ui.pos_x, node.ui.pos_y), origin);
        ImVec2 screen_max = ImVec2(screen_min.x + width * view_.zoom, screen_min.y + total_height * view_.zoom);

        // 1. Node Background and Border
        draw_list->AddRectFilled(screen_min, screen_max, kColNodeBg, 6.0f);
        draw_list->AddRect(screen_min, screen_max, kColNodeBorder, 6.0f, 0, 1.5f);

        // 2. Header Bar
        ImVec2 header_max = ImVec2(screen_max.x, screen_min.y + header_height * view_.zoom);
        ImU32 header_col = (node.plane_type == AT_NODE_PLANE_AUDIO) ? kColAudioHeader : kColControlHeader;
        draw_list->AddRectFilled(screen_min, header_max, header_col, 6.0f, ImDrawFlags_RoundCornersTop);

        // Header Title
        draw_list->AddText(ImVec2(screen_min.x + 8.0f * view_.zoom, screen_min.y + 7.0f * view_.zoom),
                           IM_COL32(255, 255, 255, 255), node.display_name.c_str());

        // Header Action Buttons (Minimize/Maximize [_] / [+], Disconnect [X-wires], Delete [X])
        // We handle header dragging to move node
        ImRect header_rect(screen_min, header_max);
        bool header_hovered = header_rect.Contains(io.MousePos);
        if (header_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Started dragging node
        }
        if (header_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && !wire_drag_.active) {
            NodeUiState ui = node.ui;
            ui.pos_x += io.MouseDelta.x / view_.zoom;
            ui.pos_y += io.MouseDelta.y / view_.zoom;
            profile.set_node_ui(node.node_id, ui);
        }

        // Action Buttons inside header
        float btn_sz = 18.0f * view_.zoom;
        float btn_y = screen_min.y + 7.0f * view_.zoom;

        // Button 1: Delete [X]
        ImVec2 btn_del(screen_max.x - 22.0f * view_.zoom, btn_y);
        bool del_hover = (io.MousePos.x >= btn_del.x && io.MousePos.x <= btn_del.x + btn_sz &&
                          io.MousePos.y >= btn_del.y && io.MousePos.y <= btn_del.y + btn_sz);
        draw_list->AddRectFilled(btn_del, ImVec2(btn_del.x + btn_sz, btn_del.y + btn_sz), del_hover ? IM_COL32(200, 40, 40, 255) : IM_COL32(0, 0, 0, 60), 3.0f);
        draw_list->AddText(ImVec2(btn_del.x + 4.0f * view_.zoom, btn_del.y + 1.0f * view_.zoom), IM_COL32(255, 255, 255, 255), "x");
        if (del_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            profile.delete_node(node.node_id);
            continue;
        }

        // Button 2: Disconnect All [~]
        ImVec2 btn_disc(btn_del.x - 22.0f * view_.zoom, btn_y);
        bool disc_hover = (io.MousePos.x >= btn_disc.x && io.MousePos.x <= btn_disc.x + btn_sz &&
                           io.MousePos.y >= btn_disc.y && io.MousePos.y <= btn_disc.y + btn_sz);
        draw_list->AddRectFilled(btn_disc, ImVec2(btn_disc.x + btn_sz, btn_disc.y + btn_sz), disc_hover ? IM_COL32(180, 100, 20, 255) : IM_COL32(0, 0, 0, 60), 3.0f);
        draw_list->AddText(ImVec2(btn_disc.x + 3.0f * view_.zoom, btn_disc.y + 1.0f * view_.zoom), IM_COL32(255, 255, 255, 255), "#");
        if (disc_hover) {
            ImGui::SetTooltip("Disconnect all wires");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                profile.disconnect_all_node_wires(node.node_id, is_audio_view, !is_audio_view);
            }
        }

        // Button 3: Toggle Expand/Minimize [_] / [+]
        ImVec2 btn_exp(btn_disc.x - 22.0f * view_.zoom, btn_y);
        bool exp_hover = (io.MousePos.x >= btn_exp.x && io.MousePos.x <= btn_exp.x + btn_sz &&
                          io.MousePos.y >= btn_exp.y && io.MousePos.y <= btn_exp.y + btn_sz);
        draw_list->AddRectFilled(btn_exp, ImVec2(btn_exp.x + btn_sz, btn_exp.y + btn_sz), exp_hover ? IM_COL32(80, 90, 110, 255) : IM_COL32(0, 0, 0, 60), 3.0f);
        draw_list->AddText(ImVec2(btn_exp.x + 4.0f * view_.zoom, btn_exp.y + 1.0f * view_.zoom), IM_COL32(255, 255, 255, 255), node.ui.is_expanded ? "_" : "+");
        if (exp_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            NodeUiState ui = node.ui;
            ui.is_expanded = !ui.is_expanded;
            profile.set_node_ui(node.node_id, ui);
        }

        // 3. Snap Points (Dots) on Left/Right Vertical Centers
        ImVec2 in_snap(screen_min.x, (screen_min.y + screen_max.y) * 0.5f);
        ImVec2 out_snap(screen_max.x, (screen_min.y + screen_max.y) * 0.5f);

        bool show_in_dot = is_audio_view ? (node.audio_category != AT_AUDIO_CAT_SOURCE) : true;
        bool show_out_dot = is_audio_view ? (node.audio_category != AT_AUDIO_CAT_OUTPUT) : true;
        ImU32 dot_col = is_audio_view ? kColAudioDot : kColControlDot;

        // In-Snap interaction
        if (show_in_dot) {
            float d_in = std::hypot(io.MousePos.x - in_snap.x, io.MousePos.y - in_snap.y);
            bool hover_in = (d_in <= kSnapHitRadius);
            draw_list->AddCircleFilled(in_snap, kSnapRadius * (hover_in ? 1.4f : 1.0f), dot_col);
            draw_list->AddCircle(in_snap, kSnapRadius * (hover_in ? 1.4f : 1.0f), IM_COL32(255, 255, 255, 255), 0, 1.5f);

            if (hover_in && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                wire_drag_.active = true;
                wire_drag_.source_kind = is_audio_view ? WireDragSource::AudioInput : WireDragSource::ControlInput;
                wire_drag_.node_id = node.node_id;
                wire_drag_.port_index = 0;
                wire_drag_.start_pos = in_snap;
            } else if (hover_in && wire_drag_.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                // Connect wire
                if (is_audio_view && wire_drag_.source_kind == WireDragSource::AudioOutput && wire_drag_.node_id != node.node_id) {
                    profile.connect_audio(wire_drag_.node_id, node.node_id);
                } else if (!is_audio_view && wire_drag_.source_kind == WireDragSource::ControlOutput && wire_drag_.node_id != node.node_id) {
                    // Open function picker popup for target node!
                    ImGui::OpenPopup(("FuncPicker_" + std::to_string(node.node_id)).c_str());
                }
            }
        }

        // Out-Snap interaction
        if (show_out_dot) {
            float d_out = std::hypot(io.MousePos.x - out_snap.x, io.MousePos.y - out_snap.y);
            bool hover_out = (d_out <= kSnapHitRadius);
            draw_list->AddCircleFilled(out_snap, kSnapRadius * (hover_out ? 1.4f : 1.0f), dot_col);
            draw_list->AddCircle(out_snap, kSnapRadius * (hover_out ? 1.4f : 1.0f), IM_COL32(255, 255, 255, 255), 0, 1.5f);

            if (hover_out && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                wire_drag_.active = true;
                wire_drag_.source_kind = is_audio_view ? WireDragSource::AudioOutput : WireDragSource::ControlOutput;
                wire_drag_.node_id = node.node_id;
                wire_drag_.port_index = 0;
                wire_drag_.start_pos = out_snap;
            } else if (hover_out && wire_drag_.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (is_audio_view && wire_drag_.source_kind == WireDragSource::AudioInput && wire_drag_.node_id != node.node_id) {
                    profile.connect_audio(node.node_id, wire_drag_.node_id);
                } else if (!is_audio_view && wire_drag_.source_kind == WireDragSource::ControlInput && wire_drag_.node_id != node.node_id) {
                    ImGui::OpenPopup(("FuncPicker_" + std::to_string(wire_drag_.node_id)).c_str());
                }
            }
        }

        // Context Menu for function picker when connecting in Control View
        if (ImGui::BeginPopup(("FuncPicker_" + std::to_string(node.node_id)).c_str())) {
            ImGui::TextDisabled("Select target function on '%s':", node.display_name.c_str());
            ImGui::Separator();
            for (const auto& func : node.functions) {
                if (ImGui::MenuItem(func.display_name.c_str())) {
                    if (func.parameter_type == AT_VAL_NONE) {
                        profile.connect_control(wire_drag_.node_id, wire_drag_.port_index, node.node_id, func.function_id, OwnedValue{});
                    } else {
                        // Open parameterized modal
                        binding_modal_.open = true;
                        binding_modal_.src_node_id = wire_drag_.node_id;
                        binding_modal_.src_port_index = wire_drag_.port_index;
                        binding_modal_.dst_node_id = node.node_id;
                        binding_modal_.function = func;
                        binding_modal_.entered_number = func.default_value;
                        binding_modal_.entered_bool = false;
                    }
                }
            }
            ImGui::EndPopup();
        }

        // 4. Form 2: Controls Table Rows
        if (node.ui.is_expanded) {
            ImGui::SetCursorScreenPos(ImVec2(screen_min.x + 8.0f * view_.zoom, screen_min.y + (header_height + 4.0f) * view_.zoom));
            ImGui::PushID(static_cast<int>(node.node_id));

            float row_w = (width - 16.0f) * view_.zoom;

            for (const auto& ctrl : node.controls) {
                ImGui::PushID(static_cast<int>(ctrl.control_id));

                if (ctrl.control_type == AT_CTRL_LEVEL_METER) {
                    // Meter bar
                    float val_db = static_cast<float>(ctrl.value.float_val);
                    float norm = std::clamp((val_db + 60.0f) / 60.0f, 0.0f, 1.0f);
                    ImVec2 p_bar = ImGui::GetCursorScreenPos();
                    draw_list->AddRectFilled(p_bar, ImVec2(p_bar.x + row_w, p_bar.y + 14.0f), IM_COL32(20, 20, 25, 255), 2.0f);
                    if (norm > 0.01f) {
                        ImU32 bar_col = (norm > 0.85f) ? IM_COL32(230, 60, 60, 255) : ((norm > 0.65f) ? IM_COL32(230, 200, 40, 255) : IM_COL32(40, 200, 120, 255));
                        draw_list->AddRectFilled(p_bar, ImVec2(p_bar.x + row_w * norm, p_bar.y + 14.0f), bar_col, 2.0f);
                    }
                    ImGui::Dummy(ImVec2(row_w, 18.0f));
                } else if (ctrl.control_type == AT_CTRL_SLIDER) {
                    float fval = static_cast<float>(ctrl.value.float_val);
                    ImGui::SetNextItemWidth(row_w * 0.58f);
                    if (ImGui::SliderFloat("##s", &fval, static_cast<float>(ctrl.min_value), static_cast<float>(ctrl.max_value), "%.1f dB")) {
                        profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_float(fval));
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(ctrl.display_name.c_str());
                } else if (ctrl.control_type == AT_CTRL_BUTTON) {
                    bool bval = ctrl.value.bool_val;
                    if (bval) {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 80, 40, 255));
                    }
                    if (ImGui::Button((ctrl.display_name + (bval ? " [ON]" : " [OFF]")).c_str(), ImVec2(row_w, 22.0f))) {
                        profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_bool(!bval));
                    }
                    if (bval) {
                        ImGui::PopStyleColor();
                    }
                } else if (ctrl.control_type == AT_CTRL_AUDIO_DEVICE_SELECT) {
                    std::string cur_dev = ctrl.value.string_val;
                    std::string label = cur_dev;

                    // Match friendly name
                    for (const auto& d : dev_list) {
                        if (d.endpoint_id == cur_dev) {
                            label = d.friendly_name;
                            break;
                        }
                    }

                    ImGui::SetNextItemWidth(row_w);
                    if (ImGui::BeginCombo("##devcombo", label.c_str())) {
                        bool is_capture = (ctrl.device_filter & AT_DEV_FILTER_INPUT_CAPTURE) != 0;
                        for (const auto& d : dev_list) {
                            if (is_capture && !d.is_input_capture && !d.endpoint_id.starts_with("virtual://")) continue;
                            if (!is_capture && d.is_input_capture && !d.endpoint_id.starts_with("virtual://")) continue;

                            bool selected = (d.endpoint_id == cur_dev);
                            if (ImGui::Selectable(d.friendly_name.c_str(), selected)) {
                                profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_string(d.endpoint_id));
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else if (ctrl.control_type == AT_CTRL_RANGE_TEXT_INPUT) {
                    if (ctrl.value_type == AT_VAL_INT) {
                        int ival = static_cast<int>(ctrl.value.int_val);
                        ImGui::SetNextItemWidth(row_w * 0.55f);
                        if (ImGui::InputInt("##i", &ival)) {
                            profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_int(ival));
                        }
                    } else {
                        float fval = static_cast<float>(ctrl.value.float_val);
                        ImGui::SetNextItemWidth(row_w * 0.55f);
                        if (ImGui::InputFloat("##f", &fval, 0.5f, 1.0f, "%.1f")) {
                            profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_float(fval));
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(ctrl.display_name.c_str());
                }

                ImGui::PopID();
            }

            ImGui::PopID();
        }
    }
}

void CanvasRenderer::render_context_menu(ProfileGraph& profile, const ImVec2& /*origin*/, AudioEngine& engine) {
    if (ImGui::BeginPopup("CanvasContextMenu")) {
        ImGui::TextDisabled("Create New Node");
        ImGui::Separator();

        auto plugins = engine.list_plugins();

        // Categorize into folders (Audio/Sources, Audio/Outputs, Audio/Processing, Control/Triggers)
        std::map<std::string, std::vector<PluginInfo>> folders;
        for (const auto& p : plugins) {
            folders[p.relative_folder_path].push_back(p);
        }

        for (const auto& [folder, plist] : folders) {
            std::string label = folder.empty() ? "General" : folder;
            if (ImGui::BeginMenu(label.c_str())) {
                for (const auto& p : plist) {
                    if (ImGui::MenuItem(p.display_name.c_str())) {
                        std::string err;
                        uint32_t nid = profile.spawn_node(p.plugin_id, "", &err);
                        if (nid != 0) {
                            NodeUiState ui;
                            ui.pos_x = spawn_world_pos_.x;
                            ui.pos_y = spawn_world_pos_.y;
                            ui.is_expanded = true;
                            profile.set_node_ui(nid, ui);
                        }
                    }
                }
                ImGui::EndMenu();
            }
        }

        ImGui::EndPopup();
    }
}

void CanvasRenderer::render_function_binding_modal(ProfileGraph& profile) {
    if (binding_modal_.open) {
        ImGui::OpenPopup("Configure Control Function");
    }

    if (ImGui::BeginPopupModal("Configure Control Function", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Bind pulse trigger to: %s", binding_modal_.function.display_name.c_str());
        ImGui::Separator();

        if (binding_modal_.function.parameter_type == AT_VAL_FLOAT || binding_modal_.function.parameter_type == AT_VAL_INT) {
            ImGui::Text("%s (%s):", binding_modal_.function.parameter_name.c_str(), binding_modal_.function.parameter_unit.c_str());
            float fval = static_cast<float>(binding_modal_.entered_number);
            if (ImGui::DragFloat("##num", &fval, 0.5f, static_cast<float>(binding_modal_.function.min_value), static_cast<float>(binding_modal_.function.max_value), "%.1f")) {
                binding_modal_.entered_number = fval;
            }
        }

        if (ImGui::Button("Confirm Binding", ImVec2(140, 0))) {
            OwnedValue arg;
            if (binding_modal_.function.parameter_type == AT_VAL_FLOAT) {
                arg = OwnedValue::make_float(binding_modal_.entered_number);
            } else if (binding_modal_.function.parameter_type == AT_VAL_INT) {
                arg = OwnedValue::make_int(static_cast<int64_t>(binding_modal_.entered_number));
            }
            profile.connect_control(binding_modal_.src_node_id, binding_modal_.src_port_index,
                                    binding_modal_.dst_node_id, binding_modal_.function.function_id, arg);
            binding_modal_.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            binding_modal_.open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace at
