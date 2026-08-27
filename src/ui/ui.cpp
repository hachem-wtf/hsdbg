#include "ui/ui.h"

#include "core/log.h"
#include "core/window.h"
#include "debugger/debugger.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <filesystem>
#include <format>

namespace Hsdbg
{
    namespace
    {
        constexpr const char* PANEL_SOURCE = "source";
        constexpr const char* PANEL_BREAKPOINTS = "breakpoints";
        constexpr const char* PANEL_CALL_STACK = "call stack";
        constexpr const char* PANEL_THREADS = "threads";
        constexpr const char* PANEL_LOCALS = "locals";
        constexpr const char* PANEL_REGISTERS = "registers";
        constexpr const char* PANEL_CONSOLE = "console";

        constexpr const char* LOAD_TARGET_POPUP = "load target";

        // macos gives us a 4.1 core context, which speaks glsl 1.50
        // #portability
        constexpr const char* GLSL_VERSION = "#version 150";

        constexpr size_t MAX_CONSOLE_LINES = 2048;

        // breathing room between the window edge and everything docked inside it
        const ImVec2 ROOT_PADDING(8.0f, 6.0f);
        const ImVec2 TOOLBAR_PADDING(4.0f, 4.0f);

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

        // a saved layout beats the built in one, so only build when there is none
        m_layout_built = io.IniFilename != nullptr && std::filesystem::exists(io.IniFilename);

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
    }

    auto Ui::open_source(const std::filesystem::path& path) -> void
    {
        if (const auto result = m_source_view.open(path); !result)
        {
            Log::error("{}", result.error());
            push_console(std::format("error: {}", result.error()));
        }
    }

    auto Ui::draw(Debugger& debugger) -> void
    {
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
        }

        draw_status_bar(debugger);
        draw_load_target_popup(debugger);

        ImGui::End();

        draw_source_panel(debugger);
        draw_breakpoints_panel(debugger);
        draw_call_stack_panel(debugger);
        draw_threads_panel(debugger);
        draw_locals_panel(debugger);
        draw_registers_panel(debugger);
        draw_console_panel(debugger);

