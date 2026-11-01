#include "gui/CanvasRenderer.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

namespace at {

namespace {

constexpr float kSnapRadius        = 6.5f;
constexpr float kSnapHitRadius     = 13.0f;
constexpr float kDeleteCrossRadius = 9.0f;
constexpr float kPi                = 3.14159265358979323846f;

// Color schemes per specification (04_GUI_Specification.md)
// Audio Plane: Cyan / Cool Slate
constexpr ImU32 kColAudioBg         = IM_COL32(24, 26, 31, 255);
constexpr ImU32 kColAudioGridMinor  = IM_COL32(34, 37, 45, 255);
constexpr ImU32 kColAudioGridMajor  = IM_COL32(46, 50, 62, 255);
constexpr ImU32 kColAudioHeader     = IM_COL32(43, 108, 176, 255);
constexpr ImU32 kColAudioWire       = IM_COL32(0, 229, 255, 255);
constexpr ImU32 kColAudioDot        = IM_COL32(0, 229, 255, 255);

// Control / Data Plane: Amber / Warm Indigo
constexpr ImU32 kColControlBg        = IM_COL32(20, 29, 36, 255);
constexpr ImU32 kColControlGridMinor = IM_COL32(29, 42, 52, 255);
constexpr ImU32 kColControlGridMajor = IM_COL32(40, 58, 72, 255);
constexpr ImU32 kColControlHeader    = IM_COL32(217, 119, 6, 255);
constexpr ImU32 kColControlWire      = IM_COL32(255, 179, 0, 255);
constexpr ImU32 kColControlDot       = IM_COL32(255, 179, 0, 255);

// Common node table colors
constexpr ImU32 kColNodeBg          = IM_COL32(32, 35, 42, 248);
constexpr ImU32 kColNodeRowEven     = IM_COL32(36, 39, 47, 248);
constexpr ImU32 kColNodeRowOdd      = IM_COL32(30, 33, 40, 248);
constexpr ImU32 kColNodeBorder      = IM_COL32(68, 73, 86, 255);
constexpr ImU32 kColRowBorder       = IM_COL32(48, 52, 62, 255);
constexpr ImU32 kColTextPrimary     = IM_COL32(235, 238, 245, 255);
constexpr ImU32 kColTextMuted       = IM_COL32(165, 172, 188, 255);

void draw_arrow_head(ImDrawList* dl, const ImVec2& tip, const ImVec2& dir, float size, ImU32 color) {
    ImVec2 d = dir;
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-4f) d = ImVec2(1.0f, 0.0f);
    else { d.x /= len; d.y /= len; }

    ImVec2 normal(-d.y, d.x);
    ImVec2 p1(tip.x - d.x * size + normal.x * (size * 0.55f),
              tip.y - d.y * size + normal.y * (size * 0.55f));
    ImVec2 p2(tip.x - d.x * size - normal.x * (size * 0.55f),
              tip.y - d.y * size - normal.y * (size * 0.55f));

    dl->AddTriangleFilled(tip, p1, p2, color);
}

// Compute zoom-independent text width (at zoom = 1.0)
float calc_unscaled_text_width(const std::string& text, float current_zoom) {
    if (text.empty()) return 0.0f;
    float z = std::max(0.1f, current_zoom);
    return ImGui::CalcTextSize(text.c_str()).x / z;
}

// Custom interactive rotary knob control for AT_STYLE_ROTARY_KNOB
bool draw_rotary_knob(
    const char* id_str,
    float* p_value,
    float v_min,
    float v_max,
    const char* unit_label,
    float radius,
    float total_width) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float diameter = radius * 2.0f;
    ImGui::InvisibleButton(id_str, ImVec2(total_width, diameter));
    bool is_active = ImGui::IsItemActive();
    bool is_hovered = ImGui::IsItemHovered();
    bool value_changed = false;

    if (is_active && (io.MouseDelta.y != 0.0f || io.MouseDelta.x != 0.0f)) {
        float range = std::max(1e-4f, v_max - v_min);
        float delta = (-io.MouseDelta.y + io.MouseDelta.x) * (range / 160.0f);
        float new_val = std::clamp(*p_value + delta, v_min, v_max);
        if (std::abs(new_val - *p_value) > 1e-5f) {
            *p_value = new_val;
            value_changed = true;
        }
    }

    float t = (v_max > v_min) ? std::clamp((*p_value - v_min) / (v_max - v_min), 0.0f, 1.0f) : 0.0f;
    float angle_min = kPi * 0.75f;
    float angle_max = kPi * 2.25f;
    float angle = angle_min + (angle_max - angle_min) * t;

    ImVec2 center(pos.x + radius + 4.0f, pos.y + radius);
    dl->AddCircleFilled(center, radius, is_active ? IM_COL32(60, 70, 90, 255) : (is_hovered ? IM_COL32(50, 56, 70, 255) : IM_COL32(38, 42, 52, 255)));
    dl->AddCircle(center, radius, is_active ? kColAudioWire : IM_COL32(95, 105, 125, 255), 24, 1.5f);

    // Value pointer line
    ImVec2 needle(center.x + std::cos(angle) * (radius - 2.0f),
                  center.y + std::sin(angle) * (radius - 2.0f));
    dl->AddLine(center, needle, kColAudioWire, 2.0f);

    // Value text next to knob
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", *p_value, unit_label ? unit_label : "");
    ImVec2 text_sz = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(center.x + radius + 8.0f, center.y - text_sz.y * 0.5f), kColTextPrimary, buf);

    return value_changed;
}

// Recursive folder tree node for right-click filesystem-mirrored spawner menu
struct FolderMenuTree {
    std::map<std::string, FolderMenuTree> subfolders;
    std::vector<PluginInfo>               plugins;
};

void build_folder_tree(FolderMenuTree& root, const std::vector<PluginInfo>& plugins) {
    for (const auto& p : plugins) {
        FolderMenuTree* cur = &root;
        std::filesystem::path rel(p.relative_folder_path);
        for (const auto& part : rel) {
            std::string folder_name = part.string();
            if (!folder_name.empty() && folder_name != ".") {
                cur = &cur->subfolders[folder_name];
            }
        }
        cur->plugins.push_back(p);
    }
}

