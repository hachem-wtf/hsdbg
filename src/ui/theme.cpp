#include "ui/theme.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Hsdbg
{
    namespace
    {
        // a deliberately small TOML reader: enough for theme files (tables, one key
        // per line, strings / numbers / bools / arrays, inline comments) without a
        // dependency; anything unrecognised keeps the default

        auto trim(std::string_view text) -> std::string_view
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1);

            return text;
        }

        // drop a trailing "# comment", but only when the '#' sits outside a
        // quoted string, so a hex colour like "#88C0D0" survives
        auto strip_comment(std::string_view line) -> std::string_view
        {
            bool in_quote = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"')
                    in_quote = !in_quote;
                else if (line[i] == '#' && !in_quote)
                    return line.substr(0, i);
            }
            return line;
        }

        auto unquote(std::string_view value) -> std::string_view
        {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                return value.substr(1, value.size() - 2);
            return value;
        }

        auto hex_pair(std::string_view text) -> float
        {
            int value = 0;
            std::from_chars(text.data(), text.data() + text.size(), value, 16);
            return static_cast<float>(value) / 255.0f;
        }

        // "#rgb", "#rrggbb" or "#rrggbbaa", or an array "[r, g, b(, a)]" of
        // floats in 0..1; anything else leaves the fallback in place
        auto parse_color(std::string_view value, ImVec4 fallback) -> ImVec4
        {
            value = trim(unquote(trim(value)));
            if (value.empty())
                return fallback;

            if (value.front() == '#')
            {
                value.remove_prefix(1);
                ImVec4 out = fallback;
                out.w = 1.0f;

                if (value.size() == 3)
                {
                    const auto nibble = [&](size_t i) {
                        const char pair[2] = { value[i], value[i] };
                        return hex_pair(std::string_view(pair, 2));
                    };
                    out.x = nibble(0);
                    out.y = nibble(1);
                    out.z = nibble(2);
                    return out;
                }

                if (value.size() == 6 || value.size() == 8)
                {
                    out.x = hex_pair(value.substr(0, 2));
                    out.y = hex_pair(value.substr(2, 2));
                    out.z = hex_pair(value.substr(4, 2));
                    if (value.size() == 8)
                        out.w = hex_pair(value.substr(6, 2));
                    return out;
                }

                return fallback;
            }

            if (value.front() == '[')
            {
                value.remove_prefix(1);
                if (!value.empty() && value.back() == ']')
                    value.remove_suffix(1);

                std::array<float, 4> parts = { fallback.x, fallback.y, fallback.z, 1.0f };
                size_t count = 0;
                while (!value.empty() && count < parts.size())
                {
                    const size_t comma = value.find(',');
                    const std::string_view token = trim(value.substr(0, comma));
                    float parsed = parts[count];
                    std::from_chars(token.data(), token.data() + token.size(), parsed);
                    parts[count++] = parsed;
                    if (comma == std::string_view::npos)
                        break;
                    value.remove_prefix(comma + 1);
                }

                if (count >= 3)
                    return ImVec4(parts[0], parts[1], parts[2], parts[3]);
            }

            return fallback;
        }

        auto parse_float(std::string_view value, float fallback) -> float
        {
            value = trim(unquote(trim(value)));
            float parsed = fallback;
            std::from_chars(value.data(), value.data() + value.size(), parsed);
            return parsed;
        }

        using Section = std::unordered_map<std::string, std::string>;
        using Document = std::unordered_map<std::string, Section>;

        auto parse_document(const std::filesystem::path& path, Document& out) -> bool
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;

            std::string current;
            std::string line;
            while (std::getline(file, line))
            {
                std::string_view view = trim(strip_comment(line));
                if (view.empty())
                    continue;

                if (view.front() == '[')
                {
                    const size_t close = view.find(']');
                    if (close != std::string_view::npos)
                        current = std::string(trim(view.substr(1, close - 1)));
                    continue;
                }

                const size_t equals = view.find('=');
                if (equals == std::string_view::npos)
                    continue;

                const std::string key(trim(view.substr(0, equals)));
                const std::string value(trim(view.substr(equals + 1)));
                out[current][key] = value;
            }

            return true;
        }

        auto lookup(const Document& doc, std::string_view section, std::string_view key)
            -> const std::string*
        {
            const auto s = doc.find(std::string(section));
            if (s == doc.end())
                return nullptr;
            const auto k = s->second.find(std::string(key));
            return k == s->second.end() ? nullptr : &k->second;
        }
    }

    auto default_theme() -> Theme
    {
        return Theme{};
    }

    auto load_theme(const std::filesystem::path& path) -> Theme
    {
        Theme theme = default_theme();

        Document doc;
        if (!parse_document(path, doc))
        {
            Log::warn("theme: could not read '{}'", path.string());
            return theme;
        }

        if (const std::string* name = lookup(doc, "theme", "name"))
            theme.name = std::string(unquote(trim(*name)));

        const auto color = [&](const char* section, const char* key, ImVec4& field) {
            if (const std::string* value = lookup(doc, section, key))
                field = parse_color(*value, field);
        };

        color("palette", "text", theme.text);
        color("palette", "text_dim", theme.text_dim);
        color("palette", "bg", theme.bg);
        color("palette", "bg_low", theme.bg_low);
        color("palette", "bg_high", theme.bg_high);
        color("palette", "surface", theme.surface);
        color("palette", "border", theme.border);
        color("palette", "accent", theme.accent);

        if (const std::string* value = lookup(doc, "style", "rounding"))
            theme.rounding = parse_float(*value, theme.rounding);
        if (const std::string* value = lookup(doc, "style", "window_border"))
            theme.window_border = parse_float(*value, theme.window_border);

        color("syntax", "keyword", theme.syntax_keyword);
        color("syntax", "type", theme.syntax_type);
        color("syntax", "string", theme.syntax_string);
        color("syntax", "number", theme.syntax_number);
        color("syntax", "comment", theme.syntax_comment);
        color("syntax", "preprocessor", theme.syntax_preprocessor);
        color("syntax", "current_line", theme.current_line);

        return theme;
    }

    auto list_themes(const std::filesystem::path& dir) -> std::vector<ThemeEntry>
    {
        std::vector<ThemeEntry> entries;

        std::error_code error;
        if (!std::filesystem::is_directory(dir, error))
            return entries;

        for (const auto& item : std::filesystem::directory_iterator(dir, error))
        {
            if (!item.is_regular_file() || item.path().extension() != ".toml")
                continue;

            // the display name lives in the file; read just that rather than the
            // full palette, since the picker only needs a label
            Document doc;
            std::string name = item.path().stem().string();
            if (parse_document(item.path(), doc))
            {
                if (const std::string* value = lookup(doc, "theme", "name"))
                    name = std::string(unquote(trim(*value)));
            }

            entries.push_back(ThemeEntry{ item.path().stem().string(), std::move(name), item.path() });
        }

        std::sort(entries.begin(), entries.end(),
                  [](const ThemeEntry& a, const ThemeEntry& b) { return a.name < b.name; });

        return entries;
    }
}
