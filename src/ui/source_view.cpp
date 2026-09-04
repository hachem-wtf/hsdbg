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
#include <span>
#include <string_view>
#include <unordered_set>

namespace Hsdbg
{
    namespace
    {
        constexpr float BREAKPOINT_RADIUS = 5.0f;


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
            return std::ranges::all_of(line.begin(), line.begin() + upto, [](char character)
            {
                return std::isspace(static_cast<unsigned char>(character)) != 0;
            });
        }

        // splits every line into contiguous coloured spans. the whole file is
        // walked in order so a block comment opened on one line stays open on the
        // next, which a per-line pass could not know
        auto highlight_lines(const std::vector<std::string>& lines, Language language,
                             const MacroTable& macros)
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

                        const std::string_view word =
                            std::string_view(line).substr(start, at - start);
                        SyntaxKind kind = classify_word(word, language);

                        // a plain identifier that names a #define is coloured as a
                        // macro so it reads as expandable in the source view
                        if (kind == SyntaxKind::Default && language == Language::Cpp &&
                            macros.find(word) != nullptr)
                            kind = SyntaxKind::Macro;

                        // every word gets its own span, including plain identifiers:
                        // isolating them lets the source view hit-test a name under
                        // the cursor and show its live value
                        flush_default(start);
                        emit(start, at - start, kind);
                        run = at;

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

        // muted blue-grey for the live values shown after a line, distinct from the
        // green of comments so they do not read as part of the code
        const ImU32 INLINE_VALUE_COLOR = IM_COL32(122, 140, 170, 235);

        auto line_number_width(size_t line_count) -> float
        {
            return ImGui::CalcTextSize(std::to_string(line_count).c_str()).x;
        }

        // the frame local named exactly `word`, or null; only names with a value to
        // show qualify, so pure aggregates without a summary are skipped
        auto find_local(std::span<const Variable> locals, std::string_view word) -> const Variable*
        {
            const auto match = std::ranges::find_if(locals, [&](const Variable& local)
            {
                return !local.value.empty() && local.name == word;
            });

            return match != locals.end() ? &*match : nullptr;
        }

        // whether `name` appears in `line` as a whole identifier rather than as a
        // fragment of a longer word
        auto contains_word(const std::string& line, const std::string& name) -> bool
        {
            if (name.empty())
                return false;

            for (size_t at = line.find(name); at != std::string::npos; at = line.find(name, at + 1))
            {
                const bool left = at > 0 && is_word(line[at - 1]);
                const bool right = at + name.size() < line.size() && is_word(line[at + name.size()]);

                if (!left && !right)
                    return true;
            }

            return false;
        }

        // a single-line, length-capped rendering of a value for the inline annotation
        auto inline_value(const Variable& local) -> std::string
        {
            std::string text = local.name + " = " + local.value;

            if (const auto newline = text.find('\n'); newline != std::string::npos)
                text.resize(newline);

            constexpr size_t cap = 48;
            if (text.size() > cap)
            {
                text.resize(cap - 3);
                text += "...";
            }

            return text;
        }

