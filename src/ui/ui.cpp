#include "ui/ui.h"

#include "core/log.h"
#include "core/window.h"
#include "debugger/debugger.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <vector>

#ifndef HSDBG_ASSET_DIR
    #define HSDBG_ASSET_DIR ""
#endif

namespace Hsdbg
{
    namespace
    {
        constexpr const char* PANEL_SOURCE = "source";
        constexpr const char* PANEL_BREAKPOINTS = "breakpoints";
        constexpr const char* PANEL_CALL_STACK = "call stack";
        constexpr const char* PANEL_THREADS = "threads";
        constexpr const char* PANEL_SOURCE_TREE = "source tree";
        constexpr const char* PANEL_LOCALS = "locals";
        constexpr const char* PANEL_WATCH = "watch";
        constexpr const char* PANEL_REGISTERS = "registers";
        constexpr const char* PANEL_SYMBOLS = "symbols";
        constexpr const char* PANEL_DISASSEMBLY = "disassembly";
        constexpr const char* PANEL_CONSOLE = "console";
        constexpr const char* PANEL_PROFILER = "profiler";
        constexpr const char* PANEL_MACROS = "macros";

        constexpr const char* LOAD_TARGET_POPUP = "load target";

        // the lowest glsl that every 3.2+ core profile accepts, and the window
        // asks for a 3.3 core context on all three platforms
        constexpr const char* GLSL_VERSION = "#version 150";

        constexpr size_t MAX_CONSOLE_LINES = 2048;

        // breathing room between the window edge and everything docked inside it
        const ImVec2 ROOT_PADDING(8.0f, 6.0f);
        const ImVec2 TOOLBAR_PADDING(4.0f, 4.0f);

        // the same green the source view puts behind the current line
        const ImU32 CURRENT_INSTRUCTION_COLOR = IM_COL32(58, 72, 46, 255);

        constexpr float BREAKPOINT_RADIUS = 5.0f;
        constexpr float DISASSEMBLY_GUTTER_WIDTH = 22.0f;

        const ImU32 BREAKPOINT_COLOR = IM_COL32(226, 84, 84, 255);
        const ImU32 BREAKPOINT_DISABLED_COLOR = IM_COL32(120, 90, 90, 255);
        const ImU32 BREAKPOINT_HOVER_COLOR = IM_COL32(226, 84, 84, 90);

        // folders read as structure, so they take a cool tint; the file that is
        // currently open in the source view keeps its accent even when the row
        // is not the selected one, so it stays easy to find in a long tree
        const ImU32 SOURCE_FOLDER_COLOR = IM_COL32(150, 178, 214, 255);
        const ImU32 SOURCE_OPEN_FILE_COLOR = IM_COL32(126, 194, 126, 255);

        // the dim connectors drawn between a folder and its children
        const ImU32 SOURCE_TREE_LINE_COLOR = IM_COL32(110, 110, 122, 160);

        auto breakpoint_at(const Debugger& debugger, const Instruction& instruction) -> const Breakpoint*
        {
            const std::span<const Breakpoint> breakpoints = debugger.breakpoints();
            const auto found = std::ranges::find_if(breakpoints, [&](const Breakpoint& candidate)
            {
                if (instruction.file_address != 0 &&
                    candidate.file_address == instruction.file_address)
                {
                    return true;
                }

                return candidate.address != 0 && candidate.address == instruction.address;
            });

            return found != breakpoints.end() ? &*found : nullptr;
        }

        auto toggle_instruction_breakpoint(Debugger& debugger, const Instruction& instruction) -> void
        {
            if (const Breakpoint* existing = breakpoint_at(debugger, instruction); existing != nullptr)
            {
                debugger.remove_breakpoint(existing->id);
                return;
            }

            if (instruction.file_address != 0)
                debugger.add_address_breakpoint(instruction.file_address);
        }

        auto matches_filter(std::string_view name, std::string_view filter) -> bool
        {
            if (filter.empty())
                return true;

            if (filter.size() > name.size())
                return false;

            const auto equal = [](char left, char right)
            {
                return std::tolower(static_cast<unsigned char>(left)) ==
                       std::tolower(static_cast<unsigned char>(right));
            };

            return std::ranges::search(name, filter, equal).begin() != name.end();
        }

        // the sixteen standard ansi colours, tuned to read on a dark console (the
        // vs code terminal set); indices 0-7 are the normal colours, 8-15 bright
        constexpr ImU32 ANSI_COLORS[16] = {
            IM_COL32( 90,  90,  90, 255), IM_COL32(205,  49,  49, 255),
            IM_COL32( 13, 188, 121, 255), IM_COL32(229, 229,  16, 255),
            IM_COL32( 59, 142, 234, 255), IM_COL32(188,  63, 188, 255),
            IM_COL32( 17, 168, 205, 255), IM_COL32(204, 204, 204, 255),
            IM_COL32(127, 127, 127, 255), IM_COL32(241,  76,  76, 255),
            IM_COL32( 35, 209, 139, 255), IM_COL32(245, 245,  67, 255),
            IM_COL32( 96, 165, 250, 255), IM_COL32(214, 112, 214, 255),
            IM_COL32( 41, 184, 219, 255), IM_COL32(255, 255, 255, 255),
        };

        // the colour an xterm 256-colour index maps to: the 16 base colours, then
        // the 6x6x6 rgb cube, then the 24-step grey ramp
        auto xterm_256_color(int index) -> ImU32
        {
            if (index < 16)
                return ANSI_COLORS[index];

            if (index < 232)
            {
                const int value = index - 16;
                const auto step = [](int component)
                {
                    return component == 0 ? 0 : 55 + component * 40;
                };

                return IM_COL32(step(value / 36), step((value / 6) % 6), step(value % 6), 255);
            }

            const int grey = 8 + (index - 232) * 10;
            return IM_COL32(grey, grey, grey, 255);
        }

        // the foreground state an SGR (colour) escape run leaves behind
        struct AnsiStyle
        {
            int color = -1;       // 0-15 palette index, or -1 for the theme default
            bool bold = false;
            bool truecolor = false;
            ImU32 rgb = 0;
        };

        // applies the numeric parameters of one "\x1b[ ... m" sequence to the style
        auto apply_sgr(const std::string& text, size_t begin, size_t end, AnsiStyle& style) -> void
        {
            std::vector<int> params;
            int value = 0;

            for (size_t at = begin; at < end; ++at)
            {
                if (text[at] >= '0' && text[at] <= '9')
                    value = value * 10 + (text[at] - '0');
                else if (text[at] == ';')
                {
                    params.push_back(value);
                    value = 0;
                }
            }

            params.push_back(value); // a bare "\x1b[m" collapses to {0}, i.e. reset

            for (size_t index = 0; index < params.size(); ++index)
            {
                const int code = params[index];

                if (code == 0)
                    style = AnsiStyle{};
                else if (code == 1)
                    style.bold = true;
                else if (code == 22)
                    style.bold = false;
                else if (code == 39)
                    style = AnsiStyle{ -1, style.bold, false, 0 };
                else if (code >= 30 && code <= 37)
                    style = AnsiStyle{ code - 30, style.bold, false, 0 };
                else if (code >= 90 && code <= 97)
                    style = AnsiStyle{ code - 90 + 8, style.bold, false, 0 };
                else if ((code == 38 || code == 48) && index + 1 < params.size())
                {
                    // extended colour; 48 (background) is parsed but not applied
                    const int mode = params[index + 1];

                    if (mode == 5 && index + 2 < params.size())
                    {
                        if (code == 38)
                        {
                            style.truecolor = true;
                            style.rgb = xterm_256_color(params[index + 2]);
                        }
                        index += 2;
                    }
                    else if (mode == 2 && index + 4 < params.size())
                    {
                        if (code == 38)
                        {
                            style.truecolor = true;
                            style.rgb = IM_COL32(params[index + 2], params[index + 3],
                                                 params[index + 4], 255);
                        }
                        index += 4;
                    }
                }
            }
        }

        auto ansi_color(const AnsiStyle& style) -> std::optional<ImU32>
        {
            if (style.truecolor)
                return style.rgb;

            if (style.color < 0)
                return std::nullopt; // leave the theme's default text colour in place

            const int index = (style.bold && style.color < 8) ? style.color + 8 : style.color;
            return ANSI_COLORS[index];
        }

        // renders one console line, honouring ansi SGR colour escapes and dropping
        // any other control sequence so it never shows as garbage
        auto draw_ansi_line(const std::string& line) -> void
        {
            AnsiStyle style;
            bool first = true;
            const size_t size = line.size();
            size_t at = 0;

            while (at < size)
            {
                if (line[at] == '\x1b' && at + 1 < size && line[at + 1] == '[')
                {
                    size_t final = at + 2;

                    while (final < size && !(line[final] >= '@' && line[final] <= '~'))
                        ++final;

                    if (final < size && line[final] == 'm')
                        apply_sgr(line, at + 2, final, style);

                    at = final < size ? final + 1 : size;
                    continue;
                }

                size_t run_end = line.find('\x1b', at);
                if (run_end == std::string::npos)
                    run_end = size;

                if (run_end > at)
                {
                    if (!first)
                        ImGui::SameLine(0.0f, 0.0f);

                    first = false;

                    const std::optional<ImU32> color = ansi_color(style);

                    if (color)
                        ImGui::PushStyleColor(ImGuiCol_Text, *color);

                    ImGui::TextUnformatted(line.c_str() + at, line.c_str() + run_end);

                    if (color)
                        ImGui::PopStyleColor();
                }

                at = run_end;
            }

            // a line that was empty or only escapes still takes a row
            if (first)
                ImGui::TextUnformatted("");
        }

        struct SourceNode
        {
            std::string name;
            std::filesystem::path path;
            std::vector<SourceNode> children;
        };

        auto find_or_add_child(std::vector<SourceNode>& nodes, std::string_view name) -> SourceNode&
        {
            const auto existing = std::ranges::find(nodes, name, &SourceNode::name);

            if (existing != nodes.end())
                return *existing;

            nodes.push_back({ std::string(name), {}, {} });
            return nodes.back();
        }