void render_folder_tree_node(
    const FolderMenuTree& tree,
    ProfileGraph& profile,
    const ImVec2& spawn_world_pos,
    bool is_audio_view,
    std::string* out_toast) {
    for (const auto& [folder_name, subtree] : tree.subfolders) {
        if (ImGui::BeginMenu(folder_name.c_str())) {
            render_folder_tree_node(subtree, profile, spawn_world_pos, is_audio_view, out_toast);
            ImGui::EndMenu();
        }
    }
    for (const auto& p : tree.plugins) {
        if (ImGui::MenuItem(p.display_name.c_str())) {
            std::string err;
            uint32_t nid = profile.spawn_node(p.plugin_id, "", &err);
            if (nid != 0) {
                NodeUiState ui;
                ui.pos_x = spawn_world_pos.x;
                ui.pos_y = spawn_world_pos.y;
                ui.is_expanded = true;
                profile.set_node_ui(nid, ui);
                if (is_audio_view && p.plane_type == AT_NODE_PLANE_CONTROL && out_toast) {
                    *out_toast = "Spawned Control Node '" + p.display_name + "' (Switch to Control/Data View to wire triggers)";
                }
            } else if (out_toast && !err.empty()) {
                *out_toast = err;
            }
        }
    }
}

} // namespace

CanvasRenderer::CanvasRenderer() = default;

NodeLayoutMetrics CanvasRenderer::compute_node_layout(const NodeSnapshot& node, const ImVec2& origin) const {
    NodeLayoutMetrics m;
    float title_w = calc_unscaled_text_width(node.display_name, view_.zoom);

    if (!node.ui.is_expanded) {
        // Form 1: Minimized compact strip
        m.header_height = 36.0f;
        m.total_height  = 36.0f;
        m.width         = std::max(185.0f, title_w + 96.0f);
    } else {
        // Form 2: Expanded table-row layout ("Keep the width of the widest element")
        m.header_height = 32.0f;
        m.row_height    = 30.0f;

        float max_label_w = 95.0f;
        float min_widget_w = 180.0f;

        for (const auto& ctrl : node.controls) {
            float lw = calc_unscaled_text_width(ctrl.display_name, view_.zoom) + 20.0f;
            max_label_w = std::max(max_label_w, lw);
            if (ctrl.control_type == AT_CTRL_AUDIO_DEVICE_SELECT) {
                min_widget_w = std::max(min_widget_w, 210.0f);
            }
        }

        m.label_col_width  = std::clamp(max_label_w, 95.0f, 190.0f);
        m.width            = std::max({295.0f, title_w + 98.0f, m.label_col_width + min_widget_w + 24.0f});
        m.widget_col_width = m.width - m.label_col_width - 24.0f;
        m.total_height     = m.header_height + static_cast<float>(node.controls.size()) * m.row_height + 6.0f;
    }

    m.screen_min      = view_.world_to_screen(ImVec2(node.ui.pos_x, node.ui.pos_y), origin);
    m.screen_max      = ImVec2(m.screen_min.x + m.width * view_.zoom, m.screen_min.y + m.total_height * view_.zoom);
    m.in_snap_screen  = ImVec2(m.screen_min.x, (m.screen_min.y + m.screen_max.y) * 0.5f);
    m.out_snap_screen = ImVec2(m.screen_max.x, (m.screen_min.y + m.screen_max.y) * 0.5f);
    return m;
}

