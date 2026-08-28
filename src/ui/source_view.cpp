#include "ui/source_view.h"

#include "core/log.h"
#include "debugger/debugger.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <fstream>

namespace Hsdbg
{
    namespace
    {
        constexpr float BREAKPOINT_RADIUS = 5.0f;

        // the dot sits in the gutter with this much room on either side, and the
        // line numbers pick the same gap up again so the column reads evenly
        constexpr float GUTTER_MARGIN = 6.0f;
        constexpr float GUTTER_WIDTH = BREAKPOINT_RADIUS * 2.0f + GUTTER_MARGIN * 2.0f;

        const ImU32 BREAKPOINT_COLOR = IM_COL32(226, 84, 84, 255);
        const ImU32 BREAKPOINT_DISABLED_COLOR = IM_COL32(120, 90, 90, 255);
        const ImU32 BREAKPOINT_HOVER_COLOR = IM_COL32(226, 84, 84, 90);
        const ImU32 HIGHLIGHT_COLOR = IM_COL32(58, 72, 46, 255);

        auto line_number_width(size_t line_count) -> float
        {
            return ImGui::CalcTextSize(std::to_string(line_count).c_str()).x;
        }
    }

    auto SourceView::open(const std::filesystem::path& path) -> Result<void>
    {
        std::ifstream file(path);
        if (!file.is_open())
            return fail("could not open '{}'", path.string());

        std::vector<std::string> lines;
        std::string line;

        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            lines.push_back(std::move(line));
        }

        m_path = path;
        m_lines = std::move(lines);
        m_path_input = path.string();
        m_error.clear();
        m_highlighted_line = 0;

        Log::info("source: opened '{}' ({} lines)", m_path.string(), m_lines.size());

        return {};
    }

    auto SourceView::close() -> void
    {
        m_path.clear();
        m_lines.clear();
        m_error.clear();
        m_highlighted_line = 0;
    }

    auto SourceView::set_highlighted_line(uint32_t line) -> void
    {
        m_highlighted_line = line;
        m_scroll_to_highlight = line != 0;
    }

    auto SourceView::draw(Debugger& debugger) -> void
    {
        draw_open_bar();

        ImGui::Separator();

        if (m_lines.empty())
        {
            ImGui::TextDisabled("no source file open");
            return;
        }

        draw_lines(debugger);
    }

    auto SourceView::draw_open_bar() -> void
    {
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("open").x - ImGui::GetStyle().FramePadding.x * 4.0f);

        const bool submitted = ImGui::InputTextWithHint("##source_path",
                                                        "path to a source file",
                                                        &m_path_input,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine();

        if (ImGui::Button("open") || submitted)
        {
            if (const auto result = open(m_path_input); !result)
            {
                m_error = result.error();
                Log::error("{}", m_error);
            }
        }

        if (!m_error.empty())
            ImGui::TextColored(ImVec4(0.89f, 0.33f, 0.33f, 1.0f), "%s", m_error.c_str());
    }

    auto SourceView::draw_lines(Debugger& debugger) -> void
    {
        const float text_height = ImGui::GetTextLineHeightWithSpacing();
        const float number_width = line_number_width(m_lines.size());

        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        if (!ImGui::BeginChild("##source_lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
        {
            ImGui::EndChild();
            return;
        }

        if (m_scroll_to_highlight && m_highlighted_line != 0)
        {
            ImGui::SetScrollY(static_cast<float>(m_highlighted_line - 1) * text_height -
                              ImGui::GetWindowHeight() * 0.4f);
            m_scroll_to_highlight = false;
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_lines.size()), text_height);

        while (clipper.Step())
        {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const uint32_t line_number = static_cast<uint32_t>(index) + 1;
                const ImVec2 row_start = ImGui::GetCursorScreenPos();

                if (line_number == m_highlighted_line)
                {
                    draw_list->AddRectFilled(row_start,
                                             ImVec2(row_start.x + ImGui::GetContentRegionAvail().x,
                                                    row_start.y + text_height),
                                             HIGHLIGHT_COLOR);
                }

                ImGui::PushID(index);

                if (ImGui::InvisibleButton("##gutter", ImVec2(GUTTER_WIDTH, text_height)))
                {
                    const Breakpoint* existing = nullptr;

                    for (const Breakpoint& breakpoint : debugger.breakpoints())
                    {
                        if (breakpoint.file == m_path && breakpoint.line == line_number)
                        {
                            existing = &breakpoint;
                            break;
                        }
                    }

                    if (existing != nullptr)
                        debugger.remove_breakpoint(existing->id);
                    else
                        debugger.add_breakpoint(m_path, line_number);
                }

                const bool gutter_hovered = ImGui::IsItemHovered();

                ImGui::PopID();

                const Breakpoint* breakpoint = nullptr;

                for (const Breakpoint& candidate : debugger.breakpoints())
                {
                    if (candidate.file == m_path && candidate.line == line_number)
                    {
                        breakpoint = &candidate;
                        break;
                    }
                }

                const ImVec2 marker_center(row_start.x + GUTTER_WIDTH * 0.5f,
                                           row_start.y + text_height * 0.5f);

                if (breakpoint != nullptr)
                {
                    const ImU32 color = breakpoint->enabled ? BREAKPOINT_COLOR
                                                            : BREAKPOINT_DISABLED_COLOR;

                    // hollow until lldb finds somewhere to actually put it
                    if (breakpoint->resolved)
                        draw_list->AddCircleFilled(marker_center, BREAKPOINT_RADIUS, color);
                    else
                        draw_list->AddCircle(marker_center, BREAKPOINT_RADIUS, color, 0, 1.5f);
                }
                else if (gutter_hovered)
                {
                    draw_list->AddCircleFilled(marker_center, BREAKPOINT_RADIUS, BREAKPOINT_HOVER_COLOR);
                }

                const std::string number_text = std::to_string(line_number);

                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + number_width -
                                     ImGui::CalcTextSize(number_text.c_str()).x);
                ImGui::TextDisabled("%s", number_text.c_str());

                ImGui::SameLine(0.0f, GUTTER_MARGIN * 2.0f);
                ImGui::TextUnformatted(m_lines[static_cast<size_t>(index)].c_str());
            }
        }

        clipper.End();

        ImGui::EndChild();
    }
}
