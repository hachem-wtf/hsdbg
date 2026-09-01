#pragma once

#include "core/result.h"
#include "ui/animated_image.h"
#include "ui/image_renderer.h"
#include "ui/preferences.h"
#include "ui/source_view.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Hsdbg
{
    class Debugger;
    class Window;
    struct StackFrame;

    class Ui
    {
    public:
        explicit Ui(Window& window);
        ~Ui();

        Ui(const Ui&) = delete;
        Ui(Ui&&) = delete;
        auto operator=(const Ui&) -> Ui& = delete;
        auto operator=(Ui&&) -> Ui& = delete;

        auto begin_frame() -> void;
        auto draw(Debugger& debugger) -> void;
        auto end_frame() -> void;

        // where the ui will jump when a frame is selected or a breakpoint is hit
        auto open_source(const std::filesystem::path& path) -> void;

    private:
        struct PanelVisibility
        {
            bool source = true;
            bool breakpoints = true;
            bool call_stack = true;
            bool threads = true;
            bool source_tree = true;
            bool locals = true;
            bool registers = true;
            bool symbols = true;
            bool disassembly = true;
            bool console = true;
            bool demo = false;
        };

        auto apply_style() -> void;
        auto build_default_layout(uint32_t dockspace_id) -> void;

        auto draw_menu_bar(Debugger& debugger) -> void;
        auto draw_toolbar(Debugger& debugger) -> void;
        auto draw_status_bar(Debugger& debugger) -> void;
        auto draw_load_target_popup(Debugger& debugger) -> void;
        auto draw_preferences_window() -> void;
        auto apply_preferences() -> void;

        auto draw_source_panel(Debugger& debugger) -> void;
        auto draw_breakpoints_panel(Debugger& debugger) -> void;
        auto draw_call_stack_panel(Debugger& debugger) -> void;
        auto draw_threads_panel(Debugger& debugger) -> void;
        auto draw_source_tree_panel(Debugger& debugger) -> void;
        auto draw_locals_panel(Debugger& debugger) -> void;
        auto draw_registers_panel(Debugger& debugger) -> void;
        auto draw_symbols_panel(Debugger& debugger) -> void;
        auto draw_disassembly_panel(Debugger& debugger) -> void;
        auto draw_console_panel(Debugger& debugger) -> void;

        auto push_console(std::string line) -> void;
        auto report(const Result<void>& result, std::string_view action) -> void;

        auto follow_stop(Debugger& debugger) -> void;
        auto follow_target(Debugger& debugger) -> void;
        auto show_frame(const StackFrame& frame) -> void;

        Window* m_window = nullptr;
        SourceView m_source_view;
        PanelVisibility m_visible;

        Preferences m_preferences;
        std::filesystem::path m_preferences_path;
        bool m_show_preferences = false;
        bool m_restyle_pending = false;
        int m_preferences_tab = 0;

        // the crying-pepe that lives in the toolbar; loaded on the first frame
        // once there is a gl context to upload its textures to, and drawn with a
        // dedicated shader after imgui rather than through ImGui::Image
        AnimatedImage m_mascot;
        ImageRenderer m_image_renderer;
        bool m_mascot_loaded = false;
        bool m_image_renderer_ready = false;
        bool m_mascot_pending = false;
        unsigned int m_mascot_texture = 0;
        float m_mascot_x0 = 0.0f;
        float m_mascot_y0 = 0.0f;
        float m_mascot_x1 = 0.0f;
        float m_mascot_y1 = 0.0f;

        std::vector<std::string> m_console_lines;
        std::string m_console_input;
        std::string m_target_input;
        std::string m_symbol_filter;
        std::string m_source_filter;

        uint64_t m_followed_stop = 0;
        std::filesystem::path m_followed_target;

        bool m_layout_built = false;
        bool m_select_default_tabs = false;
        bool m_console_scroll_pending = false;
        bool m_load_target_pending = false;
        bool m_focus_breakpoints = false;
        bool m_focus_symbols = false;
        bool m_focus_disassembly = false;
        bool m_scroll_to_program_counter = false;
        bool m_scroll_to_symbol = false;
    };
}