void CanvasRenderer::render(ProfileGraph& profile, AudioEngine& engine, float reserved_bottom_height) {
    if (!devices_initialized_) {
        cached_devices_ = engine.device_manager().enumerate_devices();
        devices_initialized_ = true;
    }

    ProfileSnapshot snap = profile.snapshot();
    bool is_audio_view = (snap.meta.active_plane_view == "AUDIO");

    if (profile.id() != last_profile_id_) {
        view_.pan_x = snap.meta.canvas_pan_x;
        view_.pan_y = snap.meta.canvas_pan_y;
        view_.zoom  = (snap.meta.canvas_zoom > 0.1f) ? snap.meta.canvas_zoom : 1.0f;
        wire_drag_.reset();
        node_drag_.reset();
        last_profile_id_ = profile.id();
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 canvas_size( std::max(60.0f, avail.x), std::max(60.0f, avail.y - reserved_bottom_height));

    // Isolate the canvas inside a dedicated child window for hardware scissor clipping
    // and clean widget focus management.
    ImGuiWindowFlags canvas_flags = ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoScrollWithMouse |
                                    ImGuiWindowFlags_NoMove;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("CanvasViewport", canvas_size, ImGuiChildFlags_None, canvas_flags);
    ImGui::PopStyleVar();

    ImVec2 canvas_pos = ImGui::GetWindowPos();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Scale font inside canvas according to current zoom
    ImGui::SetWindowFontScale(view_.zoom);

    // 1. Canvas Background & Grid
    ImU32 bg_col = is_audio_view ? kColAudioBg : kColControlBg;
    draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), bg_col);
    render_grid(draw_list, canvas_pos, canvas_size, is_audio_view);

    // Precompute unified node layouts for both wire rendering and node rendering
    std::map<uint32_t, NodeLayoutMetrics> layouts;
    for (const auto& node : snap.nodes) {
        layouts[node.node_id] = compute_node_layout(node, canvas_pos);
    }

    // 2. Render Existing Connections (Audio or Control Plane)
    bool wire_interacted = render_connections(profile, snap, layouts, draw_list, is_audio_view);

    // 3. Render All Nodes & Interactive Table Row Controls
    bool node_interacted = render_nodes(profile, snap, layouts, draw_list, canvas_pos, is_audio_view, engine);

    // 4. Render Active Wire Drag / Click-then-Click Preview Line
    if (wire_drag_.active) {
        // Keep wire start anchored to the source snap point as canvas pans/zooms
        auto src_it = layouts.find(wire_drag_.node_id);
        if (src_it != layouts.end()) {
            bool from_out = (wire_drag_.source_kind == WireDragSource::AudioOutput ||
                             wire_drag_.source_kind == WireDragSource::ControlOutput);
            wire_drag_.start_pos = from_out ? src_it->second.out_snap_screen : src_it->second.in_snap_screen;
        }

        ImVec2 cur = io.MousePos;
        bool is_audio_wire = (wire_drag_.source_kind == WireDragSource::AudioInput ||
                              wire_drag_.source_kind == WireDragSource::AudioOutput);
        ImU32 wcol = is_audio_wire ? kColAudioWire : kColControlWire;

        // Signal direction is always Output -> Input
        bool from_output = (wire_drag_.source_kind == WireDragSource::AudioOutput ||
                            wire_drag_.source_kind == WireDragSource::ControlOutput);
        ImVec2 p_from = from_output ? wire_drag_.start_pos : cur;
        ImVec2 p_to   = from_output ? cur : wire_drag_.start_pos;

        draw_list->AddLine(p_from, p_to, wcol, 2.5f);
        draw_arrow_head(draw_list, p_to, ImVec2(p_to.x - p_from.x, p_to.y - p_from.y), 10.0f, wcol);

        // Handle drag-release vs click-then-click transitions
        if (!wire_drag_.click_mode) {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                float drag_dist = std::hypot(io.MousePos.x - wire_drag_.press_screen_pos.x,
                                             io.MousePos.y - wire_drag_.press_screen_pos.y);
                if (drag_dist < 6.0f) {
                    // User clicked and released on the first snap dot -> enter Click-then-Click mode!
                    wire_drag_.click_mode = true;
                } else {
                    // User dragged and released on empty space -> cancel wire
                    wire_drag_.reset();
                }
            }
        } else {
            // In Click-then-Click mode: clicking empty space, right-clicking, or pressing Escape cancels
            if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !node_interacted) ||
                ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                wire_drag_.reset();
            }
        }
    }

    // 5. Canvas Background Pan, Zoom, and Right-Click Context Menu
    const bool canvas_window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool any_widget_busy = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() ||
                                 node_interacted || wire_interacted || node_drag_.active || wire_drag_.active;

    // Panning: Middle-mouse drag anywhere, Right-mouse drag, or Left-mouse drag on empty background
    bool pan_With_middle = canvas_window_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f);
    bool pan_with_right  = canvas_window_hovered && !wire_drag_.active && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f);
    bool pan_with_left   = canvas_window_hovered && !any_widget_busy && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);

    if (pan_With_middle || pan_with_right || pan_with_left) {
        view_.pan_x += io.MouseDelta.x;
        view_.pan_y += io.MouseDelta.y;
        profile.set_canvas_state(view_.pan_x, view_.pan_y, snap.meta.active_plane_view, view_.zoom);
    }

    // Zooming: Mouse wheel centered around cursor position (when not scrolling an open combo popup)
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) {
        ImVec2 mouse_world_before = view_.screen_to_world(io.MousePos, canvas_pos);
        float zoom_factor = (io.MouseWheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
        view_.zoom = std::clamp(view_.zoom * zoom_factor, 0.45f, 2.0f);
        ImVec2 mouse_world_after = view_.screen_to_world(io.MousePos, canvas_pos);
        view_.pan_x += (mouse_world_after.x - mouse_world_before.x) * view_.zoom;
        view_.pan_y += (mouse_world_after.y - mouse_world_before.y) * view_.zoom;
        profile.set_canvas_state(view_.pan_x, view_.pan_y, snap.meta.active_plane_view, view_.zoom);
    }

    // Right-click on empty canvas -> open filesystem-mirrored node spawner menu
    if (ImGui::IsWindowHovered() && !any_widget_busy &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) {
        spawn_world_pos_ = view_.screen_to_world(io.MousePos, canvas_pos);
        ImGui::OpenPopup("CanvasContextMenu");
    }

    // Restore font scale before rendering popups/modals so popup menus stay at 1.0x Crisp UI scale
    ImGui::SetWindowFontScale(1.0f);

    render_context_menu(profile, engine);
    render_function_picker_popup(profile);
    render_function_binding_modal(profile);

    // Optional status toast overlay at top-center of canvas
    if (!status_toast_.empty()) {
        double now_sec = ImGui::GetTime();
        if (now_sec - status_toast_time_ < 4.0) {
            ImVec2 t_sz = ImGui::CalcTextSize(status_toast_.c_str());
            ImVec2 t_min(canvas_pos.x + (canvas_size.x - t_sz.x) * 0.5f - 12.0f, canvas_pos.y + 12.0f);
            ImVec2 t_max(t_min.x + t_sz.x + 24.0f, t_min.y + t_sz.y + 12.0f);
            draw_list->AddRectFilled(t_min, t_max, IM_COL32(25, 28, 36, 235), 5.0f);
            draw_list->AddRect(t_min, t_max, kColControlWire, 5.0f, 0, 1.5f);
            draw_list->AddText(ImVec2(t_min.x + 12.0f, t_min.y + 6.0f), kColTextPrimary, status_toast_.c_str());
        } else {
            status_toast_.clear();
        }
    }

    ImGui::EndChild();
}

void CanvasRenderer::render_grid(ImDrawList* draw_list, const ImVec2& origin, const ImVec2& size, bool is_audio_view) {
    ImU32 minor_col = is_audio_view ? kColAudioGridMinor : kColControlGridMinor;
    ImU32 major_col = is_audio_view ? kColAudioGridMajor : kColControlGridMajor;

    float minor_step = 32.0f * view_.zoom;
    if (minor_step < 10.0f) minor_step = 10.0f;
    float major_step = minor_step * 4.0f;

    float x_start = std::fmod(view_.pan_x, minor_step);
    if (x_start < 0.0f) x_start += minor_step;
    for (float x = x_start; x < size.x; x += minor_step) {
        draw_list->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), minor_col, 1.0f);
    }

    float y_start = std::fmod(view_.pan_y, minor_step);
    if (y_start < 0.0f) y_start += minor_step;
    for (float y = y_start; y < size.y; y += minor_step) {
        draw_list->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), minor_col, 1.0f);
    }

    float mx_start = std::fmod(view_.pan_x, major_step);
    if (mx_start < 0.0f) mx_start += major_step;
    for (float x = mx_start; x < size.x; x += major_step) {
        draw_list->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), major_col, 1.2f);
    }

    float my_start = std::fmod(view_.pan_y, major_step);
    if (my_start < 0.0f) my_start += major_step;
    for (float y = my_start; y < size.y; y += major_step) {
        draw_list->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), major_col, 1.2f);
    }
}

