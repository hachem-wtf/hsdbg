#include "ui/preferences.h"

#include "core/log.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

namespace Hsdbg
{
    namespace
    {
        auto as_float(std::string_view text, float fallback) -> float
        {
            float value = fallback;
            std::from_chars(text.data(), text.data() + text.size(), value);
            return value;
        }

        auto as_bool(std::string_view text, bool fallback) -> bool
        {
            if (text == "true" || text == "1")
                return true;

            if (text == "false" || text == "0")
                return false;

            return fallback;
        }

        auto read_color(std::string_view value, float (&out)[3]) -> void
        {
            std::sscanf(std::string(value).c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
        }
    }

    auto load_preferences(const std::filesystem::path& path) -> Preferences
    {
        Preferences preferences;

        std::ifstream file(path);
        if (!file.is_open())
            return preferences;

        std::string line;
        while (std::getline(file, line))
        {
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            const std::string_view key(line.data(), equals);
            const std::string_view value(line.data() + equals + 1, line.size() - equals - 1);

            if (key == "ui_scale")
                preferences.ui_scale = as_float(value, preferences.ui_scale);
            else if (key == "accent")
                std::sscanf(std::string(value).c_str(), "%f %f %f",
                            &preferences.accent[0], &preferences.accent[1], &preferences.accent[2]);
            else if (key == "rounding")
                preferences.rounding = as_float(value, preferences.rounding);
            else if (key == "show_mascot")
                preferences.show_mascot = as_bool(value, preferences.show_mascot);
            else if (key == "mascot_scale")
                preferences.mascot_scale = as_float(value, preferences.mascot_scale);
            else if (key == "syntax_highlighting")
                preferences.syntax_highlighting = as_bool(value, preferences.syntax_highlighting);
            else if (key == "show_line_numbers")
                preferences.show_line_numbers = as_bool(value, preferences.show_line_numbers);
            else if (key == "highlight_current_line")
                preferences.highlight_current_line = as_bool(value, preferences.highlight_current_line);
            else if (key == "color_keyword")
                read_color(value, preferences.color_keyword);
            else if (key == "color_type")
                read_color(value, preferences.color_type);
            else if (key == "color_string")
                read_color(value, preferences.color_string);
            else if (key == "color_number")
                read_color(value, preferences.color_number);
            else if (key == "color_comment")
                read_color(value, preferences.color_comment);
            else if (key == "color_preprocessor")
                read_color(value, preferences.color_preprocessor);
            else if (key == "color_current_line")
                read_color(value, preferences.color_current_line);
            else if (key == "stop_at_entry")
                preferences.stop_at_entry = as_bool(value, preferences.stop_at_entry);
            else if (key == "step_by_instruction")
                preferences.step_by_instruction = as_bool(value, preferences.step_by_instruction);
            else if (key == "show_fps")
                preferences.show_fps = as_bool(value, preferences.show_fps);
        }

        return preferences;
    }

    auto save_preferences(const std::filesystem::path& path, const Preferences& preferences) -> void
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open())
        {
            Log::warn("preferences: could not write '{}'", path.string());
            return;
        }

        const auto color = [](const float (&c)[3]) {
            return std::to_string(c[0]) + ' ' + std::to_string(c[1]) + ' ' + std::to_string(c[2]);
        };

        file << "ui_scale=" << preferences.ui_scale << '\n'
             << "accent=" << color(preferences.accent) << '\n'
             << "rounding=" << preferences.rounding << '\n'
             << "show_mascot=" << (preferences.show_mascot ? "true" : "false") << '\n'
             << "mascot_scale=" << preferences.mascot_scale << '\n'
             << "syntax_highlighting=" << (preferences.syntax_highlighting ? "true" : "false") << '\n'
             << "show_line_numbers=" << (preferences.show_line_numbers ? "true" : "false") << '\n'
             << "highlight_current_line=" << (preferences.highlight_current_line ? "true" : "false") << '\n'
             << "color_keyword=" << color(preferences.color_keyword) << '\n'
             << "color_type=" << color(preferences.color_type) << '\n'
             << "color_string=" << color(preferences.color_string) << '\n'
             << "color_number=" << color(preferences.color_number) << '\n'
             << "color_comment=" << color(preferences.color_comment) << '\n'
             << "color_preprocessor=" << color(preferences.color_preprocessor) << '\n'
             << "color_current_line=" << color(preferences.color_current_line) << '\n'
             << "stop_at_entry=" << (preferences.stop_at_entry ? "true" : "false") << '\n'
             << "step_by_instruction=" << (preferences.step_by_instruction ? "true" : "false") << '\n'
             << "show_fps=" << (preferences.show_fps ? "true" : "false") << '\n';
    }
}
