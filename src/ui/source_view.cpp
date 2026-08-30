#include "ui/source_view.h"

#include "core/log.h"
#include "debugger/debugger.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace Hsdbg
{
    namespace
    {
        constexpr float BREAKPOINT_RADIUS = 5.0f;

        // a dark palette in the spirit of the source view's existing greens; the
        // default kind carries no colour so ordinary text keeps the theme's own
        auto color_of(SyntaxKind kind) -> ImU32
        {
            switch (kind)
            {
                case SyntaxKind::Keyword:      return IM_COL32(86, 156, 214, 255);
                case SyntaxKind::Type:         return IM_COL32(78, 201, 176, 255);
                case SyntaxKind::String:       return IM_COL32(206, 145, 120, 255);
                case SyntaxKind::Number:       return IM_COL32(181, 206, 168, 255);
                case SyntaxKind::Comment:      return IM_COL32(106, 153, 85, 255);
                case SyntaxKind::Preprocessor: return IM_COL32(197, 134, 192, 255);
                case SyntaxKind::Default:      break;
            }

            return 0;
        }

        enum class Language : uint8_t
        {
            Cpp,
            Rust,
        };

        auto is_word_start(char c) -> bool
        {
            return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        auto is_word(char c) -> bool
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        auto classify_word(std::string_view word, Language language) -> SyntaxKind
        {
            static const std::unordered_set<std::string_view> cpp_keywords = {
                "alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor", "break",
                "case", "catch", "class", "co_await", "co_return", "co_yield", "compl",
                "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
                "continue", "decltype", "default", "delete", "do", "dynamic_cast", "else",
                "enum", "explicit", "export", "extern", "false", "final", "for", "friend",
                "goto", "if", "inline", "mutable", "namespace", "new", "noexcept", "not",
                "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private",
                "protected", "public", "register", "reinterpret_cast", "requires", "return",
                "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
                "template", "this", "thread_local", "throw", "true", "try", "typedef",
                "typeid", "typename", "union", "using", "virtual", "volatile", "while",
                "xor", "xor_eq",
            };

            static const std::unordered_set<std::string_view> cpp_types = {
                "auto", "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float",
                "int", "int8_t", "int16_t", "int32_t", "int64_t", "intptr_t", "long",
                "ptrdiff_t", "short", "signed", "size_t", "ssize_t", "uint8_t", "uint16_t",
                "uint32_t", "uint64_t", "uintptr_t", "unsigned", "void", "wchar_t",
            };

            static const std::unordered_set<std::string_view> rust_keywords = {
                "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
                "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop",
                "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self",
                "static", "struct", "super", "trait", "true", "type", "union", "unsafe",
                "use", "where", "while", "box", "dyn", "macro_rules", "yield",
            };

            static const std::unordered_set<std::string_view> rust_types = {
                "bool", "char", "str", "String", "i8", "i16", "i32", "i64", "i128", "isize",
                "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64", "Vec", "Option",
                "Result", "Box", "Rc", "Arc", "Cell", "RefCell", "HashMap", "HashSet",
                "BTreeMap", "BTreeSet", "VecDeque", "Cow",
            };

            const bool rust = language == Language::Rust;
            const auto& keywords = rust ? rust_keywords : cpp_keywords;
            const auto& types = rust ? rust_types : cpp_types;

            if (keywords.contains(word))
                return SyntaxKind::Keyword;

            if (types.contains(word))
                return SyntaxKind::Type;

            return SyntaxKind::Default;
        }

        auto only_space_before(const std::string& line, uint32_t upto) -> bool
        {
            for (uint32_t index = 0; index < upto; ++index)
            {
                if (std::isspace(static_cast<unsigned char>(line[index])) == 0)
                    return false;
            }

            return true;
        }

        // splits every line into contiguous coloured spans. the whole file is
        // walked in order so a block comment opened on one line stays open on the
        // next, which a per-line pass could not know
        auto highlight_lines(const std::vector<std::string>& lines, Language language)
            -> std::vector<std::vector<SourceSpan>>
        {
            std::vector<std::vector<SourceSpan>> out(lines.size());
            bool in_block = false;

            for (size_t number = 0; number < lines.size(); ++number)
            {
                const std::string& line = lines[number];
                std::vector<SourceSpan>& spans = out[number];
                const uint32_t size = static_cast<uint32_t>(line.size());

                uint32_t at = 0;
                uint32_t run = 0;

                const auto emit = [&](uint32_t start, uint32_t length, SyntaxKind kind)
                {
                    if (length != 0)
                        spans.push_back({ start, length, kind });
                };

                const auto flush_default = [&](uint32_t upto)
                {
                    emit(run, upto - run, SyntaxKind::Default);
                };

                if (in_block)
                {
                    const size_t end = line.find("*/");

                    if (end == std::string::npos)
                    {
                        emit(0, size, SyntaxKind::Comment);
                        continue;
                    }

                    emit(0, static_cast<uint32_t>(end) + 2, SyntaxKind::Comment);
                    at = static_cast<uint32_t>(end) + 2;
                    run = at;
                    in_block = false;
                }

                while (at < size)
                {
                    const char c = line[at];

                    if (c == '/' && at + 1 < size && line[at + 1] == '/')
                    {
                        flush_default(at);
                        emit(at, size - at, SyntaxKind::Comment);
                        run = size;
                        at = size;
                        break;
                    }

                    if (c == '/' && at + 1 < size && line[at + 1] == '*')
                    {
                        flush_default(at);
                        const size_t end = line.find("*/", at + 2);

                        if (end == std::string::npos)
                        {
                            emit(at, size - at, SyntaxKind::Comment);
                            in_block = true;
                            at = size;
                        }
                        else
                        {
                            emit(at, static_cast<uint32_t>(end) + 2 - at, SyntaxKind::Comment);
                            at = static_cast<uint32_t>(end) + 2;
                        }

                        run = at;
                        continue;
                    }

                    // a rust lifetime ('a) looks like an unterminated char literal,
                    // so peel it off as an identifier before the string handling
                    if (c == '\'' && language == Language::Rust && at + 1 < size &&
                        is_word_start(line[at + 1]) && !(at + 2 < size && line[at + 2] == '\''))
                    {
                        flush_default(at);
                        const uint32_t start = at++;

                        while (at < size && is_word(line[at]))
                            ++at;

                        emit(start, at - start, SyntaxKind::Default);
                        run = at;
                        continue;
                    }

                    if (c == '"' || c == '\'')
                    {
                        flush_default(at);
                        const uint32_t start = at++;

                        while (at < size)
                        {
                            if (line[at] == '\\' && at + 1 < size)
                            {
                                at += 2;
                                continue;
                            }

                            if (line[at] == c)
                            {
                                ++at;
                                break;
                            }

                            ++at;
                        }

                        emit(start, at - start, SyntaxKind::String);
                        run = at;
                        continue;
                    }

                    if (c == '#' && language == Language::Cpp && only_space_before(line, at))
                    {
                        flush_default(at);
                        const uint32_t start = at++;

                        while (at < size && is_word(line[at]))
                            ++at;

                        emit(start, at - start, SyntaxKind::Preprocessor);
                        run = at;
                        continue;
                    }

                    if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                        (c == '.' && at + 1 < size &&
                         std::isdigit(static_cast<unsigned char>(line[at + 1])) != 0))
                    {
                        flush_default(at);
                        const uint32_t start = at++;

                        while (at < size)
                        {
                            const char d = line[at];

                            if (std::isalnum(static_cast<unsigned char>(d)) != 0 ||
                                d == '.' || d == '\'' || d == '_')
                                ++at;
                            else
                                break;
                        }

                        emit(start, at - start, SyntaxKind::Number);
                        run = at;
                        continue;
                    }

                    if (is_word_start(c))
                    {
                        const uint32_t start = at++;

                        while (at < size && is_word(line[at]))
                            ++at;

                        if (const SyntaxKind kind = classify_word(
                                std::string_view(line).substr(start, at - start), language);
                            kind != SyntaxKind::Default)
                        {
                            flush_default(start);
                            emit(start, at - start, kind);
                            run = at;
                        }

                        continue;
                    }

                    ++at;
                }

                flush_default(size);
            }

            return out;
        }

        auto language_of(const std::filesystem::path& path) -> std::optional<Language>
        {
            static const std::unordered_set<std::string_view> cpp = {
                ".c", ".h", ".cc", ".cp", ".cpp", ".cxx", ".c++", ".hh", ".hpp", ".hxx",
                ".h++", ".inl", ".ipp", ".m", ".mm", ".cu", ".cuh",
            };

            std::string ext = path.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });

            if (ext == ".rs")
                return Language::Rust;

            if (cpp.contains(ext))
                return Language::Cpp;

            return std::nullopt;
        }

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

        const std::optional<Language> language = language_of(path);

        m_path = path;
        m_lines = std::move(lines);
        m_highlight = language.has_value();
        m_spans = language ? highlight_lines(m_lines, *language)
                           : std::vector<std::vector<SourceSpan>>{};
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
        m_spans.clear();
        m_highlight = false;
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

                const std::string& text = m_lines[static_cast<size_t>(index)];

                if (!m_highlight || m_spans[static_cast<size_t>(index)].empty())
                {
                    ImGui::TextUnformatted(text.c_str());
                }
                else
                {
                    bool first = true;

                    for (const SourceSpan& span : m_spans[static_cast<size_t>(index)])
                    {
                        if (!first)
                            ImGui::SameLine(0.0f, 0.0f);

                        first = false;

                        const char* const begin = text.c_str() + span.start;
                        const char* const end = begin + span.length;

                        if (span.kind == SyntaxKind::Default)
                        {
                            ImGui::TextUnformatted(begin, end);
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, color_of(span.kind));
                            ImGui::TextUnformatted(begin, end);
                            ImGui::PopStyleColor();
                        }
                    }
                }
            }
        }

        clipper.End();

        ImGui::EndChild();
    }
}
