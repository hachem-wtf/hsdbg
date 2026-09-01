#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Hsdbg
{
    class Debugger;

    // one coloured run inside a source line; start and length index into the
    // line's own string so the spans stay valid as long as the line does
    enum class SyntaxKind : uint8_t
    {
        Default,
        Keyword,
        Type,
        String,
        Number,
        Comment,
        Preprocessor,
    };

    struct SourceSpan
    {
        uint32_t start;
        uint32_t length;
        SyntaxKind kind;
    };

    class SourceView
    {
    public:
        auto open(const std::filesystem::path& path) -> Result<void>;
        auto close() -> void;

        auto draw(Debugger& debugger) -> void;

        auto set_highlighted_line(uint32_t line) -> void;
        auto set_highlighting(bool enabled) -> void { m_highlighting_enabled = enabled; }
        auto set_line_numbers(bool enabled) -> void { m_line_numbers = enabled; }
        auto set_highlight_current_line(bool enabled) -> void { m_highlight_current_line = enabled; }

        // packed ImU32 (0xAABBGGRR) colours, one per SyntaxKind, index 0 unused
        auto set_syntax_color(size_t kind, unsigned int color) -> void { m_syntax_colors[kind] = color; }
        auto set_current_line_color(unsigned int color) -> void { m_current_line_color = color; }

        auto path() const -> const std::filesystem::path& { return m_path; }
        auto line_count() const -> size_t { return m_lines.size(); }
        auto is_open() const -> bool { return !m_lines.empty(); }

    private:
        auto draw_open_bar() -> void;
        auto draw_lines(Debugger& debugger) -> void;

        std::filesystem::path m_path;
        std::vector<std::string> m_lines;
        std::vector<std::vector<SourceSpan>> m_spans;
        bool m_highlight = false;
        bool m_highlighting_enabled = true;
        bool m_line_numbers = true;
        bool m_highlight_current_line = true;

        // indexed by SyntaxKind; the defaults match the built-in dark palette
        unsigned int m_syntax_colors[7] = {
            0, 0xFFD69C56, 0xFFB0C94E, 0xFF7891CE, 0xFFA8CEB5, 0xFF55996A, 0xFFC086C5,
        };
        unsigned int m_current_line_color = 0xFF2E483A;
        std::string m_path_input;
        std::string m_error;
        uint32_t m_highlighted_line = 0;
        bool m_scroll_to_highlight = false;
    };
}