        // grab a macro invocation out of a line starting at the name: the name
        // alone for an object-like use, or the name plus a balanced argument list
        // for a function-like one. a call that runs off the end of the line falls
        // back to the bare name rather than guessing where it closes
        auto capture_invocation(const std::string& line, uint32_t start) -> std::string
        {
            const size_t size = line.size();
            size_t at = start;

            while (at < size && is_word(line[at]))
                ++at;

            size_t paren = at;

            while (paren < size && std::isspace(static_cast<unsigned char>(line[paren])) != 0)
                ++paren;

            if (paren < size && line[paren] == '(')
            {
                int depth = 0;

                for (size_t i = paren; i < size; ++i)
                {
                    if (line[i] == '(')
                    {
                        ++depth;
                    }
                    else if (line[i] == ')')
                    {
                        --depth;

                        if (depth == 0)
                            return line.substr(start, i + 1 - start);
                    }
                }
            }

            return line.substr(start, at - start);
        }
    }

    auto SourceView::open(const std::filesystem::path& file_path) -> Result<void>
    {
        std::ifstream file(file_path);
        if (!file.is_open())
            return fail("could not open '{}'", file_path.string());

        std::vector<std::string> lines;
        std::string line;

        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            lines.push_back(std::move(line));
        }

        const std::optional<Language> language = language_of(file_path);

        m_path = file_path;
        m_lines = std::move(lines);
        m_highlight = language.has_value();

        // gather the file's #defines (chasing local quote includes) so macro
        // names highlight and the macros panel has something to expand
        m_macros.clear();

        if (language == Language::Cpp)
            m_macros.build(m_lines, file_path.parent_path());

        m_spans = language ? highlight_lines(m_lines, *language, m_macros)
                           : std::vector<std::vector<SourceSpan>>{};
        m_path_input = file_path.string();
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
        m_macros.clear();
        m_macro_request.reset();
        m_highlight = false;
        m_error.clear();
        m_highlighted_line = 0;
    }

    auto SourceView::take_macro_request() -> std::optional<std::string>
    {
        std::optional<std::string> request = std::move(m_macro_request);
        m_macro_request.reset();
        return request;
    }

    auto SourceView::take_watch_request() -> std::optional<std::string>
    {
        std::optional<std::string> request = std::move(m_watch_request);
        m_watch_request.reset();
        return request;
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
        const float number_width = m_line_numbers ? line_number_width(m_lines.size()) : 0.0f;

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

        // the selected frame's locals, but only when we are stopped in this very
        // file (a non-zero highlight means show_frame put us here); used both to
        // annotate lines and to answer a hover over a name
        const std::span<const Variable> locals =
            (debugger.is_stopped() && m_highlighted_line != 0) ? debugger.locals()
                                                               : std::span<const Variable>{};

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_lines.size()), text_height);

        while (clipper.Step())
        {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const uint32_t line_number = static_cast<uint32_t>(index) + 1;
                const ImVec2 row_start = ImGui::GetCursorScreenPos();

                if (line_number == m_highlighted_line && m_highlight_current_line)
                {
                    draw_list->AddRectFilled(row_start,
                                             ImVec2(row_start.x + ImGui::GetContentRegionAvail().x,
                                                    row_start.y + text_height),
                                             m_current_line_color);
                }

                ImGui::PushID(index);

                if (ImGui::InvisibleButton("##gutter", ImVec2(GUTTER_WIDTH, text_height)))
                {
                    const std::span<const Breakpoint> breakpoints = debugger.breakpoints();
                    const auto found = std::ranges::find_if(breakpoints, [&](const Breakpoint& candidate)
                    {
                        return candidate.file == m_path && candidate.line == line_number;
                    });
                    const Breakpoint* existing = found != breakpoints.end() ? &*found : nullptr;

                    if (existing != nullptr)
                        debugger.remove_breakpoint(existing->id);
                    else
                        debugger.add_breakpoint(m_path, line_number);
                }

                const bool gutter_hovered = ImGui::IsItemHovered();

                ImGui::PopID();

                const std::span<const Breakpoint> breakpoints = debugger.breakpoints();
                const auto found = std::ranges::find_if(breakpoints, [&](const Breakpoint& candidate)
                {
                    return candidate.file == m_path && candidate.line == line_number;
                });
                const Breakpoint* breakpoint = found != breakpoints.end() ? &*found : nullptr;

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

                if (m_line_numbers)
                {
                    const std::string number_text = std::to_string(line_number);

                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + number_width -
                                         ImGui::CalcTextSize(number_text.c_str()).x);
                    ImGui::TextDisabled("%s", number_text.c_str());
                }

                ImGui::SameLine(0.0f, GUTTER_MARGIN * 2.0f);

                const std::string& text = m_lines[static_cast<size_t>(index)];

                if (!m_highlight || !m_highlighting_enabled ||
                    m_spans[static_cast<size_t>(index)].empty())
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

                            const std::string_view word(begin, static_cast<size_t>(end - begin));
                            const bool is_name = !word.empty() && is_word_start(word.front());

                            // hovering a name that is a live local shows its value
                            if (!locals.empty() && ImGui::IsItemHovered())
                            {
                                if (const Variable* local = find_local(locals, word))
                                    ImGui::SetTooltip("%s = %s%s%s", local->name.c_str(),
                                                      local->value.c_str(),
                                                      local->type.empty() ? "" : "\n",
                                                      local->type.c_str());
                            }

                            // right-click any name to add it to the watch list
                            if (is_name && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                                m_watch_request = std::string(word);
                        }
                        else if (span.kind == SyntaxKind::Macro)
                        {
                            draw_macro_span(draw_list, text, span);
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  m_syntax_colors[static_cast<size_t>(span.kind)]);
                            ImGui::TextUnformatted(begin, end);
                            ImGui::PopStyleColor();
                        }
                    }
                }

                // trailing live values: the locals that appear on this line, shown
                // only up to the line the pc sits on, since anything past it has
                // not run yet and would read as a stale or unset value
                if (!locals.empty() && line_number <= m_highlighted_line)
                {
                    int shown = 0;

                    for (const Variable& local : locals)
                    {
                        if (shown >= 4)
                            break;

                        if (local.value.empty() || !contains_word(text, local.name))
                            continue;

                        ImGui::SameLine(0.0f, shown == 0 ? GUTTER_MARGIN * 3.0f : GUTTER_MARGIN);
                        ImGui::PushStyleColor(ImGuiCol_Text, INLINE_VALUE_COLOR);
                        ImGui::TextUnformatted(inline_value(local).c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s = %s%s%s", local.name.c_str(), local.value.c_str(),
                                              local.type.empty() ? "" : "\n", local.type.c_str());

                        ++shown;
                    }
                }
            }
        }

        clipper.End();

        ImGui::EndChild();
    }

    auto SourceView::draw_macro_span(ImDrawList* draw_list, const std::string& line,
                                     const SourceSpan& span) -> void
    {
        const char* const begin = line.c_str() + span.start;
        const char* const end = begin + span.length;
        const ImU32 color = m_syntax_colors[static_cast<size_t>(SyntaxKind::Macro)];

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(begin, end);
        ImGui::PopStyleColor();

        if (!ImGui::IsItemHovered())
            return;

        // underline on hover so the name reads as a link
        const ImVec2 rect_min = ImGui::GetItemRectMin();
        const ImVec2 rect_max = ImGui::GetItemRectMax();
        draw_list->AddLine(ImVec2(rect_min.x, rect_max.y - 1.0f),
                           ImVec2(rect_max.x, rect_max.y - 1.0f), color);

        const std::string invocation = capture_invocation(line, span.start);
        const MacroExpansion expansion = expand_stages(m_macros, invocation);

        ImGui::BeginTooltip();

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(invocation.c_str());
        ImGui::PopStyleColor();

        ImGui::Separator();

        std::string preview = expansion.levels.back();

        if (constexpr size_t limit = 240; preview.size() > limit)
        {
            preview.resize(limit);
            preview += " ...";
        }

        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(preview.c_str());
        ImGui::PopTextWrapPos();

        ImGui::Spacing();

        const size_t steps = expansion.levels.size() - 1;

        if (steps == 0)
            ImGui::TextDisabled("no expansion");
        else if (!expansion.fully_expanded())
            ImGui::TextDisabled("%zu+ levels — click to step through", steps);
        else
            ImGui::TextDisabled("%zu level%s — click to step through", steps,
                                steps == 1 ? "" : "s");

        ImGui::EndTooltip();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_macro_request = invocation;
    }
}