bool CanvasRenderer::render_connections(
    ProfileGraph& profile,
    const ProfileSnapshot& snap,
    const std::map<uint32_t, NodeLayoutMetrics>& layouts,
    ImDrawList* draw_list,
    bool is_audio_view) {
    ImGuiIO& io = ImGui::GetIO();
    bool interacted = false;

    if (is_audio_view) {
        for (const auto& edge : snap.audio_edges) {
            auto src_it = layouts.find(edge.src_node_id);
            auto dst_it = layouts.find(edge.dst_node_id);
            if (src_it == layouts.end() || dst_it == layouts.end()) continue;

            ImVec2 p_out = src_it->second.out_snap_screen;
            ImVec2 p_in  = dst_it->second.in_snap_screen;

            // Draw wire line & directional arrow pointing into target input
            draw_list->AddLine(p_out, p_in, kColAudioWire, 2.5f);
            draw_arrow_head(draw_list, p_in, ImVec2(p_in.x - p_out.x, p_in.y - p_out.y), 11.0f, kColAudioWire);

            // Midpoint delete cross [×]
            ImVec2 mid((p_out.x + p_in.x) * 0.5f, (p_out.y + p_in.y) * 0.5f);
            float d_mouse = std::hypot(io.MousePos.x - mid.x, io.MousePos.y - mid.y);
            bool hover = (d_mouse <= kDeleteCrossRadius);

            ImU32 badge_bg = hover ? IM_COL32(220, 50, 50, 255) : IM_COL32(36, 40, 48, 235);
            draw_list->AddCircleFilled(mid, kDeleteCrossRadius, badge_bg);
            draw_list->AddCircle(mid, kDeleteCrossRadius, hover ? IM_COL32(255, 130, 130, 255) : kColAudioWire, 0, 1.5f);

            float s = 3.5f;
            draw_list->AddLine(ImVec2(mid.x - s, mid.y - s), ImVec2(mid.x + s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);
            draw_list->AddLine(ImVec2(mid.x + s, mid.y - s), ImVec2(mid.x - s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);

            if (hover) {
                interacted = true;
                ImGui::SetTooltip("Click to disconnect audio wire");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    profile.disconnect_audio(edge.edge_id);
                }
            }
        }
    } else {
        for (const auto& edge : snap.control_edges) {
            auto src_it = layouts.find(edge.src_node_id);
            auto dst_it = layouts.find(edge.dst_node_id);
            if (src_it == layouts.end() || dst_it == layouts.end()) continue;

            ImVec2 p_out = src_it->second.out_snap_screen;
            ImVec2 p_in  = dst_it->second.in_snap_screen;

            // Flash line on recent pulse (< 0.20s)
            bool pulse_active = (edge.seconds_since_last_fire < 0.20);
            ImU32 wire_col = pulse_active ? IM_COL32(255, 255, 210, 255) : kColControlWire;
            float thickness = pulse_active ? 4.0f : 2.5f;

            draw_list->AddLine(p_out, p_in, wire_col, thickness);
            draw_arrow_head(draw_list, p_in, ImVec2(p_in.x - p_out.x, p_in.y - p_out.y), 11.0f, wire_col);

            // Midpoint delete cross & function label badge
            ImVec2 mid((p_out.x + p_in.x) * 0.5f, (p_out.y + p_in.y) * 0.5f);
            float d_mouse = std::hypot(io.MousePos.x - mid.x, io.MousePos.y - mid.y);
            bool hover = (d_mouse <= kDeleteCrossRadius);

            ImU32 badge_bg = hover ? IM_COL32(220, 50, 50, 255) : IM_COL32(36, 40, 48, 235);
            draw_list->AddCircleFilled(mid, kDeleteCrossRadius, badge_bg);
            draw_list->AddCircle(mid, kDeleteCrossRadius, hover ? IM_COL32(255, 130, 130, 255) : kColControlWire, 0, 1.5f);

            float s = 3.5f;
            draw_list->AddLine(ImVec2(mid.x - s, mid.y - s), ImVec2(mid.x + s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);
            draw_list->AddLine(ImVec2(mid.x + s, mid.y - s), ImVec2(mid.x - s, mid.y + s), IM_COL32(255, 255, 255, 255), 1.5f);

            if (hover) {
                interacted = true;
                const char* fn_label = edge.target_function_name.empty() ? edge.target_function_identifier.c_str() : edge.target_function_name.c_str();
                if (edge.bound_arg.type == AT_VAL_FLOAT) {
                    ImGui::SetTooltip("Bound Function: %s (%.1f) — Click to delete", fn_label, edge.bound_arg.float_val);
                } else if (edge.bound_arg.type == AT_VAL_INT) {
                    ImGui::SetTooltip("Bound Function: %s (%lld) — Click to delete", fn_label, static_cast<long long>(edge.bound_arg.int_val));
                } else {
                    ImGui::SetTooltip("Bound Function: %s — Click to delete", fn_label);
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    profile.disconnect_control(edge.edge_id);
                }
            }
        }
    }
    return interacted;
}

bool CanvasRenderer::render_nodes(
    ProfileGraph& profile,
    const ProfileSnapshot& snap,
    const std::map<uint32_t, NodeLayoutMetrics>& layouts,
    ImDrawList* draw_list,
    const ImVec2& origin,
    bool is_audio_view,
    AudioEngine& engine) {
    ImGuiIO& io = ImGui::GetIO();
    bool any_node_interacted = false;

    // Handle active node dragging across frames (even if cursor moves rapidly)
    if (node_drag_.active) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            for (const auto& node : snap.nodes) {
                if (node.node_id == node_drag_.node_id) {
                    ImVec2 mouse_world = view_.screen_to_world(io.MousePos, origin);
                    NodeUiState ui = node.ui;
                    ui.pos_x = mouse_world.x + node_drag_.world_offset.x;
                    ui.pos_y = mouse_world.y + node_drag_.world_offset.y;
                    profile.set_node_ui(node.node_id, ui);
                    any_node_interacted = true;
                    break;
                }
            }
        } else {
            node_drag_.reset();
        }
    }

    // Map node_id -> NodeSnapshot* for looking up receiver functions when completing a control wire
    std::map<uint32_t, const NodeSnapshot*> node_ptr_map;
    for (const auto& node : snap.nodes) {
        node_ptr_map[node.node_id] = &node;
    }

    for (const auto& node : snap.nodes) {
        auto layout_it = layouts.find(node.node_id);
        if (layout_it == layouts.end()) continue;
        const NodeLayoutMetrics& layout = layout_it->second;

        const bool is_control_node = (node.plane_type == AT_NODE_PLANE_CONTROL);
        const bool dim_in_audio_view = (is_audio_view && is_control_node);
        const float alpha_scale = dim_in_audio_view ? 0.45f : 1.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha_scale);
        ImGui::PushID(static_cast<int>(node.node_id));

        const ImVec2& screen_min = layout.screen_min;
        const ImVec2& screen_max = layout.screen_max;

        // 1. Node Background and Outer Border
        ImU32 bg_col = dim_in_audio_view ? IM_COL32(28, 30, 36, 150) : kColNodeBg;
        draw_list->AddRectFilled(screen_min, screen_max, bg_col, 6.0f);

        // 2. Topbar Header
        ImVec2 header_max(screen_max.x, screen_min.y + layout.header_height * view_.zoom);
        ImU32 header_col = (node.plane_type == AT_NODE_PLANE_AUDIO) ? kColAudioHeader : kColControlHeader;
        if (dim_in_audio_view) {
            header_col = IM_COL32(120, 80, 30, 140);
        }
        ImDrawFlags header_corners = node.ui.is_expanded ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
        draw_list->AddRectFilled(screen_min, header_max, header_col, 6.0f, header_corners);

        // Header Title (clipped so it never overlaps topbar buttons)
        float btn_sz = 18.0f * view_.zoom;
        float btn_y  = screen_min.y + ((layout.header_height - 18.0f) * 0.5f) * view_.zoom;
        ImVec2 btn_del(screen_max.x - 24.0f * view_.zoom, btn_y);
        ImVec2 btn_disc(btn_del.x - 22.0f * view_.zoom, btn_y);
        ImVec2 btn_exp(btn_disc.x - 22.0f * view_.zoom, btn_y);

        draw_list->PushClipRect(screen_min, ImVec2(btn_exp.x - 4.0f * view_.zoom, header_max.y), true);
        draw_list->AddText(ImVec2(screen_min.x + 10.0f * view_.zoom, screen_min.y + ((layout.header_height - 16.0f) * 0.5f) * view_.zoom),
                           IM_COL32(255, 255, 255, 255), node.display_name.c_str());
        draw_list->PopClipRect();

        // 3. Header Action Buttons: [_]/[+] Expand, [#] Disconnect All, [x] Delete
        bool header_btn_clicked = false;

        // Button: Delete [x]
        ImRect rect_del(btn_del, ImVec2(btn_del.x + btn_sz, btn_del.y + btn_sz));
        bool del_hover = rect_del.Contains(io.MousePos);
        draw_list->AddRectFilled(rect_del.Min, rect_del.Max, del_hover ? IM_COL32(215, 45, 45, 255) : IM_COL32(0, 0, 0, 75), 3.0f);
        draw_list->AddText(ImVec2(btn_del.x + 5.0f * view_.zoom, btn_del.y + 1.0f * view_.zoom), IM_COL32(255, 255, 255, 255), "x");
        if (del_hover) {
            any_node_interacted = true;
            ImGui::SetTooltip("Delete Node");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                header_btn_clicked = true;
                profile.delete_node(node.node_id);
                ImGui::PopID();
                ImGui::PopStyleVar();
                continue;
            }
        }

        // Button: Disconnect All [#]
        ImRect rect_disc(btn_disc, ImVec2(btn_disc.x + btn_sz, btn_disc.y + btn_sz));
        bool disc_hover = rect_disc.Contains(io.MousePos);
        draw_list->AddRectFilled(rect_disc.Min, rect_disc.Max, disc_hover ? IM_COL32(195, 110, 20, 255) : IM_COL32(0, 0, 0, 75), 3.0f);
        draw_list->AddText(ImVec2(btn_disc.x + 4.0f * view_.zoom, btn_disc.y + 1.0f * view_.zoom), IM_COL32(255, 255, 255, 255), "#");
        if (disc_hover) {
            any_node_interacted = true;
            ImGui::SetTooltip("Disconnect all wires on active plane");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                header_btn_clicked = true;
                profile.disconnect_all_node_wires(node.node_id, is_audio_view, !is_audio_view);
            }
        }

        // Button: Toggle Form 1 / Form 2 [_] / [+]
        ImRect rect_exp(btn_exp, ImVec2(btn_exp.x + btn_sz, btn_exp.y + btn_sz));
        bool exp_hover = rect_exp.Contains(io.MousePos);
        draw_list->AddRectFilled(rect_exp.Min, rect_exp.Max, exp_hover ? IM_COL32(85, 95, 120, 255) : IM_COL32(0, 0, 0, 75), 3.0f);
        draw_list->AddText(ImVec2(btn_exp.x + 4.5f * view_.zoom, btn_exp.y + 1.0f * view_.zoom),
                           IM_COL32(255, 255, 255, 255), node.ui.is_expanded ? "_" : "+");
        if (exp_hover) {
            any_node_interacted = true;
            ImGui::SetTooltip(node.ui.is_expanded ? "Minimize to Form 1" : "Expand to Form 2");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                header_btn_clicked = true;
                NodeUiState ui = node.ui;
                ui.is_expanded = !ui.is_expanded;
                profile.set_node_ui(node.node_id, ui);
            }
        }

        // 4. Form 2: Stacked Table Rows for Node Controls
        //    Each row has two cells: Left Cell (Label) and Right Cell (Interactive Control)
        //    Every control explicitly sets its screen cursor position so rows NEVER snap to the window left!
        if (node.ui.is_expanded && !node.controls.empty()) {
            float scaled_pad_y = std::max(1.0f, 3.0f * view_.zoom);
            float scaled_pad_x = std::max(2.0f, 6.0f * view_.zoom);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(scaled_pad_x, scaled_pad_y));

            const float col_sep_x = screen_min.x + (10.0f + layout.label_col_width) * view_.zoom;
            const float widget_x  = col_sep_x + 8.0f * view_.zoom;
            const float widget_w  = std::max(40.0f, layout.widget_col_width * view_.zoom);

            for (size_t r = 0; r < node.controls.size(); ++r) {
                const auto& ctrl = node.controls[r];
                ImGui::PushID(static_cast<int>(ctrl.control_id));

                const float row_top    = screen_min.y + (layout.header_height + static_cast<float>(r) * layout.row_height) * view_.zoom;
                const float row_bottom = row_top + layout.row_height * view_.zoom;
                const float row_h      = row_bottom - row_top;

                // Alternating subtle table row background & borders
                ImU32 row_bg = ((r & 1) == 0) ? kColNodeRowEven : kColNodeRowOdd;
                if (!dim_in_audio_view) {
                    draw_list->AddRectFilled(ImVec2(screen_min.x + 1.0f, row_top), ImVec2(screen_max.x - 1.0f, row_bottom), row_bg);
                }
                draw_list->AddLine(ImVec2(screen_min.x + 1.0f, row_bottom), ImVec2(screen_max.x - 1.0f, row_bottom), kColRowBorder, 1.0f);
                draw_list->AddLine(ImVec2(col_sep_x, row_top), ImVec2(col_sep_x, row_bottom), kColRowBorder, 1.0f);

                // Left Table Cell: Control Display Name (vertically centered & clipped to label column)
                ImVec2 label_sz = ImGui::CalcTextSize(ctrl.display_name.c_str());
                float label_y = row_top + std::max(2.0f, (row_h - label_sz.y) * 0.5f);
                draw_list->PushClipRect(ImVec2(screen_min.x + 4.0f, row_top), ImVec2(col_sep_x - 4.0f, row_bottom), true);
                draw_list->AddText(ImVec2(screen_min.x + 10.0f * view_.zoom, label_y), kColTextMuted, ctrl.display_name.c_str());
                draw_list->PopClipRect();

                // Right Table Cell: Interactive Control Widget
                // Explicitly anchor cursor horizontally to widget_x and vertically inside row r!
                float frame_h = ImGui::GetFrameHeight();
                float widget_y = row_top + std::max(2.0f, (row_h - frame_h) * 0.5f);
                ImGui::SetCursorScreenPos(ImVec2(widget_x, widget_y));

                if (ctrl.control_type == AT_CTRL_LEVEL_METER) {
                    float val_db = static_cast<float>(ctrl.value.float_val);
                    float norm = std::clamp((val_db + 60.0f) / 66.0f, 0.0f, 1.0f);
                    float meter_h = std::max(10.0f, 14.0f * view_.zoom);
                    float meter_y = row_top + (row_h - meter_h) * 0.5f;
                    ImVec2 p_min(widget_x, meter_y);
                    ImVec2 p_max(widget_x + widget_w, meter_y + meter_h);

                    draw_list->AddRectFilled(p_min, p_max, IM_COL32(18, 20, 25, 255), 3.0f);
                    if (norm > 0.005f) {
                        ImU32 bar_col = (val_db > -1.0f) ? IM_COL32(235, 65, 65, 255)
                                      : ((val_db > -12.0f) ? IM_COL32(235, 195, 45, 255) : IM_COL32(45, 205, 120, 255));
                        draw_list->AddRectFilled(p_min, ImVec2(p_min.x + widget_w * norm, p_max.y), bar_col, 3.0f);
                    }
                    draw_list->AddRect(p_min, p_max, IM_COL32(55, 60, 72, 255), 3.0f);

                    char db_buf[32];
                    if (val_db <= -90.0f) std::snprintf(db_buf, sizeof(db_buf), "-inf dB");
                    else std::snprintf(db_buf, sizeof(db_buf), "%.1f dB", val_db);
                    ImVec2 db_sz = ImGui::CalcTextSize(db_buf);
                    draw_list->AddText(ImVec2(p_max.x - db_sz.x - 6.0f * view_.zoom, row_top + (row_h - db_sz.y) * 0.5f),
                                       kColTextPrimary, db_buf);

                    ImGui::Dummy(ImVec2(widget_w, meter_h));
                } else if (ctrl.control_type == AT_CTRL_SLIDER) {
                    float fval = static_cast<float>(ctrl.value.float_val);
                    if (ctrl.preferred_style == AT_STYLE_ROTARY_KNOB) {
                        float knob_r = std::max(8.0f, 11.0f * view_.zoom);
                        ImGui::SetCursorScreenPos(ImVec2(widget_x, row_top + (row_h - knob_r * 2.0f) * 0.5f));
                        if (draw_rotary_knob("##knob", &fval, static_cast<float>(ctrl.min_value),
                                             static_cast<float>(ctrl.max_value), ctrl.unit_label.c_str(), knob_r, widget_w)) {
                            profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_float(fval));
                        }
                    } else {
                        std::string fmt = ctrl.unit_label.empty() ? "%.1f" : ("%.1f " + ctrl.unit_label);
                        ImGui::SetNextItemWidth(widget_w);
                        if (ImGui::SliderFloat("##slider", &fval, static_cast<float>(ctrl.min_value),
                                               static_cast<float>(ctrl.max_value), fmt.c_str())) {
                            profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_float(fval));
                        }
                    }
                } else if (ctrl.control_type == AT_CTRL_BUTTON || ctrl.control_type == AT_CTRL_TOGGLE_SWITCH) {
                    bool bval = ctrl.value.bool_val;
                    if (bval) {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(210, 75, 45, 255));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(230, 95, 60, 255));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(185, 60, 35, 255));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(52, 58, 72, 255));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(68, 76, 94, 255));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(44, 50, 62, 255));
                    }

                    std::string btn_label = (bval ? "ON" : "OFF") + std::string("##btn");
                    if (ImGui::Button(btn_label.c_str(), ImVec2(widget_w, frame_h))) {
                        profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_bool(!bval));
                    }
                    ImGui::PopStyleColor(3);
                } else if (ctrl.control_type == AT_CTRL_AUDIO_DEVICE_SELECT) {
                    std::string cur_dev = ctrl.value.string_val;
                    std::string label = cur_dev;

                    for (const auto& d : cached_devices_) {
                        if (d.endpoint_id == cur_dev) {
                            label = d.friendly_name;
                            break;
                        }
                    }

                    ImGui::SetNextItemWidth(widget_w);
                    if (ImGui::BeginCombo("##devcombo", label.c_str())) {
                        // Refresh cached device list only when the combo popup first opens
                        if (ImGui::IsWindowAppearing()) {
                            ImGui::SetWindowFontScale(1.0f);
                            cached_devices_ = engine.device_manager().enumerate_devices();
                        }
                        bool want_capture  = (ctrl.device_filter & AT_DEV_FILTER_INPUT_CAPTURE) != 0;
                        bool want_loopback = (ctrl.device_filter & AT_DEV_FILTER_LOOPBACK) != 0;
                        bool want_render   = (ctrl.device_filter & AT_DEV_FILTER_OUTPUT_RENDER) != 0;

                        for (const auto& d : cached_devices_) {
                            bool is_loop = (d.endpoint_id == "default_loopback" || d.endpoint_id.rfind(AT_LOOPBACK_PREFIX, 0) == 0);
                            bool match = false;
                            if (want_capture && d.is_input_capture && !is_loop) match = true;
                            if (want_loopback && is_loop) match = true;
                            if (want_render && d.is_output_render) match = true;
                            if (!want_capture && !want_loopback && !want_render) match = true;

                            if (!match) continue;

                            bool selected = (d.endpoint_id == cur_dev);
                            if (ImGui::Selectable(d.friendly_name.c_str(), selected)) {
                                profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_string(d.endpoint_id));
                            }
                            if (selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else if (ctrl.control_type == AT_CTRL_RANGE_TEXT_INPUT || ctrl.control_type == AT_CTRL_VALUE_TEXT_INPUT) {
                    ImGui::SetNextItemWidth(widget_w);
                    if (ctrl.value_type == AT_VAL_INT) {
                        int ival = static_cast<int>(ctrl.value.int_val);
                        int step = std::max(1, static_cast<int>(ctrl.step_value));
                        if (ImGui::InputInt("##int_in", &ival, step, step * 5)) {
                            int64_t clamped = std::clamp<int64_t>(
                                ival, static_cast<int64_t>(ctrl.min_value), static_cast<int64_t>(ctrl.max_value));
                            profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_int(clamped));
                        }
                    } else if (ctrl.value_type == AT_VAL_FLOAT) {
                        float fval = static_cast<float>(ctrl.value.float_val);
                        float step = (ctrl.step_value > 0.0) ? static_cast<float>(ctrl.step_value) : 0.5f;
                        std::string fmt = ctrl.unit_label.empty() ? "%.1f" : ("%.1f " + ctrl.unit_label);
                        if (ImGui::InputFloat("##flt_in", &fval, step, step * 5.0f, fmt.c_str())) {
                            double clamped = std::clamp<double>(fval, ctrl.min_value, ctrl.max_value);
                            profile.set_control(node.node_id, ctrl.control_id, OwnedValue::make_float(clamped));
                        }
                    }
                }

                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                    any_node_interacted = true;
                }

                ImGui::PopID();
            }

            ImGui::PopStyleVar(); // FramePadding
        }

        // Outer border drawn over row backgrounds so corners stay crisp
        draw_list->AddRect(screen_min, screen_max, kColNodeBorder, 6.0f, 0, 1.5f);

        // 5. Snap Points (Left Input Dot & Right Output Dot at vertical midpoint)
        bool show_in_dot  = false;
        bool show_out_dot = false;
        if (is_audio_view) {
            if (node.plane_type == AT_NODE_PLANE_AUDIO) {
                show_in_dot  = (node.audio_category != AT_AUDIO_CAT_SOURCE);
                show_out_dot = (node.audio_category != AT_AUDIO_CAT_OUTPUT);
            }
        } else {
            // Control / Data View: Nodes with functions accept control triggers on left;
            // Control nodes emit triggers on right.
            show_in_dot  = !node.functions.empty();
            show_out_dot = (node.plane_type == AT_NODE_PLANE_CONTROL);
        }

        ImU32 dot_col = is_audio_view ? kColAudioDot : kColControlDot;
        bool snap_clicked = false;

        // Helper lambda to finalize a wire connection (Output -> Input)
        auto complete_connection = [&](uint32_t src_id, uint32_t src_port, uint32_t dst_id) {
            if (src_id == dst_id) return;
            if (is_audio_view) {
                std::string err;
                if (profile.connect_audio(src_id, dst_id, &err) == 0 && !err.empty()) {
                    status_toast_ = err;
                    status_toast_time_ = ImGui::GetTime();
                }
            } else {
                auto dst_it = node_ptr_map.find(dst_id);
                if (dst_it != node_ptr_map.end() && !dst_it->second->functions.empty()) {
                    func_picker_.pending_open   = true;
                    func_picker_.src_node_id    = src_id;
                    func_picker_.src_port_index = src_port;
                    func_picker_.dst_node_id    = dst_id;
                    func_picker_.dst_node_name  = dst_it->second->display_name;
                    func_picker_.functions      = dst_it->second->functions;
                }
            }
        };

        // Left Input Snap Point
        if (show_in_dot) {
            const ImVec2& in_snap = layout.in_snap_screen;
            float d_in = std::hypot(io.MousePos.x - in_snap.x, io.MousePos.y - in_snap.y);
            bool hover_in = (d_in <= kSnapHitRadius);
            float r = kSnapRadius * (hover_in ? 1.45f : 1.0f);
            draw_list->AddCircleFilled(in_snap, r, hover_in ? IM_COL32(80, 255, 140, 255) : dot_col);
            draw_list->AddCircle(in_snap, r, IM_COL32(255, 255, 255, 255), 0, 1.5f);

            if (hover_in) {
                any_node_interacted = true;
                ImGui::SetTooltip(is_audio_view ? "Audio Input Port (Click or Drag to Connect)"
                                                : "Control Trigger Receiver (Click or Drag to Bind Function)");

                if (!wire_drag_.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    wire_drag_.active           = true;
                    wire_drag_.click_mode       = false;
                    wire_drag_.source_kind      = is_audio_view ? WireDragSource::AudioInput : WireDragSource::ControlInput;
                    wire_drag_.node_id          = node.node_id;
                    wire_drag_.port_index       = 0;
                    wire_drag_.start_pos        = in_snap;
                    wire_drag_.press_screen_pos = io.MousePos;
                    snap_clicked = true;
                } else if (wire_drag_.active && wire_drag_.node_id != node.node_id) {
                    bool valid_source = is_audio_view ? (wire_drag_.source_kind == WireDragSource::AudioOutput)
                                                      : (wire_drag_.source_kind == WireDragSource::ControlOutput);
                    bool trigger_connect = (!wire_drag_.click_mode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) ||
                                           (wire_drag_.click_mode && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
                    if (valid_source && trigger_connect) {
                        complete_connection(wire_drag_.node_id, wire_drag_.port_index, node.node_id);
                        wire_drag_.reset();
                        snap_clicked = true;
                    }
                }
            }
        }

        // Right Output Snap Point
        if (show_out_dot) {
            const ImVec2& out_snap = layout.out_snap_screen;
            float d_out = std::hypot(io.MousePos.x - out_snap.x, io.MousePos.y - out_snap.y);
            bool hover_out = (d_out <= kSnapHitRadius);
            float r = kSnapRadius * (hover_out ? 1.45f : 1.0f);
            draw_list->AddCircleFilled(out_snap, r, hover_out ? IM_COL32(80, 255, 140, 255) : dot_col);
            draw_list->AddCircle(out_snap, r, IM_COL32(255, 255, 255, 255), 0, 1.5f);

            if (hover_out) {
                any_node_interacted = true;
                ImGui::SetTooltip(is_audio_view ? "Audio Output Port (Click or Drag to Connect)"
                                                : "Control Trigger Output (Click or Drag to Connect)");

                if (!wire_drag_.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    wire_drag_.active           = true;
                    wire_drag_.click_mode       = false;
                    wire_drag_.source_kind      = is_audio_view ? WireDragSource::AudioOutput : WireDragSource::ControlOutput;
                    wire_drag_.node_id          = node.node_id;
                    wire_drag_.port_index       = 0;
                    wire_drag_.start_pos        = out_snap;
                    wire_drag_.press_screen_pos = io.MousePos;
                    snap_clicked = true;
                } else if (wire_drag_.active && wire_drag_.node_id != node.node_id) {
                    bool valid_source = is_audio_view ? (wire_drag_.source_kind == WireDragSource::AudioInput)
                                                      : (wire_drag_.source_kind == WireDragSource::ControlInput);
                    bool trigger_connect = (!wire_drag_.click_mode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) ||
                                           (wire_drag_.click_mode && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
                    if (valid_source && trigger_connect) {
                        // Reverse gesture (Input -> Output): node.node_id is the Output source, wire_drag_.node_id is the Input receiver!
                        complete_connection(node.node_id, 0, wire_drag_.node_id);
                        wire_drag_.reset();
                        snap_clicked = true;
                    }
                }
            }
        }

        // 6. Header Bar Dragging & Double-Click to Expand/Collapse
        ImRect header_rect(screen_min, header_max);
        if (header_rect.Contains(io.MousePos) && !header_btn_clicked && !snap_clicked) {
            any_node_interacted = true;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                NodeUiState ui = node.ui;
                ui.is_expanded = !ui.is_expanded;
                profile.set_node_ui(node.node_id, ui);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !wire_drag_.active) {
                ImVec2 mouse_world = view_.screen_to_world(io.MousePos, origin);
                node_drag_.active       = true;
                node_drag_.node_id      = node.node_id;
                node_drag_.world_offset = ImVec2(node.ui.pos_x - mouse_world.x, node.ui.pos_y - mouse_world.y);
            }
        }

        // Prevent clicking inside a node body from starting a background canvas pan
        ImRect full_node_rect(screen_min, screen_max);
        if (full_node_rect.Contains(io.MousePos)) {
            any_node_interacted = true;
        }

        ImGui::PopID();
        ImGui::PopStyleVar(); // Alpha
    }

    return any_node_interacted;
}

