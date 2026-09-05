#pragma once

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace Hsdbg
{
    // a small semantic palette, not a full ImGuiCol table: the ui derives every
    // interactive state and the accent from these level-based tokens (light or dark)
    struct Theme
    {
        std::string name = "Midnight";

        // neutral palette: the theme's identity
        ImVec4 text = ImVec4(0.925f, 0.933f, 0.953f, 1.00f);    // primary foreground
        ImVec4 text_dim = ImVec4(0.514f, 0.545f, 0.620f, 1.00f); // muted labels
        ImVec4 bg = ImVec4(0.082f, 0.086f, 0.110f, 1.00f);       // window body
        ImVec4 bg_low = ImVec4(0.063f, 0.067f, 0.086f, 1.00f);   // deepest: titles, tracks
        ImVec4 bg_high = ImVec4(0.118f, 0.125f, 0.157f, 1.00f);  // raised: inputs, headers
        ImVec4 surface = ImVec4(0.149f, 0.161f, 0.200f, 1.00f);  // button base, grabs
        ImVec4 border = ImVec4(0.212f, 0.227f, 0.278f, 1.00f);

        // seeded into the live preferences when the theme is chosen, so the accent /
        // rounding / syntax pickers reflect it yet stay tweakable
        ImVec4 accent = ImVec4(0.302f, 0.714f, 0.769f, 1.00f);
        float rounding = 6.0f;
        float window_border = 1.0f;

        // editor syntax colours, also copied into the live preferences on select
        ImVec4 syntax_keyword = ImVec4(0.337f, 0.612f, 0.839f, 1.00f);
        ImVec4 syntax_type = ImVec4(0.306f, 0.788f, 0.690f, 1.00f);
        ImVec4 syntax_string = ImVec4(0.808f, 0.569f, 0.471f, 1.00f);
        ImVec4 syntax_number = ImVec4(0.710f, 0.808f, 0.659f, 1.00f);
        ImVec4 syntax_comment = ImVec4(0.416f, 0.600f, 0.333f, 1.00f);
        ImVec4 syntax_preprocessor = ImVec4(0.773f, 0.525f, 0.753f, 1.00f);
        ImVec4 current_line = ImVec4(0.227f, 0.282f, 0.180f, 1.00f);
    };

    // a theme file on disk: the stem is the stable id persisted in preferences,
    // the name is what the picker shows
    struct ThemeEntry
    {
        std::string id;   // filename stem, e.g. "midnight"
        std::string name; // display name from [theme].name, else the stem
        std::filesystem::path path;
    };

    // the built-in fallback, used when no theme file can be read so the ui never
    // comes up unstyled
    auto default_theme() -> Theme;

    // every *.toml in the directory, sorted by display name; missing directory
    // yields an empty list
    auto list_themes(const std::filesystem::path& dir) -> std::vector<ThemeEntry>;

    // parse one theme file; any field the file omits keeps the default, and a
    // file that cannot be opened returns default_theme()
    auto load_theme(const std::filesystem::path& path) -> Theme;
}