        auto common_directory(std::span<const std::filesystem::path> files) -> std::filesystem::path
        {
            if (files.empty())
                return {};

            std::filesystem::path prefix = files.front().parent_path();

            for (const std::filesystem::path& file : files)
            {
                while (true)
                {
                    if (file.parent_path() == prefix)
                        break;

                    std::error_code error;
                    const std::filesystem::path relative =
                        std::filesystem::relative(file.parent_path(), prefix, error);
                    const std::string text = relative.generic_string();

                    if (!error && (text.empty() || text == "." || !text.starts_with("..")))
                        break;

                    const std::filesystem::path parent = prefix.parent_path();

                    if (parent == prefix)
                        break;

                    prefix = parent;
                }
            }

            return prefix;
        }

        auto build_source_tree(std::span<const std::filesystem::path> files) -> SourceNode
        {
            SourceNode root;
            const std::filesystem::path prefix = common_directory(files);
            root.name = prefix.empty() ? "/" : prefix.filename().string();

            if (root.name.empty())
                root.name = prefix.string();

            for (const std::filesystem::path& file : files)
            {
                std::error_code error;
                std::filesystem::path relative = std::filesystem::relative(file, prefix, error);

                if (error || relative.empty())
                    relative = file.filename();

                SourceNode* current = &root;

                const std::vector<std::filesystem::path> parts(relative.begin(), relative.end());

                for (size_t index = 0; index < parts.size(); ++index)
                {
                    SourceNode& child = find_or_add_child(current->children, parts[index].string());

                    if (index + 1 == parts.size())
                        child.path = file;
                    else
                        current = &child;
                }
            }

            const auto sort_nodes = [](this const auto& self, std::vector<SourceNode>& nodes) -> void
            {
                std::ranges::sort(nodes, [](const SourceNode& left, const SourceNode& right)
                {
                    const bool left_directory = left.path.empty();
                    const bool right_directory = right.path.empty();

                    if (left_directory != right_directory)
                        return left_directory;

                    return left.name < right.name;
                });

                for (SourceNode& node : nodes)
                    self(node.children);
            };

            sort_nodes(root.children);

            return root;
        }

        auto source_node_matches(const SourceNode& node, std::string_view filter) -> bool
        {
            if (filter.empty())
                return true;

            if (!node.path.empty())
                return matches_filter(node.name, filter);

            return std::ranges::any_of(node.children, [&](const SourceNode& child)
            {
                return source_node_matches(child, filter);
            });
        }

        // whether the file lives anywhere under this node, so a folder can tell if
        // it is on the path to the open source and should reveal itself
        auto node_contains(const SourceNode& node, const std::filesystem::path& target) -> bool
        {
            if (!node.path.empty())
                return node.path == target;

            return std::ranges::any_of(node.children, [&](const SourceNode& child)
            {
                return node_contains(child, target);
            });
        }

        auto preferred_source(std::span<const std::filesystem::path> files) -> std::filesystem::path
        {
            const auto named_main = std::ranges::find_if(files, [](const std::filesystem::path& file)
            {
                return file.stem() == "main" && std::filesystem::exists(file);
            });

            if (named_main != files.end())
                return *named_main;

            const auto existing = std::ranges::find_if(files, [](const std::filesystem::path& file)
            {
                return std::filesystem::exists(file);
            });

            return existing != files.end() ? *existing : std::filesystem::path{};
        }

        auto draw_instruction_table(Debugger& debugger,
                                    std::span<const Instruction> instructions,
                                    bool scroll_to_current) -> void
        {
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_NoPadOuterX;

            if (!ImGui::BeginTable("##instructions", 5, flags, ImVec2(0.0f, 0.0f)))
                return;

            ImGui::TableSetupColumn("##gutter",
                                    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
                                    DISASSEMBLY_GUTTER_WIDTH);
            ImGui::TableSetupColumn("address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("mnemonic", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("operands");
            ImGui::TableSetupColumn("comment");
            ImGui::TableHeadersRow();

            const float row_height = ImGui::GetTextLineHeightWithSpacing();

            if (scroll_to_current)
            {
                int scroll_index = 0;

                for (int index = 0; index < static_cast<int>(instructions.size()); ++index)
                {
                    if (instructions[static_cast<size_t>(index)].current)
                    {
                        scroll_index = index;
                        break;
                    }
                }

                ImGui::SetScrollY(static_cast<float>(scroll_index) * row_height -
                                  ImGui::GetWindowHeight() * 0.35f);
            }

            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(instructions.size()), row_height);

            while (clipper.Step())
            {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                {
                    const Instruction& instruction = instructions[static_cast<size_t>(index)];
                    const Breakpoint* breakpoint = breakpoint_at(debugger, instruction);

                    ImGui::TableNextRow();
                    ImGui::PushID(index);

                    if (instruction.current)
                    {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, CURRENT_INSTRUCTION_COLOR);
                    }

                    ImGui::TableNextColumn();

                    const ImVec2 gutter_min = ImGui::GetCursorScreenPos();

                    if (ImGui::InvisibleButton("##gutter", ImVec2(DISASSEMBLY_GUTTER_WIDTH, row_height)))
                        toggle_instruction_breakpoint(debugger, instruction);

                    const bool gutter_hovered = ImGui::IsItemHovered();
                    const ImVec2 marker_center(gutter_min.x + DISASSEMBLY_GUTTER_WIDTH * 0.5f,
                                               gutter_min.y + row_height * 0.5f);

                    if (breakpoint != nullptr)
                    {
                        const ImU32 color = breakpoint->enabled ? BREAKPOINT_COLOR
                                                                : BREAKPOINT_DISABLED_COLOR;

                        if (breakpoint->resolved)
                            draw_list->AddCircleFilled(marker_center, BREAKPOINT_RADIUS, color);
                        else
                            draw_list->AddCircle(marker_center, BREAKPOINT_RADIUS, color, 0, 1.5f);
                    }
                    else if (gutter_hovered)
                    {
                        draw_list->AddCircleFilled(marker_center, BREAKPOINT_RADIUS, BREAKPOINT_HOVER_COLOR);
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%012llx", static_cast<unsigned long long>(instruction.address));

                    if (scroll_to_current && instruction.current)
                        ImGui::SetScrollHereY(0.35f);

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(instruction.mnemonic.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(instruction.operands.c_str());
                    ImGui::TableNextColumn();

                    if (!instruction.comment.empty())
                        ImGui::TextDisabled("; %s", instruction.comment.c_str());

                    ImGui::PopID();
                }
            }

            ImGui::EndTable();
        }

        auto state_color(TargetState state) -> ImVec4
        {
            switch (state)
            {
                case TargetState::Running: return ImVec4(0.45f, 0.78f, 0.45f, 1.0f);
                case TargetState::Stopped: return ImVec4(0.95f, 0.76f, 0.35f, 1.0f);
                case TargetState::Crashed: return ImVec4(0.89f, 0.33f, 0.33f, 1.0f);
                case TargetState::Exited:  return ImVec4(0.60f, 0.60f, 0.65f, 1.0f);
                default:                   return ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
            }
        }

        auto draw_variable(const Variable& variable) -> void
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            if (variable.children.empty())
            {
                ImGui::TreeNodeEx(variable.name.c_str(),
                                  ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                      ImGuiTreeNodeFlags_SpanFullWidth);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(variable.type.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(variable.value.c_str());

                return;
            }

            const bool open = ImGui::TreeNodeEx(variable.name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(variable.type.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(variable.value.c_str());

            if (!open)
                return;

            for (const Variable& child : variable.children)
                draw_variable(child);

            ImGui::TreePop();
        }
    }

    Ui::Ui(Window& window)
        : m_window(&window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        // the scalable default font re-rasterizes at any size; the classic bitmap
        // one only looks clean at 13px and turns to mush when the ui is scaled
        io.Fonts->AddFontDefaultVector();

        // a saved layout beats the built in one, so only build when there is none
        m_layout_built = io.IniFilename != nullptr && std::filesystem::exists(io.IniFilename);

        // sit the settings file next to imgui's own layout file
        m_preferences_path = io.IniFilename != nullptr
                                 ? std::filesystem::path(io.IniFilename).parent_path() / "hsdbg.ini"
                                 : std::filesystem::path("hsdbg.ini");
        m_preferences = load_preferences(m_preferences_path);

        apply_style();

        ImGui_ImplGlfw_InitForOpenGL(m_window->handle(), true);
        ImGui_ImplOpenGL3_Init(GLSL_VERSION);

        Log::info("imgui {}", IMGUI_VERSION);
    }

    Ui::~Ui()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    auto Ui::begin_frame() -> void
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    auto Ui::end_frame() -> void
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (m_mascot_pending && m_mascot_texture != 0)
        {
            if (!m_image_renderer_ready)
                m_image_renderer_ready = m_image_renderer.init();

            if (m_image_renderer_ready)
            {
                const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
                const int width = static_cast<int>(m_window->framebuffer_width());
                const int height = static_cast<int>(m_window->framebuffer_height());

                m_image_renderer.draw(m_mascot_texture,
                                      m_mascot_x0 * scale.x, m_mascot_y0 * scale.y,
                                      m_mascot_x1 * scale.x, m_mascot_y1 * scale.y,
                                      width, height);
            }
        }

        m_mascot_pending = false;
    }

    auto Ui::open_source(const std::filesystem::path& path) -> void
    {
        if (const auto result = m_source_view.open(path); !result)
        {
            Log::error("{}", result.error());
            push_console(std::format("error: {}", result.error()));
        }
    }

    auto Ui::follow_stop(Debugger& debugger) -> void
    {
        if (!debugger.is_stopped())
        {
            if (debugger.is_running())
                m_source_view.set_highlighted_line(0);

            return;
        }

        if (debugger.stop_count() == m_followed_stop)
            return;

        m_followed_stop = debugger.stop_count();

        // wanted even when no frame has source to show
        m_scroll_to_program_counter = true;
        m_scroll_to_symbol = true;

        follow_selected_frame(debugger);
    }

    auto Ui::follow_selected_frame(Debugger& debugger) -> void
    {
        const std::span<const StackFrame> stack = debugger.call_stack();

        // the innermost frames are often runtime internals with no source, and
        // pointing at them tells the user nothing
        const auto frame = std::ranges::find_if(stack, [](const StackFrame& candidate)
        {
            return candidate.line != 0 && !candidate.file.empty();
        });

        if (frame == stack.end())
            return;

        debugger.select_frame(frame->index);
        show_frame(*frame);
    }

    auto Ui::follow_target(Debugger& debugger) -> void
    {
        if (!debugger.has_target())
        {
            m_followed_target.clear();
            return;
        }

        if (debugger.target_path() == m_followed_target)
            return;

        m_followed_target = debugger.target_path();
        m_scroll_to_symbol = true;
        m_scroll_to_program_counter = true;

        if (m_source_view.path().empty())
        {
            if (const std::filesystem::path source = preferred_source(debugger.source_files());
                !source.empty())
            {
                open_source(source);
            }
        }
    }

    auto Ui::show_frame(const StackFrame& frame) -> void
    {
        if (frame.file.empty() || frame.line == 0)
            return;

        if (m_source_view.path() != frame.file)
            open_source(frame.file);

        m_source_view.set_highlighted_line(frame.line);
    }

    auto Ui::draw(Debugger& debugger) -> void
    {
        apply_preferences();

        m_profiler.sample_frame(ImGui::GetIO().DeltaTime);
        m_profiler.sample_target(debugger.resident_memory(), debugger.has_target());

        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        constexpr ImGuiWindowFlags root_flags =
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ROOT_PADDING);

        ImGui::Begin("##hsdbg_root", nullptr, root_flags);

        ImGui::PopStyleVar(3);

        draw_menu_bar(debugger);
        draw_toolbar(debugger);

        follow_stop(debugger);
        follow_target(debugger);

        const ImGuiID dockspace_id = ImGui::GetID("##hsdbg_dockspace");
        const float status_bar_height = ImGui::GetTextLineHeight() +
                                        ImGui::GetStyle().ItemSpacing.y * 3.0f;

        ImGui::DockSpace(dockspace_id,
                         ImVec2(0.0f, ImGui::GetContentRegionAvail().y - status_bar_height),
                         ImGuiDockNodeFlags_PassthruCentralNode);

        if (!m_layout_built)
        {
            build_default_layout(dockspace_id);
            m_layout_built = true;
            m_select_default_tabs = true;
        }

        draw_status_bar(debugger);
        draw_load_target_popup(debugger);

        ImGui::End();

        draw_source_panel(debugger);

        // a click on a highlighted macro in the source view loads it into the
        // macros panel, brings the panel up and jumps its focus there
        if (std::optional<std::string> request = m_source_view.take_macro_request())
        {
            m_macro_input = std::move(*request);
            m_macro_level = 0;
            m_visible.macros = true;
            m_focus_macros = true;
        }

        // right-clicking a name in the source adds it to the watch list
        if (std::optional<std::string> request = m_source_view.take_watch_request())
            add_watch(debugger, std::move(*request));

        // let the profiling views appear on their own the moment work starts:
        // the profiler (which now holds the flame chart, timings and graphs) when
        // something is being sampled, traced or instrumented. the latch means it
        // only springs up on the rising edge, so closing it makes it stay closed
        const bool profiling_active = debugger.sampling_enabled() ||
                                      debugger.instrumentation_active() ||
                                      !debugger.traces().empty();

        if (profiling_active && !m_profiler_revealed)
            m_visible.profiler = true;
        m_profiler_revealed = profiling_active;

        draw_breakpoints_panel(debugger);
        draw_call_stack_panel(debugger);
        draw_source_tree_panel(debugger);
        draw_threads_panel(debugger);
        draw_locals_panel(debugger);
        draw_watch_panel(debugger);
        draw_registers_panel(debugger);
        draw_symbols_panel(debugger);
        draw_disassembly_panel(debugger);
        draw_console_panel(debugger);
        draw_profiler_panel(debugger);
        draw_macros_panel(debugger);

        if (m_visible.demo)
            ImGui::ShowDemoWindow(&m_visible.demo);

        draw_preferences_window();
        draw_command_palette(debugger);

        // a window claims its tab when it is first submitted, so this can only be
        // asked for once every panel in the node exists. within each tabbed node
        // the later focus wins, so raise the tab we want in front last; the very
        // last call also lands the keyboard focus, which belongs on the source
        if (m_select_default_tabs)
        {
            ImGui::SetWindowFocus(PANEL_SYMBOLS);
            ImGui::SetWindowFocus(PANEL_SOURCE_TREE);
            ImGui::SetWindowFocus(PANEL_THREADS);
            ImGui::SetWindowFocus(PANEL_CALL_STACK);
            ImGui::SetWindowFocus(PANEL_WATCH);
            ImGui::SetWindowFocus(PANEL_LOCALS);
            ImGui::SetWindowFocus(PANEL_DISASSEMBLY);
            ImGui::SetWindowFocus(PANEL_BREAKPOINTS);
            ImGui::SetWindowFocus(PANEL_CONSOLE);
            ImGui::SetWindowFocus(PANEL_SOURCE);
            m_select_default_tabs = false;
        }

        if (m_focus_symbols)
        {
            ImGui::SetWindowFocus(PANEL_SYMBOLS);
            m_focus_symbols = false;
        }
        else if (m_focus_disassembly)
        {
            ImGui::SetWindowFocus(PANEL_DISASSEMBLY);
            m_focus_disassembly = false;
        }

        if (m_focus_breakpoints)
        {
            ImGui::SetWindowFocus(PANEL_BREAKPOINTS);
            m_focus_breakpoints = false;
        }

        if (m_focus_macros)
        {
            ImGui::SetWindowFocus(PANEL_MACROS);
            m_focus_macros = false;
        }

        if (m_focus_profiler)
        {
            ImGui::SetWindowFocus(PANEL_PROFILER);
            m_focus_profiler = false;
        }
    }