void CanvasRenderer::render_context_menu(ProfileGraph& profile, AudioEngine& engine) {
    if (ImGui::BeginPopup("CanvasContextMenu")) {
        ImGui::TextDisabled("Create New Node");
        ImGui::Separator();

        auto plugins = engine.list_plugins();
        FolderMenuTree root;
        build_folder_tree(root, plugins);

        bool is_audio_view = (profile.metadata().active_plane_view == "AUDIO");
        std::string toast;
        render_folder_tree_node(root, profile, spawn_world_pos_, is_audio_view, &toast);
        if (!toast.empty()) {
            status_toast_ = toast;
            status_toast_time_ = ImGui::GetTime();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Reset Canvas Pan & Zoom")) {
            view_.pan_x = 0.0f;
            view_.pan_y = 0.0f;
            view_.zoom  = 1.0f;
            profile.set_canvas_state(0.0f, 0.0f, profile.metadata().active_plane_view, 1.0f);
        }

        ImGui::EndPopup();
    }
}

void CanvasRenderer::render_function_picker_popup(ProfileGraph& profile) {
    if (func_picker_.pending_open) {
        ImGui::OpenPopup("CanvasFunctionPickerPopup");
        func_picker_.pending_open = false;
    }

    if (ImGui::BeginPopup("CanvasFunctionPickerPopup")) {
        ImGui::TextDisabled("Bind Trigger to Function on '%s':", func_picker_.dst_node_name.c_str());
        ImGui::Separator();

        for (const auto& func : func_picker_.functions) {
            if (ImGui::MenuItem(func.display_name.c_str())) {
                if (func.parameter_type == AT_VAL_NONE) {
                    profile.connect_control(func_picker_.src_node_id, func_picker_.src_port_index,
                                            func_picker_.dst_node_id, func.function_id, OwnedValue{});
                } else {
                    binding_modal_.pending_open   = true;
                    binding_modal_.src_node_id    = func_picker_.src_node_id;
                    binding_modal_.src_port_index = func_picker_.src_port_index;
                    binding_modal_.dst_node_id    = func_picker_.dst_node_id;
                    binding_modal_.function       = func;
                    binding_modal_.entered_number = func.default_value;
                    binding_modal_.entered_bool   = false;
                }
            }
            if (!func.description.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", func.description.c_str());
            }
        }

        ImGui::EndPopup();
    }
}

