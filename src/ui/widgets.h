#pragma once

#include <imgui.h>

// a small kit of hand-drawn controls that move the ui away from stock imgui:
// rounded icon buttons, ios-style toggles, pill segmented controls, rounded
// selection rows and status chips. every colour is pulled from the active
// ImGuiStyle, so these follow the theme like everything else.
// i wonder when stock imgui will stop looking like dog shit
namespace Hsdbg::Widgets
{
    // register the bundled faces so the kit can set headers in the bold face and
    // values in mono; call once after the fonts are loaded
    auto set_fonts(ImFont* ui, ImFont* strong, ImFont* mono) -> void;

    // a small, bold, upper-cased section label with a hairline running off to the
    // right — the strong equivalent of ImGui::SeparatorText
    auto section_header(const char* label) -> void;

    // a modern slider: a thin rounded track with an accent-filled portion and a
    // round grab, the value centred on the track, then the label to its right
    // (like ImGui::SliderFloat). returns true on the frames the value changes.
    auto slider_float(const char* label, float* value, float min, float max,
                      const char* fmt) -> bool;

    // a rounded square button whose label is a (Phosphor) icon glyph. `active`
    // fills it with the accent to mark a toggled-on tool. returns true on click.
    auto icon_button(const char* id, const char* icon, bool active = false,
                     const char* tooltip = nullptr, bool disabled = false,
                     float size = 34.0f) -> bool;

    // an ios-style switch bound to *value; returns true on the frame it flips
    auto toggle(const char* id, bool* value) -> bool;

    // a pill segmented control; returns the index the user ends on (unchanged
    // when nothing was clicked this frame)
    auto segmented(const char* id, const char* const* labels, int count, int current) -> int;

    // a full-width, rounded selection row that behaves like ImGui::Selectable
    // but draws a rounded accent highlight; give it a label that may lead with an
    // icon glyph. returns true on click.
    auto selectable_row(const char* label, bool selected,
                        ImGuiSelectableFlags flags = 0) -> bool;

    // a small status chip: a coloured dot followed by text inside a rounded
    // outline. advances the cursor like a normal item.
    auto chip(const char* text, ImU32 dot_color) -> void;

    // a centred, dimmed icon over a caption, filling the panel — the resting
    // state for a panel that has nothing to show yet
    auto empty_state(const char* icon, const char* text) -> void;

    // a small borderless × for removing a row (watch, breakpoint, trace); dim at
    // rest, red on hover. sized to the text line so it fits inside a table cell.
    auto remove_button(const char* id) -> bool;
}