        if (m_visible.demo)
            ImGui::ShowDemoWindow(&m_visible.demo);
    }

    auto Ui::build_default_layout(uint32_t dockspace_id) -> void
    {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

        ImGuiID center_id = dockspace_id;
        const ImGuiID left_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.20f, nullptr, &center_id);
        const ImGuiID right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.28f, nullptr, &center_id);
        const ImGuiID bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.28f, nullptr, &center_id);

        ImGuiID left_top_id = left_id;
        const ImGuiID left_bottom_id = ImGui::DockBuilderSplitNode(left_top_id, ImGuiDir_Down, 0.5f, nullptr, &left_top_id);

        ImGuiID right_top_id = right_id;
        const ImGuiID right_bottom_id = ImGui::DockBuilderSplitNode(right_top_id, ImGuiDir_Down, 0.5f, nullptr, &right_top_id);

        ImGui::DockBuilderDockWindow(PANEL_SOURCE, center_id);
        ImGui::DockBuilderDockWindow(PANEL_THREADS, left_top_id);
        ImGui::DockBuilderDockWindow(PANEL_CALL_STACK, left_bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_LOCALS, right_top_id);
        ImGui::DockBuilderDockWindow(PANEL_REGISTERS, right_bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_BREAKPOINTS, bottom_id);
        ImGui::DockBuilderDockWindow(PANEL_CONSOLE, bottom_id);

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

            if (ImGui::MenuItem("step over", "f10", false, has_target))
                report(debugger.step_over(), "step over");

            if (ImGui::MenuItem("step into", "f11", false, has_target))
                report(debugger.step_into(), "step into");

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
            ImGui::MenuItem(PANEL_BREAKPOINTS, nullptr, &m_visible.breakpoints);
            ImGui::MenuItem(PANEL_CALL_STACK, nullptr, &m_visible.call_stack);
            ImGui::MenuItem(PANEL_THREADS, nullptr, &m_visible.threads);
            ImGui::MenuItem(PANEL_LOCALS, nullptr, &m_visible.locals);
            ImGui::MenuItem(PANEL_REGISTERS, nullptr, &m_visible.registers);
            ImGui::MenuItem(PANEL_CONSOLE, nullptr, &m_visible.console);

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

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLBAR_PADDING);
        ImGui::BeginChild("##toolbar",
                          ImVec2(0.0f, ImGui::GetFrameHeight() + TOOLBAR_PADDING.y * 2.0f));

        ImGui::BeginDisabled(!has_target || running);
        if (ImGui::Button("run"))
        {
            LaunchSpec spec;
            spec.executable = debugger.target_path();

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
            report(debugger.step_over(), "step over");

        ImGui::SameLine();

        if (ImGui::Button("step into"))
            report(debugger.step_into(), "step into");

        ImGui::SameLine();

        if (ImGui::Button("step out"))
            report(debugger.step_out(), "step out");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(state_color(debugger.state()), "%s", to_string(debugger.state()).data());

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }

    auto Ui::draw_status_bar(Debugger& debugger) -> void
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

        const std::string frame_stats = std::format("{:.1f} fps", ImGui::GetIO().Framerate);
        const float text_width = ImGui::CalcTextSize(frame_stats.c_str()).x;

        ImGui::SameLine(ImGui::GetContentRegionMax().x - text_width - ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextDisabled("%s", frame_stats.c_str());
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
                ImGui::TextDisabled("no breakpoints, click a line gutter in the source panel");
            }
            else
            {
                constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                  ImGuiTableFlags_SizingStretchProp |
                                                  ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable("##breakpoints", 5, flags))
                {
                    ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, 26.0f);
                    ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("location");
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
                        if (breakpoint.line != 0)
                        {
                            ImGui::Text("%s:%u",
                                        breakpoint.file.filename().string().c_str(),
                                        breakpoint.line);
                        }
                        else
                        {
                            ImGui::Text("%s()", breakpoint.function.c_str());
                        }

                        if (ImGui::IsItemHovered() && !breakpoint.file.empty())
                            ImGui::SetTooltip("%s", breakpoint.file.string().c_str());

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
                        debugger.select_frame(frame.index);

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
                        debugger.select_thread(thread.id);

                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", to_string(thread.stop_reason).data());
                }
            }
        }

        ImGui::End();
    }

    auto Ui::draw_locals_panel(Debugger& debugger) -> void
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

    auto Ui::draw_registers_panel(Debugger& debugger) -> void
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
                    ImGui::TextUnformatted(line.c_str());

                for (const std::string& line : m_console_lines)
                    ImGui::TextUnformatted(line.c_str());

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

        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 4.0f;

        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 5.0f);
        style.ScrollbarSize = 11.0f;
        style.GrabMinSize = 9.0f;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.SeparatorTextBorderSize = 1.0f;

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
        colors[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.72f, 0.78f, 1.00f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.72f, 0.78f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.38f, 0.80f, 0.86f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.40f, 0.44f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.52f, 0.58f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.40f, 0.44f, 1.00f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.52f, 0.58f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.30f, 0.72f, 0.78f, 0.60f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.30f, 0.72f, 0.78f, 1.00f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.24f, 0.24f, 0.29f, 1.00f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.72f, 0.78f, 0.60f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.30f, 0.72f, 0.78f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.40f, 0.44f, 1.00f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.30f, 0.34f, 1.00f);
        colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.30f, 0.72f, 0.78f, 1.00f);
        colors[ImGuiCol_TabDimmed] = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.14f, 0.20f, 0.22f, 1.00f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.30f, 0.72f, 0.78f, 0.50f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.30f, 0.72f, 0.78f, 0.35f);
        colors[ImGuiCol_NavCursor] = ImVec4(0.30f, 0.72f, 0.78f, 1.00f);
    }
}