void CanvasRenderer::render_function_binding_modal(ProfileGraph& profile) {
    if (binding_modal_.pending_open) {
        ImGui::OpenPopup("Configure Control Function");
        binding_modal_.pending_open = false;
    }

    if (ImGui::BeginPopupModal("Configure Control Function", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Bind trigger pulse to: %s", binding_modal_.function.display_name.c_str());
        if (!binding_modal_.function.description.empty()) {
            ImGui::TextDisabled("%s", binding_modal_.function.description.c_str());
        }
        ImGui::Separator();

        if (binding_modal_.function.parameter_type == AT_VAL_FLOAT) {
            ImGui::Text("%s (%s) [%.1f .. %.1f]:",
                        binding_modal_.function.parameter_name.c_str(),
                        binding_modal_.function.parameter_unit.c_str(),
                        binding_modal_.function.min_value,
                        binding_modal_.function.max_value);
            float fval = static_cast<float>(binding_modal_.entered_number);
            if (ImGui::InputFloat("##param_flt", &fval, 0.5f, 2.0f, "%.1f")) {
                binding_modal_.entered_number = std::clamp<double>(
                    fval, binding_modal_.function.min_value, binding_modal_.function.max_value);
            }
        } else if (binding_modal_.function.parameter_type == AT_VAL_INT) {
            ImGui::Text("%s (%s) [%lld .. %lld]:",
                        binding_modal_.function.parameter_name.c_str(),
                        binding_modal_.function.parameter_unit.c_str(),
                        static_cast<long long>(binding_modal_.function.min_value),
                        static_cast<long long>(binding_modal_.function.max_value));
            int ival = static_cast<int>(binding_modal_.entered_number);
            if (ImGui::InputInt("##param_int", &ival, 1, 10)) {
                binding_modal_.entered_number = std::clamp<double>(
                    ival, binding_modal_.function.min_value, binding_modal_.function.max_value);
            }
        } else if (binding_modal_.function.parameter_type == AT_VAL_BOOL) {
            ImGui::Checkbox(binding_modal_.function.parameter_name.c_str(), &binding_modal_.entered_bool);
        }

        ImGui::Spacing();
        if (ImGui::Button("Bind Function", ImVec2(140, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            OwnedValue arg;
            if (binding_modal_.function.parameter_type == AT_VAL_FLOAT) {
                arg = OwnedValue::make_float(binding_modal_.entered_number);
            } else if (binding_modal_.function.parameter_type == AT_VAL_INT) {
                arg = OwnedValue::make_int(static_cast<int64_t>(binding_modal_.entered_number));
            } else if (binding_modal_.function.parameter_type == AT_VAL_BOOL) {
                arg = OwnedValue::make_bool(binding_modal_.entered_bool);
            }
            profile.connect_control(binding_modal_.src_node_id, binding_modal_.src_port_index,
                                    binding_modal_.dst_node_id, binding_modal_.function.function_id, arg);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace at
