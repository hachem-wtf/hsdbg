#pragma once

#include <filesystem>

namespace Hsdbg
{
    // user-facing settings edited from the preferences panel and persisted next
    // to the imgui layout; every field wires to something the ui actually reads
    struct Preferences
    {
        // appearance
        float ui_scale = 1.0f;               // scales every font, 0.75 - 2.0
        float accent[3] = { 0.30f, 0.72f, 0.78f }; // the teal that runs through the theme
        float rounding = 4.0f;               // corner radius on frames, windows, tabs
        bool show_mascot = true;             // the crying-pepe in the toolbar
        float mascot_scale = 1.6f;           // mascot height as a multiple of a button

        // editor
        bool syntax_highlighting = true;
        bool show_line_numbers = true;
        bool highlight_current_line = true;
        float color_keyword[3] = { 0.337f, 0.612f, 0.839f };
        float color_type[3] = { 0.306f, 0.788f, 0.690f };
        float color_string[3] = { 0.808f, 0.569f, 0.471f };
        float color_number[3] = { 0.710f, 0.808f, 0.659f };
        float color_comment[3] = { 0.416f, 0.600f, 0.333f };
        float color_preprocessor[3] = { 0.773f, 0.525f, 0.753f };
        float color_current_line[3] = { 0.227f, 0.282f, 0.180f };

        // debugger
        bool stop_at_entry = false;          // break at the entry point on launch
        bool step_by_instruction = false;    // step buttons move one instruction, not a line

        // interface
        bool show_fps = true;                // frames-per-second read-out in the status bar
    };

    auto load_preferences(const std::filesystem::path& path) -> Preferences;
    auto save_preferences(const std::filesystem::path& path, const Preferences& preferences) -> void;
}