    auto Ui::build_default_layout(uint32_t dockspace_id) -> void
    {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

        // four regions frame a central code view, each holding one stage of the
        // debugging loop so related panels sit together instead of scattered:
        //   left   navigator - where to go (files, symbols) over where execution
        //                      currently is (call stack, threads)
        //   center code      - source and disassembly, both tracking the pc
        //   right  inspector - the selected frame's state: locals over registers
        //   bottom output    - the console and the auxiliary read-outs
        ImGuiID center_id = dockspace_id;
        const ImGuiID left_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.20f, nullptr, &center_id);
        const ImGuiID right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, nullptr, &center_id);
        const ImGuiID bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.26f, nullptr, &center_id);

        ImGuiID left_top_id = left_id;
        const ImGuiID left_bottom_id = ImGui::DockBuilderSplitNode(left_top_id, ImGuiDir_Down, 0.5f, nullptr, &left_top_id);

        ImGuiID right_top_id = right_id;
        const ImGuiID right_bottom_id = ImGui::DockBuilderSplitNode(right_top_id, ImGuiDir_Down, 0.45f, nullptr, &right_top_id);

        // navigator: where to go, then where execution is
        ImGui::DockBuilderDockWindow(PANEL_SOURCE_TREE, left_top_id);
        ImGui::DockBuilderDockWindow(PANEL_SYMBOLS, left_top_id);
        ImGui::DockBuilderDockWindow(PANEL_CALL_STACK, left_bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_THREADS, left_bottom_id);

        // code
        ImGui::DockBuilderDockWindow(PANEL_SOURCE, center_id);
        ImGui::DockBuilderDockWindow(PANEL_DISASSEMBLY, center_id);

        // inspector: locals and the watch list share the top, the frame's state
        // over the frame's registers
        ImGui::DockBuilderDockWindow(PANEL_LOCALS, right_top_id);
        ImGui::DockBuilderDockWindow(PANEL_WATCH, right_top_id);
        ImGui::DockBuilderDockWindow(PANEL_REGISTERS, right_bottom_id);

        // output drawer
        ImGui::DockBuilderDockWindow(PANEL_CONSOLE, bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_BREAKPOINTS, bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_PROFILER, bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_MACROS, bottom_id);

        ImGui::DockBuilderFinish(dockspace_id);
    }

    auto Ui::draw_menu_bar(Debugger& debugger) -> void
    {
        if (!ImGui::BeginMenuBar())
            return;

        if (ImGui::BeginMenu("file"))
        {
            // a menu is its own popup, so the request has to be handed back to the
            // root window or BeginPopupModal never sees a matching id
            if (ImGui::MenuItem("load target..."))
            {
                m_target_input = debugger.target_path().string();
                m_load_target_pending = true;
            }

            if (ImGui::MenuItem("unload target", nullptr, false, debugger.has_target()))
                debugger.unload_target();

            ImGui::Separator();

            if (ImGui::MenuItem("command palette...", "cmd+k"))
                m_palette_request = true;

            ImGui::MenuItem("preferences...", nullptr, &m_show_preferences);

            ImGui::Separator();

            if (ImGui::MenuItem("quit", "cmd+q"))
                m_window->set_should_close(true);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("run"))
        {
            const bool has_target = debugger.has_target();

            if (ImGui::MenuItem("continue", "f5", false, has_target))
                report(debugger.resume(), "continue");

            if (ImGui::MenuItem("pause", nullptr, false, debugger.is_running()))
                report(debugger.pause(), "pause");

            if (ImGui::MenuItem("stop", nullptr, false, has_target))
                report(debugger.terminate(), "stop");

            ImGui::Separator();

            const StepMode step_mode =
                m_preferences.step_by_instruction ? StepMode::Instruction : StepMode::Line;

            if (ImGui::MenuItem("step over", "f10", false, has_target))
                report(debugger.step_over(step_mode), "step over");

            if (ImGui::MenuItem("step into", "f11", false, has_target))
                report(debugger.step_into(step_mode), "step into");

            if (ImGui::MenuItem("step out", "shift+f11", false, has_target))
                report(debugger.step_out(), "step out");

            ImGui::Separator();

            if (ImGui::MenuItem("clear breakpoints", nullptr, false, !debugger.breakpoints().empty()))
                debugger.clear_breakpoints();

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("view"))
        {
            ImGui::MenuItem(PANEL_SOURCE, nullptr, &m_visible.source);
            ImGui::MenuItem(PANEL_SOURCE_TREE, nullptr, &m_visible.source_tree);
            ImGui::MenuItem(PANEL_BREAKPOINTS, nullptr, &m_visible.breakpoints);
            ImGui::MenuItem(PANEL_CALL_STACK, nullptr, &m_visible.call_stack);
            ImGui::MenuItem(PANEL_THREADS, nullptr, &m_visible.threads);
            ImGui::MenuItem(PANEL_LOCALS, nullptr, &m_visible.locals);
            ImGui::MenuItem(PANEL_WATCH, nullptr, &m_visible.watch);
            ImGui::MenuItem(PANEL_REGISTERS, nullptr, &m_visible.registers);
            ImGui::MenuItem(PANEL_SYMBOLS, nullptr, &m_visible.symbols);
            ImGui::MenuItem(PANEL_DISASSEMBLY, nullptr, &m_visible.disassembly);
            ImGui::MenuItem(PANEL_CONSOLE, nullptr, &m_visible.console);
            ImGui::MenuItem(PANEL_PROFILER, nullptr, &m_visible.profiler);
            ImGui::MenuItem(PANEL_MACROS, nullptr, &m_visible.macros);

            ImGui::Separator();

            if (ImGui::MenuItem("reset layout"))
                m_layout_built = false;

            ImGui::MenuItem("imgui demo", nullptr, &m_visible.demo);

            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    auto Ui::draw_toolbar(Debugger& debugger) -> void
    {
        const bool has_target = debugger.has_target();
        const bool running = debugger.is_running();

        if (!m_mascot_loaded)
        {
            m_mascot_loaded = true;
            m_mascot.load(std::filesystem::path(HSDBG_ASSET_DIR) / "peepocry.gif");
        }

        const float button_height = ImGui::GetFrameHeight();
        const bool has_mascot = m_preferences.show_mascot && m_mascot.valid() &&
                                m_mascot.height() > 0.0f;
        const float mascot_height = has_mascot ? button_height * m_preferences.mascot_scale : 0.0f;
        const float row_height = std::max(button_height, mascot_height);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLBAR_PADDING);
        ImGui::BeginChild("##toolbar", ImVec2(0.0f, row_height + TOOLBAR_PADDING.y * 2.0f));

        const float row_top = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(row_top + (row_height - button_height) * 0.5f);

        const StepMode step_mode =
            m_preferences.step_by_instruction ? StepMode::Instruction : StepMode::Line;

        ImGui::BeginDisabled(!has_target || running);
        if (ImGui::Button("run"))
        {
            LaunchSpec spec;
            spec.executable = debugger.target_path();
            spec.stop_at_entry = m_preferences.stop_at_entry;

            report(debugger.launch(spec), "run");
        }
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(!running);
        if (ImGui::Button("pause"))
            report(debugger.pause(), "pause");
        ImGui::EndDisabled();

        ImGui::SameLine();

        ImGui::BeginDisabled(!has_target);
        if (ImGui::Button("stop"))
            report(debugger.terminate(), "stop");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button("step over"))
            report(debugger.step_over(step_mode), "step over");

        ImGui::SameLine();

        if (ImGui::Button("step into"))
            report(debugger.step_into(step_mode), "step into");

        ImGui::SameLine();

        if (ImGui::Button("step out"))
            report(debugger.step_out(), "step out");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // the way in to profiling, and the way back out: the panel only exists
        // while this is on, and lights up to show that it is. the button toggles
        // that state, so the tint is keyed off a copy taken before the click or
        // the push and pop would not balance
        const bool profiler_open = m_visible.profiler;

        if (profiler_open)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        if (ImGui::Button("profiler"))
        {
            m_visible.profiler = !m_visible.profiler;

            if (m_visible.profiler)
                m_focus_profiler = true;
        }

        if (profiler_open)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(state_color(debugger.state()), "%s", to_string(debugger.state()).data());

        if (has_mascot)
        {
            const ImVec2 size(mascot_height * (m_mascot.width() / m_mascot.height()), mascot_height);

            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - size.x);
            ImGui::SetCursorPosY(row_top);

            // reserve the spot but let imgui draw nothing here; the mascot is
            // rendered by our own shader after imgui, straight to the framebuffer
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(size);

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("it's going to be okay");

            const ImVec2 viewport = ImGui::GetMainViewport()->Pos;
            m_mascot_pending = true;
            m_mascot_texture = m_mascot.texture();
            m_mascot_x0 = origin.x - viewport.x;
            m_mascot_y0 = origin.y - viewport.y;
            m_mascot_x1 = m_mascot_x0 + size.x;
            m_mascot_y1 = m_mascot_y0 + size.y;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }

    auto Ui::draw_status_bar(const Debugger& debugger) -> void
    {
        ImGui::Separator();

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(state_color(debugger.state()), "%s", to_string(debugger.state()).data());

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (debugger.has_target())
            ImGui::Text("%s", debugger.target_path().filename().string().c_str());
        else
            ImGui::TextDisabled("no target");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::Text("%zu breakpoints", debugger.breakpoints().size());

        if (m_preferences.show_fps)
        {
            const std::string frame_stats = std::format("{:.1f} fps", ImGui::GetIO().Framerate);
            const float text_width = ImGui::CalcTextSize(frame_stats.c_str()).x;

            ImGui::SameLine(ImGui::GetContentRegionMax().x - text_width -
                            ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextDisabled("%s", frame_stats.c_str());
        }
    }

    auto Ui::draw_load_target_popup(Debugger& debugger) -> void
    {
        if (m_load_target_pending)
        {
            ImGui::OpenPopup(LOAD_TARGET_POPUP);
            m_load_target_pending = false;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                            viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (!ImGui::BeginPopupModal(LOAD_TARGET_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::TextDisabled("path to an executable to debug");

        ImGui::SetNextItemWidth(420.0f);

        const bool submitted = ImGui::InputText("##target_path",
                                                &m_target_input,
                                                ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::Button("load") || submitted)
        {
            if (const auto result = debugger.load_target(m_target_input); result)
            {
                push_console(std::format("loaded target {}", debugger.target_path().string()));
                ImGui::CloseCurrentPopup();
            }
            else
            {
                push_console(std::format("error: {}", result.error()));
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("cancel"))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    auto Ui::apply_preferences() -> void
    {
        // FontScaleMain re-rasterizes the vector font crisply, unlike the legacy
        // FontGlobalScale which just stretched the baked atlas
        ImGui::GetStyle().FontScaleMain = m_preferences.ui_scale;

        m_source_view.set_highlighting(m_preferences.syntax_highlighting);
        m_source_view.set_line_numbers(m_preferences.show_line_numbers);
        m_source_view.set_highlight_current_line(m_preferences.highlight_current_line);

        const auto pack = [](const float c[3]) {
            return IM_COL32(static_cast<int>(c[0] * 255.0f), static_cast<int>(c[1] * 255.0f),
                            static_cast<int>(c[2] * 255.0f), 255);
        };
        m_source_view.set_syntax_color(1, pack(m_preferences.color_keyword));
        m_source_view.set_syntax_color(2, pack(m_preferences.color_type));
        m_source_view.set_syntax_color(3, pack(m_preferences.color_string));
        m_source_view.set_syntax_color(4, pack(m_preferences.color_number));
        m_source_view.set_syntax_color(5, pack(m_preferences.color_comment));
        m_source_view.set_syntax_color(6, pack(m_preferences.color_preprocessor));
        m_source_view.set_current_line_color(pack(m_preferences.color_current_line));

        // the theme colours and rounding only need rebuilding when they change
        if (m_restyle_pending)
        {
            apply_style();
            m_restyle_pending = false;
        }
    }

    auto Ui::draw_preferences_window() -> void
    {
        if (!m_show_preferences)
            return;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                            viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560.0f, 380.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));

        if (!ImGui::Begin("preferences", &m_show_preferences, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }

        static constexpr const char* CATEGORIES[] = { "appearance", "editor", "debugger" };
        bool changed = false;
        bool restyle = false;

        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

        // left: the category list; right: that category's settings
        ImGui::BeginChild("##pref_categories", ImVec2(150.0f, -footer), ImGuiChildFlags_Borders);
        for (int index = 0; index < IM_ARRAYSIZE(CATEGORIES); ++index)
        {
            if (ImGui::Selectable(CATEGORIES[index], m_preferences_tab == index))
                m_preferences_tab = index;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##pref_content", ImVec2(0.0f, -footer));
        ImGui::PushItemWidth(-150.0f);

        const auto help = [](const char* text) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::BeginItemTooltip())
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
                ImGui::TextUnformatted(text);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        };

        const auto accent_swatch = [&](const char* label, float (&value)[3]) {
            if (ImGui::ColorEdit3(label, value,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
                changed = true;
        };

        if (m_preferences_tab == 0)
        {
            ImGui::SeparatorText("interface");
            changed |= ImGui::SliderFloat("ui scale", &m_preferences.ui_scale, 0.75f, 2.0f, "%.2fx");
            help("scales every font. the text stays crisp because it is re-rasterized, not stretched.");

            if (ImGui::ColorEdit3("accent colour", m_preferences.accent,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
            {
                changed = true;
                restyle = true;
            }

            if (ImGui::SliderFloat("corner rounding", &m_preferences.rounding, 0.0f, 12.0f, "%.0f px"))
            {
                changed = true;
                restyle = true;
            }

            changed |= ImGui::Checkbox("show fps in the status bar", &m_preferences.show_fps);

            ImGui::SeparatorText("layout");
            if (ImGui::Button("reset window layout"))
                m_layout_built = false;
            help("restores the default arrangement of all the docked panels.");

            ImGui::SeparatorText("mascot");
            changed |= ImGui::Checkbox("show the crying pepe", &m_preferences.show_mascot);

            ImGui::BeginDisabled(!m_preferences.show_mascot);
            changed |= ImGui::SliderFloat("pepe size", &m_preferences.mascot_scale, 1.0f, 3.0f, "%.1fx");
            ImGui::EndDisabled();
        }
        else if (m_preferences_tab == 1)
        {
            ImGui::SeparatorText("source view");
            changed |= ImGui::Checkbox("syntax highlighting", &m_preferences.syntax_highlighting);
            changed |= ImGui::Checkbox("show line numbers", &m_preferences.show_line_numbers);
            changed |= ImGui::Checkbox("highlight the current line",
                                       &m_preferences.highlight_current_line);

            ImGui::SeparatorText("colours");
            ImGui::BeginDisabled(!m_preferences.syntax_highlighting);
            accent_swatch("keyword", m_preferences.color_keyword);
            accent_swatch("type", m_preferences.color_type);
            accent_swatch("string", m_preferences.color_string);
            accent_swatch("number", m_preferences.color_number);
            accent_swatch("comment", m_preferences.color_comment);
            accent_swatch("preprocessor", m_preferences.color_preprocessor);
            ImGui::EndDisabled();
            accent_swatch("current line", m_preferences.color_current_line);
        }
        else if (m_preferences_tab == 2)
        {
            ImGui::SeparatorText("launching");
            changed |= ImGui::Checkbox("break at entry point on launch", &m_preferences.stop_at_entry);
            help("stop on the very first instruction instead of running to your breakpoints.");

            ImGui::SeparatorText("stepping");
            changed |= ImGui::Checkbox("step by instruction, not line",
                                       &m_preferences.step_by_instruction);
            help("the step over/into buttons advance one machine instruction at a time.");
        }

        ImGui::PopItemWidth();
        ImGui::EndChild();

        ImGui::Separator();

        if (ImGui::Button("reset to defaults"))
        {
            m_preferences = Preferences{};
            changed = true;
            restyle = true;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("saved to %s", m_preferences_path.filename().string().c_str());

        // persist the moment anything changes, so nothing is lost to a crash
        if (changed)
            save_preferences(m_preferences_path, m_preferences);

        if (restyle)
            m_restyle_pending = true;

        ImGui::End();
    }

    auto Ui::draw_source_panel(Debugger& debugger) -> void
    {
        if (!m_visible.source)
            return;

        if (ImGui::Begin(PANEL_SOURCE, &m_visible.source))
            m_source_view.draw(debugger);

        ImGui::End();
    }

    auto Ui::draw_breakpoints_panel(Debugger& debugger) -> void
    {
        if (!m_visible.breakpoints)
            return;

        if (ImGui::Begin(PANEL_BREAKPOINTS, &m_visible.breakpoints))
        {
            if (debugger.breakpoints().empty())
            {
                ImGui::TextDisabled("no breakpoints, click a gutter in source or disassembly");
            }
            else
            {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable("##breakpoints", 8, flags))
                {
                    ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, 26.0f);
                    ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("location");
                    ImGui::TableSetupColumn("address");
                    ImGui::TableSetupColumn("condition");
                    ImGui::TableSetupColumn("skip", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("hits", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 26.0f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    uint32_t pending_removal = 0;

                    for (const Breakpoint& breakpoint : debugger.breakpoints())
                    {
                        ImGui::PushID(static_cast<int>(breakpoint.id));
                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        bool enabled = breakpoint.enabled;
                        if (ImGui::Checkbox("##enabled", &enabled))
                            debugger.set_breakpoint_enabled(breakpoint.id, enabled);

                        ImGui::TableNextColumn();
                        ImGui::Text("%u", breakpoint.id);

                        ImGui::TableNextColumn();
                        if (breakpoint.by_address)
                        {
                            ImGui::Text("0x%llx",
                                        static_cast<unsigned long long>(breakpoint.file_address));
                        }
                        else if (breakpoint.line != 0)
                        {
                            // clicking the location takes the source view there, so
                            // the breakpoints list doubles as a jump list. plain
                            // text with a click test, not a Selectable: a full-width
                            // Selectable in this stretch-sized column feeds its own
                            // width back into the column solver and lands on NaN
                            const std::string location = std::format(
                                "{}:{}", breakpoint.file.filename().string(), breakpoint.line);

                            ImGui::TextUnformatted(location.c_str());

                            if (ImGui::IsItemHovered())
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                            if (ImGui::IsItemClicked() && !breakpoint.file.empty())
                            {
                                open_source(breakpoint.file);
                                m_source_view.set_highlighted_line(breakpoint.line);
                                ImGui::SetWindowFocus(PANEL_SOURCE);
                            }
                        }
                        else
                        {
                            ImGui::Text("%s()", breakpoint.function.c_str());
                        }

                        if (ImGui::IsItemHovered() && !breakpoint.file.empty())
                            ImGui::SetTooltip("%s", breakpoint.file.string().c_str());

                        ImGui::TableNextColumn();
                        if (breakpoint.resolved)
                        {
                            ImGui::Text("0x%llx", static_cast<unsigned long long>(breakpoint.address));

                            if (ImGui::IsItemHovered() && !breakpoint.function.empty())
                                ImGui::SetTooltip("%s", breakpoint.function.c_str());
                        }
                        else
                        {
                            ImGui::TextDisabled("pending");
                        }

                        // imgui keeps its own buffer while an input is focused, so
                        // these copies only seed it and receive the final text
                        ImGui::TableNextColumn();
                        std::string condition = breakpoint.condition;
                        ImGui::SetNextItemWidth(-1.0f);

                        if (ImGui::InputTextWithHint("##condition",
                                                     "stop when",
                                                     &condition,
                                                     ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            debugger.set_breakpoint_condition(breakpoint.id, condition);
                        }

                        ImGui::TableNextColumn();
                        int ignore_count = static_cast<int>(breakpoint.ignore_count);
                        ImGui::SetNextItemWidth(-1.0f);

                        if (ImGui::InputInt("##skip", &ignore_count, 0, 0,
                                            ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            debugger.set_breakpoint_ignore_count(breakpoint.id,
                                                                 static_cast<uint32_t>(std::max(ignore_count, 0)));
                        }

                        ImGui::TableNextColumn();
                        ImGui::Text("%u", breakpoint.hit_count);

                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("x"))
                            pending_removal = breakpoint.id;

                        ImGui::PopID();
                    }

                    ImGui::EndTable();

                    if (pending_removal != 0)
                        debugger.remove_breakpoint(pending_removal);
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_call_stack_panel(Debugger& debugger) -> void
    {
        if (!m_visible.call_stack)
            return;

        if (ImGui::Begin(PANEL_CALL_STACK, &m_visible.call_stack))
        {
            if (debugger.call_stack().empty())
            {
                ImGui::TextDisabled("no call stack");
            }
            else
            {
                for (const StackFrame& frame : debugger.call_stack())
                {
                    const bool selected = frame.index == debugger.selected_frame();
                    const std::string label = std::format("{:>2}  {}", frame.index, frame.function);

                    if (ImGui::Selectable(label.c_str(), selected))
                    {
                        debugger.select_frame(frame.index);
                        show_frame(frame);

                        m_scroll_to_program_counter = true;
                        m_scroll_to_symbol = true;
                    }

                    if (frame.line == 0)
                        continue;

                    ImGui::SameLine();
                    ImGui::TextDisabled("%s:%u", frame.file.filename().string().c_str(), frame.line);
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_threads_panel(Debugger& debugger) -> void
    {
        if (!m_visible.threads)
            return;

        if (ImGui::Begin(PANEL_THREADS, &m_visible.threads))
        {
            if (debugger.threads().empty())
            {
                ImGui::TextDisabled("no threads");
            }
            else
            {
                for (const Thread& thread : debugger.threads())
                {
                    const bool selected = thread.id == debugger.selected_thread();
                    const std::string label = std::format("{}  {}", thread.id, thread.name);

                    if (ImGui::Selectable(label.c_str(), selected))
                    {
                        debugger.select_thread(thread.id);

                        // land the source, disassembly and inspector on this
                        // thread's own frame instead of leaving them on the last
                        m_scroll_to_program_counter = true;
                        m_scroll_to_symbol = true;
                        follow_selected_frame(debugger);
                    }

                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", to_string(thread.stop_reason).data());
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_source_tree_panel(const Debugger& debugger) -> void
    {
        if (!m_visible.source_tree)
            return;

        if (ImGui::Begin(PANEL_SOURCE_TREE, &m_visible.source_tree))
        {
            const std::span<const std::filesystem::path> files = debugger.source_files();

            if (files.empty())
            {
                ImGui::TextDisabled("no source files");
            }
            else
            {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##source_filter", "filter files", &m_source_filter);

                const SourceNode tree = build_source_tree(files);
                const std::filesystem::path& open = m_source_view.path();

                // folders stay collapsed until they are needed. when the open file
                // changes, the one frame where it differs from what we last showed
                // expands the chain down to it; after that the user is in control
                const bool reveal = open != m_revealed_source;

                ImDrawList* const draw_list = ImGui::GetWindowDrawList();

                const auto draw_node = [&](this const auto& self, const SourceNode& node) -> void
                {
                    if (!source_node_matches(node, m_source_filter))
                        return;

                    if (!node.path.empty())
                    {
                        const bool selected = node.path == open;

                        if (selected)
                            ImGui::PushStyleColor(ImGuiCol_Text, SOURCE_OPEN_FILE_COLOR);

                        const bool clicked = ImGui::Selectable(node.name.c_str(), selected);

                        if (selected)
                            ImGui::PopStyleColor();

                        if (clicked)
                        {
                            open_source(node.path);
                            ImGui::SetWindowFocus(PANEL_SOURCE);
                        }

                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", node.path.string().c_str());

                        return;
                    }

                    constexpr ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                                         ImGuiTreeNodeFlags_OpenOnArrow |
                                                         ImGuiTreeNodeFlags_OpenOnDoubleClick;

                    // a filter opens every folder so its matches are visible; with
                    // no filter, only the chain down to the open file unfurls, and
                    // only on the frame it changed so manual collapses survive
                    if (!m_source_filter.empty())
                        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                    else if (reveal && node_contains(node, open))
                        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

                    ImGui::PushStyleColor(ImGuiCol_Text, SOURCE_FOLDER_COLOR);
                    const bool open_node = ImGui::TreeNodeEx(node.name.c_str(), flags);
                    ImGui::PopStyleColor();

                    if (!open_node)
                        return;

                    // connect this folder to each visible child the way `tree`
                    // does: a vertical spine down the gutter with a horizontal
                    // tick out to every child, the spine ending at the last one.
                    // the glyphs would be tofu in the default font, so the lines
                    // are drawn by hand
                    const float indent = ImGui::GetStyle().IndentSpacing;
                    const float half_row = ImGui::GetTextLineHeight() * 0.5f;
                    const float spine_x = ImGui::GetCursorScreenPos().x - indent * 0.5f;
                    const float top_y = ImGui::GetCursorScreenPos().y;
                    float last_y = top_y;

                    for (const SourceNode& child : node.children)
                    {
                        if (!source_node_matches(child, m_source_filter))
                            continue;

                        const float row_y = ImGui::GetCursorScreenPos().y + half_row;

                        draw_list->AddLine(ImVec2(spine_x, row_y),
                                           ImVec2(spine_x + indent * 0.5f - 3.0f, row_y),
                                           SOURCE_TREE_LINE_COLOR);
                        last_y = row_y;

                        self(child);
                    }

                    draw_list->AddLine(ImVec2(spine_x, top_y),
                                       ImVec2(spine_x, last_y),
                                       SOURCE_TREE_LINE_COLOR);

                    ImGui::TreePop();
                };

                if (tree.children.empty())
                    ImGui::TextDisabled("no source files");
                else
                    draw_node(tree);

                m_revealed_source = open;
            }
        }

        ImGui::End();
    }

    auto Ui::draw_locals_panel(const Debugger& debugger) -> void
    {
        if (!m_visible.locals)
            return;

        if (ImGui::Begin(PANEL_LOCALS, &m_visible.locals))
        {
            if (debugger.locals().empty())
            {
                ImGui::TextDisabled("no locals");
            }
            else
            {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable("##locals", 3, flags))
                {
                    ImGui::TableSetupColumn("name");
                    ImGui::TableSetupColumn("type");
                    ImGui::TableSetupColumn("value");
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    for (const Variable& variable : debugger.locals())
                        draw_variable(variable);

                    ImGui::EndTable();
                }
            }
        }

        ImGui::End();
    }

    auto Ui::add_watch(Debugger& debugger, std::string expression) -> void
    {
        if (expression.empty())
            return;

        if (std::ranges::any_of(m_watches, [&](const Watch& watch)
                                { return watch.expression == expression; }))
        {
            m_visible.watch = true;
            return;
        }

        Watch watch;
        watch.expression = std::move(expression);

        if (debugger.is_stopped())
        {
            if (const auto result = debugger.evaluate(watch.expression))
            {
                watch.value = *result;
                watch.ok = true;
            }
            else
            {
                watch.value = result.error();
            }
        }

        m_watches.push_back(std::move(watch));
        m_visible.watch = true;
    }

    auto Ui::draw_watch_panel(Debugger& debugger) -> void
    {
        if (!m_visible.watch)
            return;

        if (ImGui::Begin(PANEL_WATCH, &m_visible.watch))
        {
            // re-run the expressions whenever the target stops again or the frame
            // the ui is looking at moves, but never every frame: each call jits and
            // runs code in the target
            const bool stopped = debugger.is_stopped();
            const bool moved = debugger.stop_count() != m_watch_stop ||
                               debugger.selected_thread() != m_watch_thread ||
                               debugger.selected_frame() != m_watch_frame;

            if (stopped && (moved || !m_watch_evaluated))
            {
                m_watch_stop = debugger.stop_count();
                m_watch_thread = debugger.selected_thread();
                m_watch_frame = debugger.selected_frame();
                m_watch_evaluated = true;

                for (Watch& watch : m_watches)
                {
                    if (const auto result = debugger.evaluate(watch.expression))
                    {
                        watch.value = *result;
                        watch.ok = true;
                    }
                    else
                    {
                        watch.value = result.error();
                        watch.ok = false;
                    }
                }
            }

            if (!stopped)
                m_watch_evaluated = false;

            ImGui::SetNextItemWidth(-60.0f);
            const bool submitted = ImGui::InputTextWithHint("##watch_input", "expression to watch",
                                                            &m_watch_input,
                                                            ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::SameLine();
            const bool add_clicked = ImGui::Button("watch", ImVec2(-1.0f, 0.0f));

            if ((submitted || add_clicked) && !m_watch_input.empty())
            {
                add_watch(debugger, m_watch_input);
                m_watch_input.clear();
            }

            if (m_watches.empty())
            {
                ImGui::TextDisabled("watch an expression; it re-evaluates on every stop");
            }
            else
            {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable("##watches", 3, flags))
                {
                    ImGui::TableSetupColumn("expression", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                    ImGui::TableSetupColumn("value");
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    size_t remove_index = m_watches.size();

                    for (size_t index = 0; index < m_watches.size(); ++index)
                    {
                        const Watch& watch = m_watches[index];

                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(index));

                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(watch.expression.c_str());

                        ImGui::TableNextColumn();
                        if (!m_watch_evaluated && !stopped)
                            ImGui::TextDisabled("-");
                        else if (watch.ok)
                            ImGui::TextUnformatted(watch.value.c_str());
                        else
                            ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.45f, 1.0f), "%s",
                                               watch.value.c_str());

                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("x"))
                            remove_index = index;

                        ImGui::PopID();
                    }

                    ImGui::EndTable();

                    if (remove_index < m_watches.size())
                        m_watches.erase(m_watches.begin() + static_cast<std::ptrdiff_t>(remove_index));
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_registers_panel(const Debugger& debugger) -> void
    {
        if (!m_visible.registers)
            return;

        if (ImGui::Begin(PANEL_REGISTERS, &m_visible.registers))
        {
            if (debugger.registers().empty())
            {
                ImGui::TextDisabled("no registers");
            }
            else
            {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable("##registers", 2, flags))
                {
                    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("value");
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    for (const Register& entry : debugger.registers())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(entry.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("0x%016llx", static_cast<unsigned long long>(entry.value));
                    }

                    ImGui::EndTable();
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_symbols_panel(Debugger& debugger) -> void
    {
        if (!m_visible.symbols)
            return;

        if (ImGui::Begin(PANEL_SYMBOLS, &m_visible.symbols))
        {
            const std::span<const Symbol> symbols = debugger.symbols();

            if (symbols.empty())
            {
                ImGui::TextDisabled("no symbols");
            }
            else
            {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##symbol_filter", "filter symbols", &m_symbol_filter);

                std::vector<size_t> visible;

                for (size_t index = 0; index < symbols.size(); ++index)
                {
                    if (matches_filter(symbols[index].name, m_symbol_filter))
                        visible.push_back(index);
                }

                ImGui::TextDisabled("%zu / %zu", visible.size(), symbols.size());

                constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_NoPadOuterX;

                if (ImGui::BeginTable("##symbols", 2, flags))
                {
                    ImGui::TableSetupColumn("address", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("name");
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    const float row_height = ImGui::GetTextLineHeightWithSpacing();

                    if (m_scroll_to_symbol)
                    {
                        for (int index = 0; index < static_cast<int>(visible.size()); ++index)
                        {
                            if (symbols[visible[static_cast<size_t>(index)]].file_address ==
                                debugger.selected_symbol())
                            {
                                ImGui::SetScrollY(static_cast<float>(index) * row_height -
                                                  ImGui::GetWindowHeight() * 0.35f);
                                break;
                            }
                        }

                        m_scroll_to_symbol = false;
                    }

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(visible.size()), row_height);

                    while (clipper.Step())
                    {
                        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                        {
                            const Symbol& symbol = symbols[visible[static_cast<size_t>(index)]];
                            const bool selected = symbol.file_address == debugger.selected_symbol();

                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();

                            ImGui::PushID(static_cast<int>(symbol.file_address));

                            if (ImGui::Selectable(std::format("0x{:012x}", symbol.address).c_str(),
                                                  selected,
                                                  ImGuiSelectableFlags_SpanAllColumns))
                            {
                                debugger.select_symbol(symbol.file_address);
                                m_focus_disassembly = true;
                                m_scroll_to_program_counter = true;
                            }

                            if (ImGui::IsItemHovered() && symbol.name.size() > 40)
                                ImGui::SetTooltip("%s", symbol.name.c_str());

                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(symbol.name.c_str());

                            ImGui::PopID();
                        }
                    }

                    ImGui::EndTable();
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_disassembly_panel(Debugger& debugger) -> void
    {
        if (!m_visible.disassembly)
            return;

        if (ImGui::Begin(PANEL_DISASSEMBLY, &m_visible.disassembly))
        {
            const std::span<const Instruction> instructions = debugger.disassembly();

            if (instructions.empty())
            {
                ImGui::TextDisabled("no disassembly");
            }
            else
            {
                if (!debugger.disassembly_name().empty())
                    ImGui::TextUnformatted(debugger.disassembly_name().data());

                draw_instruction_table(debugger, instructions, m_scroll_to_program_counter);
                m_scroll_to_program_counter = false;
            }
        }

        ImGui::End();
    }

    auto Ui::draw_console_panel(Debugger& debugger) -> void
    {
        if (!m_visible.console)
            return;

        if (ImGui::Begin(PANEL_CONSOLE, &m_visible.console))
        {
            const float input_height = ImGui::GetFrameHeightWithSpacing();

            if (ImGui::BeginChild("##console_output", ImVec2(0.0f, -input_height)))
            {
                for (const std::string& line : debugger.console_output())
                    draw_ansi_line(line);

                for (const std::string& line : m_console_lines)
                    draw_ansi_line(line);

                if (m_console_scroll_pending)
                {
                    ImGui::SetScrollHereY(1.0f);
                    m_console_scroll_pending = false;
                }
            }

            ImGui::EndChild();

            ImGui::SetNextItemWidth(-1.0f);

            const bool submitted = ImGui::InputTextWithHint("##console_input",
                                                            "expression to evaluate",
                                                            &m_console_input,
                                                            ImGuiInputTextFlags_EnterReturnsTrue);

            if (submitted && !m_console_input.empty())
            {
                push_console(std::format("> {}", m_console_input));

                if (const auto result = debugger.evaluate(m_console_input); result)
                    push_console(*result);
                else
                    push_console(std::format("error: {}", result.error()));

                m_console_input.clear();
                ImGui::SetKeyboardFocusHere(-1);
            }
        }

        ImGui::End();
    }

    auto Ui::draw_profiler_panel(Debugger& debugger) -> void
    {
        if (!m_visible.profiler)
            return;

        if (ImGui::Begin(PANEL_PROFILER, &m_visible.profiler))
        {
            const bool instrumented = debugger.instrumentation_active();
            const std::span<const FunctionTrace> traces = debugger.traces();

            // compact header: how calls are being gathered. instrumentation, when
            // the target carries it, is exact and needs nothing; otherwise sampling
            // approximates on any binary. either way calls flow into the flame chart
            if (instrumented)
            {
                ImGui::TextDisabled("instrumented target — every function timed automatically");
            }
            else
            {
                bool sampling = debugger.sampling_enabled();
                if (ImGui::Checkbox("sample while running", &sampling))
                    debugger.set_sampling_enabled(sampling);

                ImGui::SameLine();
                ImGui::TextDisabled("(any binary, approximate)");
            }

            // the hero view: the call flame chart, given the lion's share of the
            // panel, with the detail sections collapsed beneath it
            const float flame_height = std::max(ImGui::GetContentRegionAvail().y * 0.6f, 150.0f);

            if (ImGui::BeginChild("##flamegraph", ImVec2(0.0f, flame_height), ImGuiChildFlags_Borders))
                draw_flamegraph(debugger);

            ImGui::EndChild();

            // exact per-function timings, and the box to add another
            if (ImGui::CollapsingHeader("function timings", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SetNextItemWidth(-70.0f);
                const bool submitted = ImGui::InputTextWithHint("##trace_input", "function to time",
                                                                &m_trace_input,
                                                                ImGuiInputTextFlags_EnterReturnsTrue);

                ImGui::SameLine();
                const bool add_clicked = ImGui::Button("trace", ImVec2(-1.0f, 0.0f));

                if ((submitted || add_clicked) && !m_trace_input.empty())
                {
                    debugger.add_trace(m_trace_input);
                    m_trace_input.clear();
                }

                if (traces.empty())
                {
                    ImGui::TextDisabled("name a function above to count and time its calls");
                }
                else if (ImGui::BeginTable("##traces", 6,
                                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_Resizable))
                {
                    ImGui::TableSetupColumn("function");
                    ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthFixed, 52.0f);
                    ImGui::TableSetupColumn("avg", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
                    ImGui::TableHeadersRow();

                    uint32_t remove_id = 0;

                    for (const FunctionTrace& trace : traces)
                    {
                        // milliseconds read easier than the seconds we store
                        const double avg_ms = trace.completed_count > 0
                            ? trace.total_time / static_cast<double>(trace.completed_count) * 1000.0
                            : 0.0;

                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(trace.function.c_str());

                        ImGui::TableNextColumn();
                        ImGui::Text("%llu", static_cast<unsigned long long>(trace.call_count));

                        ImGui::TableNextColumn();
                        if (trace.completed_count > 0)
                            ImGui::Text("%.3f ms", avg_ms);
                        else
                            ImGui::TextDisabled("-");

                        ImGui::TableNextColumn();
                        if (trace.completed_count > 0)
                            ImGui::Text("%.3f ms", trace.min_time * 1000.0);
                        else
                            ImGui::TextDisabled("-");

                        ImGui::TableNextColumn();
                        if (trace.completed_count > 0)
                            ImGui::Text("%.3f ms", trace.max_time * 1000.0);
                        else
                            ImGui::TextDisabled("-");

                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(trace.id));
                        if (ImGui::SmallButton("x"))
                            remove_id = trace.id;
                        ImGui::PopID();
                    }

                    ImGui::EndTable();

                    if (remove_id != 0)
                        debugger.remove_trace(remove_id);
                }
            }

            // the live footprint of the process under debug
            if (debugger.has_target() &&
                ImGui::CollapsingHeader("resident memory", ImGuiTreeNodeFlags_DefaultOpen))
            {
                const TimeSeries& memory = m_profiler.target_memory_mb();

                ImGui::Text("%.1f MB", memory.latest());
                ImGui::SameLine();
                ImGui::TextDisabled("peak %.1f MB", memory.maximum());

                ImGui::PlotLines("##target_memory", memory.values(), memory.count(), memory.offset(),
                                 nullptr, 0.0f, FLT_MAX, ImVec2(-1.0f, 60.0f));
            }

            // hsdbg's own frame cost, tucked away: useful when the ui itself feels
            // slow, not part of profiling the target
            if (ImGui::CollapsingHeader("hsdbg self-timing"))
            {
                bool paused = m_profiler.paused();
                if (ImGui::Checkbox("pause graphs", &paused))
                    m_profiler.set_paused(paused);

                ImGui::SameLine();
                if (ImGui::SmallButton("reset"))
                    m_profiler.reset();

                const TimeSeries& frame_ms = m_profiler.frame_times();
                const TimeSeries& fps = m_profiler.frame_rates();

                const std::string frame_overlay =
                    std::format("{:.2f} ms  (avg {:.2f}  peak {:.2f})",
                                frame_ms.latest(), frame_ms.average(), frame_ms.maximum());

                ImGui::TextUnformatted("frame time");
                ImGui::PlotLines("##frame_ms", frame_ms.values(), frame_ms.count(), frame_ms.offset(),
                                 frame_overlay.c_str(), 0.0f, FLT_MAX, ImVec2(-1.0f, 70.0f));

                const std::string fps_overlay =
                    std::format("{:.0f} fps  (avg {:.0f})", fps.latest(), fps.average());

                ImGui::TextUnformatted("frame rate");
                ImGui::PlotLines("##fps", fps.values(), fps.count(), fps.offset(),
                                 fps_overlay.c_str(), 0.0f, FLT_MAX, ImVec2(-1.0f, 70.0f));
            }
        }

        ImGui::End();
    }

    auto Ui::draw_flamegraph(const Debugger& debugger) -> void
    {
        const std::span<const TimelineSpan> spans = debugger.timeline();

        if (spans.empty())
        {
            if (debugger.instrumentation_active())
                ImGui::TextDisabled("instrumented target — run it to lay out its calls here");
            else if (debugger.sampling_enabled())
                ImGui::TextDisabled("sampling — run the target and the calls will appear here");
            else
                ImGui::TextDisabled("turn on sampling, or trace a function below, then run");

            return;
        }

        // resolve a trace id to its function name for labels and tooltips
        const auto name_of = [&](uint32_t trace_id) -> const char*
        {
            return debugger.span_label(trace_id);
        };

        // the time range to fit and how tall the call stack gets
        double t_min = spans.front().start;
        double t_max = t_min;
        uint32_t max_depth = 0;

        for (const TimelineSpan& span : spans)
        {
            t_min = std::min(t_min, span.start);
            t_max = std::max(t_max, span.start + span.duration);
            max_depth = std::max(max_depth, span.depth);
        }

        const double range = std::max(t_max - t_min, 1.0e-6);

        ImGui::Text("%.3f ms total   %zu calls", range * 1000.0, spans.size());

        constexpr float row_height = 20.0f;
        const float rows = static_cast<float>(max_depth + 1);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();

        const float canvas_w = std::max(avail.x, 1.0f);
        const float canvas_h = std::max(avail.y, rows * row_height + 4.0f);

        // claim the region so hovering resolves against it
        ImGui::InvisibleButton("##flame_canvas", ImVec2(canvas_w, canvas_h));
        const bool canvas_hovered = ImGui::IsItemHovered();

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->PushClipRect(origin, ImVec2(origin.x + canvas_w, origin.y + canvas_h), true);

        const float scale = canvas_w / static_cast<float>(range); // pixels per second
        const float baseline = origin.y + canvas_h;               // row 0 rests on the bottom

        const ImVec2 mouse = ImGui::GetMousePos();

        for (const TimelineSpan& span : spans)
        {
            const float x0 = origin.x + static_cast<float>((span.start - t_min) * scale);
            const float width = std::max(1.0f, static_cast<float>(span.duration * scale));
            const float y1 = baseline - static_cast<float>(span.depth) * row_height;
            const float y0 = y1 - (row_height - 2.0f);

            const ImU32 fill = ImColor::HSV(span.trace_id * 0.13f, 0.55f, 0.78f);

            draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + width, y1), fill, 2.0f);
            draw_list->AddRect(ImVec2(x0, y0), ImVec2(x0 + width, y1), IM_COL32(0, 0, 0, 90), 2.0f);

            if (width > 24.0f)
            {
                draw_list->PushClipRect(ImVec2(x0 + 2.0f, y0), ImVec2(x0 + width - 2.0f, y1), true);
                draw_list->AddText(ImVec2(x0 + 4.0f, y0 + 2.0f), IM_COL32(20, 20, 20, 255),
                              name_of(span.trace_id));
                draw_list->PopClipRect();
            }

            const bool over = canvas_hovered && mouse.x >= x0 && mouse.x <= x0 + width &&
                              mouse.y >= y0 && mouse.y <= y1;

            if (over)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(name_of(span.trace_id));
                ImGui::Text("start %.3f ms", (span.start - t_min) * 1000.0);
                if (span.duration > 0.0)
                    ImGui::Text("duration %.3f ms", span.duration * 1000.0);
                else
                    ImGui::TextDisabled("running...");
                ImGui::EndTooltip();
            }
        }

        draw_list->PopClipRect();
    }

    auto Ui::draw_macros_panel(Debugger& /*debugger*/) -> void
    {
        if (!m_visible.macros)
            return;

        if (ImGui::Begin(PANEL_MACROS, &m_visible.macros))
        {
            const MacroTable& table = m_source_view.macros();

            ImGui::TextDisabled("unroll a #define one layer per level");

            ImGui::SetNextItemWidth(-FLT_MIN);

            if (ImGui::InputTextWithHint("##macro_input",
                                         "click a macro in source, or type one like MAX(a, b)",
                                         &m_macro_input))
                m_macro_level = 0;

            if (table.empty())
                ImGui::TextDisabled("no #define macros found in the open source file");

            if (m_macro_input.empty())
            {
                ImGui::End();
                return;
            }

            const MacroExpansion expansion = expand_stages(table, m_macro_input);
            const int max_level = static_cast<int>(expansion.levels.size()) - 1;

            m_macro_level = std::clamp(m_macro_level, 0, max_level);

            ImGui::BeginDisabled(m_macro_level <= 0);
            if (ImGui::ArrowButton("##macro_prev", ImGuiDir_Left))
                --m_macro_level;
            ImGui::EndDisabled();

            ImGui::SameLine();

            ImGui::BeginDisabled(m_macro_level >= max_level);
            if (ImGui::ArrowButton("##macro_next", ImGuiDir_Right))
                ++m_macro_level;
            ImGui::EndDisabled();

            ImGui::SameLine();

            if (max_level > 0)
            {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                                        ImGui::CalcTextSize("full reset").x -
                                        ImGui::GetStyle().FramePadding.x * 6.0f -
                                        ImGui::GetStyle().ItemSpacing.x * 2.0f);
                ImGui::SliderInt("##macro_level", &m_macro_level, 0, max_level, "level %d");
            }
            else
            {
                ImGui::TextDisabled("nothing to expand here");
                ImGui::SameLine();
            }

            ImGui::SameLine();

            if (ImGui::SmallButton("full"))
                m_macro_level = max_level;

            ImGui::SameLine();

            if (ImGui::SmallButton("reset"))
                m_macro_level = 0;

            // status: where we are, and whether the tail is truly the fixpoint
            if (max_level == 0)
                ImGui::TextDisabled("already fully expanded");
            else if (m_macro_level == max_level && expansion.fully_expanded())
                ImGui::TextDisabled("level %d of %d — fully expanded", m_macro_level, max_level);
            else if (m_macro_level == max_level)
                ImGui::TextDisabled("level %d — stopped at the expansion cap", m_macro_level);
            else
                ImGui::TextDisabled("level %d of %d", m_macro_level, max_level);

            // which macros the next layer will unroll, so the step reads ahead
            if (m_macro_level < max_level)
            {
                const std::vector<std::string>& next =
                    expansion.expanded[static_cast<size_t>(m_macro_level) + 1];

                if (!next.empty())
                {
                    std::string names;

                    for (const std::string& name : next)
                    {
                        if (!names.empty())
                            names += ", ";

                        names += name;
                    }

                    ImGui::SameLine();
                    ImGui::TextDisabled("| next: %s", names.c_str());
                }
            }

            ImGui::Separator();

            m_macro_output = expansion.levels[static_cast<size_t>(m_macro_level)];

            ImGui::InputTextMultiline("##macro_output", &m_macro_output,
                                      ImGui::GetContentRegionAvail(),
                                      ImGuiInputTextFlags_ReadOnly);
        }

        ImGui::End();
    }

    auto Ui::draw_command_palette(Debugger& debugger) -> void
    {
        // cmd/ctrl+k anywhere, or the menu item, opens it; opening resets the query
        // so it always starts clean and asks for the text field's focus next frame
        const bool open_requested =
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K) || m_palette_request;

        m_palette_request = false;

        if (open_requested)
        {
            m_palette_open = true;
            m_palette_focus = true;
            m_palette_selection = 0;
            m_palette_query.clear();
            ImGui::OpenPopup("##command_palette");
        }

        if (!m_palette_open)
            return;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 size(std::min(viewport->WorkSize.x - 80.0f, 640.0f), 0.0f);

        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                       viewport->WorkPos.y + 90.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(size);

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

        if (!ImGui::BeginPopup("##command_palette", flags))
        {
            // esc or a click outside closed it
            m_palette_open = false;
            return;
        }

        // every command the palette can run; only the ones that make sense for the
        // current state are offered, so the list never dangles a dead action
        enum class Kind : uint8_t { Command, File, Symbol };
        enum Command : uint8_t
        {
            Run, Continue, Pause, Stop, StepOver, StepInto, StepOut, ToggleProfiler
        };

        struct Entry
        {
            std::string label;
            const char* hint = "";
            Kind kind = Kind::Command;
            Command command = Run;
            std::filesystem::path file;
            uint64_t symbol = 0;
        };

        const bool has_target = debugger.has_target();
        const bool running = debugger.is_running();
        const bool stopped = debugger.is_stopped();

        std::vector<Entry> entries;

        const auto add_command = [&](bool enabled, Command command, const char* label)
        {
            if (enabled && matches_filter(label, m_palette_query))
                entries.push_back({ label, "command", Kind::Command, command, {}, 0 });
        };

        add_command(has_target && !running, Run, "run");
        add_command(stopped, Continue, "continue");
        add_command(running, Pause, "pause");
        add_command(has_target, Stop, "stop");
        add_command(stopped, StepOver, "step over");
        add_command(stopped, StepInto, "step into");
        add_command(stopped, StepOut, "step out");
        add_command(true, ToggleProfiler, m_visible.profiler ? "hide profiler" : "show profiler");

        // files and symbols only clutter the list once there is a query to match
        if (!m_palette_query.empty())
        {
            for (const std::filesystem::path& file : debugger.source_files())
            {
                if (entries.size() >= 60)
                    break;

                if (matches_filter(file.filename().string(), m_palette_query))
                    entries.push_back({ file.filename().string(), "file", Kind::File, Run, file, 0 });
            }

            for (const Symbol& symbol : debugger.symbols())
            {
                if (entries.size() >= 120)
                    break;

                if (matches_filter(symbol.name, m_palette_query))
                    entries.push_back({ symbol.name, "symbol", Kind::Symbol, Run, {}, symbol.file_address });
            }
        }

        if (entries.empty())
            m_palette_selection = 0;
        else
            m_palette_selection = std::clamp(m_palette_selection, 0, static_cast<int>(entries.size()) - 1);

        // arrow keys walk the results; the input field ignores up/down itself, so
        // reading them here does not fight the cursor
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !entries.empty())
            m_palette_selection = (m_palette_selection + 1) % static_cast<int>(entries.size());

        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !entries.empty())
        {
            m_palette_selection = (m_palette_selection - 1 + static_cast<int>(entries.size())) %
                                  static_cast<int>(entries.size());
        }

        const auto activate = [&](const Entry& entry)
        {
            switch (entry.kind)
            {
                case Kind::File:
                    open_source(entry.file);
                    ImGui::SetWindowFocus(PANEL_SOURCE);
                    break;

                case Kind::Symbol:
                    if (debugger.select_symbol(entry.symbol))
                    {
                        m_focus_disassembly = true;
                        m_scroll_to_program_counter = true;
                    }
                    break;

                case Kind::Command:
                    switch (entry.command)
                    {
                        case Run:
                        {
                            LaunchSpec spec;
                            spec.executable = debugger.target_path();
                            spec.stop_at_entry = m_preferences.stop_at_entry;
                            report(debugger.launch(spec), "run");
                            break;
                        }
                        case Continue:  report(debugger.resume(), "continue");   break;
                        case Pause:     report(debugger.pause(), "pause");       break;
                        case Stop:      report(debugger.terminate(), "stop");    break;
                        case StepOver:  report(debugger.step_over(), "step over"); break;
                        case StepInto:  report(debugger.step_into(), "step into"); break;
                        case StepOut:   report(debugger.step_out(), "step out");  break;
                        case ToggleProfiler:
                            m_visible.profiler = !m_visible.profiler;
                            if (m_visible.profiler)
                                m_focus_profiler = true;
                            break;
                    }
                    break;
            }

            m_palette_open = false;
            ImGui::CloseCurrentPopup();
        };

        if (m_palette_focus)
        {
            ImGui::SetKeyboardFocusHere();
            m_palette_focus = false;
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool submitted = ImGui::InputTextWithHint("##palette_query",
                                                        "jump to a file or symbol, or run a command",
                                                        &m_palette_query,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);

        if (submitted && !entries.empty())
        {
            activate(entries[static_cast<size_t>(m_palette_selection)]);
            ImGui::EndPopup();
            return;
        }

        ImGui::Separator();

        if (entries.empty())
        {
            ImGui::TextDisabled("no matches");
        }
        else
        {
            const float row = ImGui::GetTextLineHeightWithSpacing();
            ImGui::BeginChild("##palette_results", ImVec2(0.0f, std::min(row * 10.0f + 4.0f,
                                                                         row * static_cast<float>(entries.size()) + 4.0f)));

            for (int index = 0; index < static_cast<int>(entries.size()); ++index)
            {
                const Entry& entry = entries[static_cast<size_t>(index)];
                const bool selected = index == m_palette_selection;

                ImGui::PushID(index);

                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns))
                    activate(entry);

                // keep the keyboard-picked row in view as it moves
                if (selected && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                                 ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsWindowAppearing()))
                    ImGui::SetScrollHereY(0.5f);

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(entry.label.c_str());

                const float hint_width = ImGui::CalcTextSize(entry.hint).x;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - hint_width);
                ImGui::TextDisabled("%s", entry.hint);

                ImGui::PopID();
            }

            ImGui::EndChild();
        }

        ImGui::EndPopup();
    }

    auto Ui::push_console(std::string line) -> void
    {
        m_console_lines.push_back(std::move(line));

        if (m_console_lines.size() > MAX_CONSOLE_LINES)
            m_console_lines.erase(m_console_lines.begin());

        m_console_scroll_pending = true;
    }

    auto Ui::report(const Result<void>& result, std::string_view action) -> void
    {
        if (result)
            return;

        push_console(std::format("{} failed: {}", action, result.error()));
    }

    auto Ui::apply_style() -> void
    {
        ImGuiStyle& style = ImGui::GetStyle();

        const float radius = m_preferences.rounding;

        style.WindowRounding = radius;
        style.ChildRounding = radius;
        style.FrameRounding = radius;
        style.PopupRounding = radius;
        style.ScrollbarRounding = radius + 2.0f;
        style.GrabRounding = radius;
        style.TabRounding = radius;

        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 5.0f);
        style.ScrollbarSize = 11.0f;
        style.GrabMinSize = 9.0f;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.SeparatorTextBorderSize = 1.0f;

        // the accent is user-chosen; the hover/active shades are blended from it
        // toward the button base so any colour stays coherent across the theme
        const ImVec4 accent(m_preferences.accent[0], m_preferences.accent[1],
                            m_preferences.accent[2], 1.0f);
        const ImVec4 base(0.18f, 0.18f, 0.22f, 1.0f);

        const auto mix = [](ImVec4 from, ImVec4 to, float t) {
            return ImVec4(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                          from.z + (to.z - from.z) * t, 1.0f);
        };
        const auto fade = [](ImVec4 colour, float alpha) {
            return ImVec4(colour.x, colour.y, colour.z, alpha);
        };

        const ImVec4 accent_bright = mix(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.14f);
        const ImVec4 accent_hover = mix(base, accent, 0.42f);
        const ImVec4 accent_active = mix(base, accent, 0.68f);

        ImVec4* colors = style.Colors;

        colors[ImGuiCol_Text] = ImVec4(0.86f, 0.86f, 0.88f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.48f, 1.00f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.26f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.31f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.24f, 0.29f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.36f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.36f, 0.43f, 1.00f);
        colors[ImGuiCol_CheckMark] = accent;
        colors[ImGuiCol_SliderGrab] = accent;
        colors[ImGuiCol_SliderGrabActive] = accent_bright;
        colors[ImGuiCol_Button] = base;
        colors[ImGuiCol_ButtonHovered] = accent_hover;
        colors[ImGuiCol_ButtonActive] = accent_active;
        colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
        colors[ImGuiCol_HeaderHovered] = accent_hover;
        colors[ImGuiCol_HeaderActive] = accent_active;
        colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_SeparatorHovered] = fade(accent, 0.60f);
        colors[ImGuiCol_SeparatorActive] = accent;
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.24f, 0.24f, 0.29f, 1.00f);
        colors[ImGuiCol_ResizeGripHovered] = fade(accent, 0.60f);
        colors[ImGuiCol_ResizeGripActive] = accent;
        colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
        colors[ImGuiCol_TabHovered] = accent_hover;
        colors[ImGuiCol_TabSelected] = mix(base, accent, 0.30f);
        colors[ImGuiCol_TabSelectedOverline] = accent;
        colors[ImGuiCol_TabDimmed] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = mix(ImVec4(0.14f, 0.14f, 0.17f, 1.0f), accent, 0.16f);
        colors[ImGuiCol_DockingPreview] = fade(accent, 0.50f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
        colors[ImGuiCol_TextSelectedBg] = fade(accent, 0.35f);
        colors[ImGuiCol_NavCursor] = accent;
    }
}
