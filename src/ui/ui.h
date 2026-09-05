#pragma once

#include "core/result.h"
#include "ui/animated_image.h"
#include "ui/image_renderer.h"
#include "ui/preferences.h"
#include "ui/profiler.h"
#include "ui/source_view.h"
#include "ui/theme.h"

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

        static auto begin_frame() -> void;
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
            bool watch = true;
            bool registers = true;
            bool symbols = true;
            bool disassembly = true;
            bool console = true;

            // these two are not standing panels: they earn their place only once
            // there is something to show. the profiler opens when you go to profile
            // (its flame chart, timings and graphs all live inside it), the macros
            // view when you unroll a macro from source
            bool profiler = false;
            bool macros = false;
        };

        auto apply_style() -> void;

        // loads the bundled Inter / JetBrains Mono / Phosphor faces from
        // assets/fonts and merges the icons over the ui fonts; falls back to the
        // built-in vector font if the files are missing
        auto load_fonts() -> void;

        // theme handling: the neutral palette lives in m_theme (reloaded from
        // disk at startup and whenever a theme is picked); the accent, rounding
        // and syntax colours are seeded from it into the live preferences so they
        // stay individually tweakable afterwards
        auto load_selected_theme(bool seed_preferences) -> void;
        auto seed_preferences_from_theme() -> void;
        auto select_theme(const ThemeEntry& entry) -> void;
        static auto themes_directory() -> std::filesystem::path;

        static auto build_default_layout(uint32_t dockspace_id) -> void;

        // a fuzzy omnibar (cmd/ctrl+k) over files, symbols and execution commands,
        // so navigating and driving the target never means hunting for a panel
        auto draw_command_palette(Debugger& debugger) -> void;

        auto draw_menu_bar(Debugger& debugger) -> void;
        auto draw_toolbar(Debugger& debugger) -> void;
        auto draw_status_bar(const Debugger& debugger) -> void;
        auto draw_load_target_popup(Debugger& debugger) -> void;
        auto draw_preferences_window() -> void;
        auto apply_preferences() -> void;

        auto draw_source_panel(Debugger& debugger) -> void;
        auto draw_breakpoints_panel(Debugger& debugger) -> void;
        auto draw_call_stack_panel(Debugger& debugger) -> void;
        auto draw_threads_panel(Debugger& debugger) -> void;
        auto draw_source_tree_panel(const Debugger& debugger) -> void;
        auto draw_locals_panel(const Debugger& debugger) -> void;
        auto draw_watch_panel(Debugger& debugger) -> void;
        auto draw_registers_panel(const Debugger& debugger) -> void;

        // adds an expression to the watch list and evaluates it right away if the
        // target is stopped, so it never shows a blank until the next stop
        auto add_watch(Debugger& debugger, std::string expression) -> void;
        auto draw_symbols_panel(Debugger& debugger) -> void;
        auto draw_disassembly_panel(Debugger& debugger) -> void;
        auto draw_console_panel(Debugger& debugger) -> void;
        auto draw_profiler_panel(Debugger& debugger) -> void;

        // the call flame chart, the hero of the profiler panel: draws into whatever
        // region the caller has opened, no window of its own
        static auto draw_flamegraph(const Debugger& debugger) -> void;
        auto draw_macros_panel(Debugger& debugger) -> void;

        auto push_console(std::string line) -> void;
        auto report(const Result<void>& result, std::string_view action) -> void;

        auto follow_stop(Debugger& debugger) -> void;
        auto follow_target(Debugger& debugger) -> void;

        // selects the innermost frame that has source and shows it, so a new
        // thread or stop lands the whole ui on real code rather than a runtime
        // internal frame with nothing to display
        auto follow_selected_frame(Debugger& debugger) -> void;
        auto show_frame(const StackFrame& frame) -> void;

        Window* m_window = nullptr;
        SourceView m_source_view;
        Profiler m_profiler;
        PanelVisibility m_visible;

        Preferences m_preferences;
        std::filesystem::path m_preferences_path;
        bool m_show_preferences = false;
        bool m_restyle_pending = false;
        int m_preferences_tab = 0;

        // the loaded theme's neutral palette and style metrics, and the cached
        // list of theme files shown in the picker (rescanned when the window
        // opens so newly dropped-in files appear)
        Theme m_theme;
        std::vector<ThemeEntry> m_themes;
        bool m_prefs_open_prev = false;

        // the bundled faces, all with Phosphor icons merged in: ui is Nunito (its
        // rounded letterforms echo the rounded ui), the strong variant is its
        // SemiBold for headers and toolbar labels, mono is JetBrains Mono for code
        ImFont* m_font_ui = nullptr;
        ImFont* m_font_strong = nullptr;
        ImFont* m_font_mono = nullptr;

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
        std::string m_trace_input;

        // one user-entered watch expression and its last evaluation
        struct Watch
        {
            std::string expression;
            std::string value;
            bool ok = false;
        };

        std::vector<Watch> m_watches;
        std::string m_watch_input;

        // the (stop, thread, frame) the watches were last evaluated against, so
        // they re-run when execution stops again or the selection moves, but not
        // every frame (each evaluation jits code into the target)
        uint64_t m_watch_stop = 0;
        uint64_t m_watch_thread = 0;
        uint32_t m_watch_frame = 0;
        bool m_watch_evaluated = false;

        // command palette: whether it is up, a one-shot request to grab the text
        // field's focus, the query, and which result the keyboard has landed on
        bool m_palette_open = false;
        bool m_palette_request = false;
        bool m_palette_focus = false;
        std::string m_palette_query;
        int m_palette_selection = 0;

        // macros panel: the invocation being unrolled, the current unroll depth,
        // and the read-only buffer that shows the tokens at that depth
        std::string m_macro_input;
        std::string m_macro_output;
        int m_macro_level = 0;

        uint64_t m_followed_stop = 0;
        std::filesystem::path m_followed_target;

        // the source file whose folder chain the tree last expanded; when the
        // open file moves away from it the tree reveals the new path once
        std::filesystem::path m_revealed_source;

        // rising-edge latch so the profiler surfaces itself the first time there is
        // profiling activity, without fighting the user if they then close it
        bool m_profiler_revealed = false;

        bool m_layout_built = false;
        bool m_select_default_tabs = false;
        bool m_console_scroll_pending = false;
        bool m_load_target_pending = false;
        bool m_focus_breakpoints = false;
        bool m_focus_macros = false;
        bool m_focus_profiler = false;
        bool m_focus_symbols = false;
        bool m_focus_disassembly = false;
        bool m_scroll_to_program_counter = false;
        bool m_scroll_to_symbol = false;
    };
}
