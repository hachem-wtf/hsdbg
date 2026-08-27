#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Hsdbg
{
    class Debugger;

    class SourceView
    {
    public:
        auto open(const std::filesystem::path& path) -> Result<void>;
        auto close() -> void;

        auto draw(Debugger& debugger) -> void;

        auto set_highlighted_line(uint32_t line) -> void;

        auto path() const -> const std::filesystem::path& { return m_path; }
        auto line_count() const -> size_t { return m_lines.size(); }
        auto is_open() const -> bool { return !m_lines.empty(); }

    private:
        auto draw_open_bar() -> void;
        auto draw_lines(Debugger& debugger) -> void;

        std::filesystem::path m_path;
        std::vector<std::string> m_lines;
        std::string m_path_input;
        std::string m_error;
        uint32_t m_highlighted_line = 0;
        bool m_scroll_to_highlight = false;
    };
}
