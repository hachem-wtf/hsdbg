#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Hsdbg
{
    // one preprocessing token. whitespace is dropped during tokenising, so a run
    // of tokens is rendered back to text with a small spacing heuristic. the hide
    // set is the classic preprocessor "blue paint": the names of macros that must
    // not expand this token again, which is what keeps a self-referential macro
    // from unrolling forever
    enum class PpKind : uint8_t
    {
        Identifier,
        Number,
        String,
        Char,
        Punct,
    };

    struct PpToken
    {
        PpKind kind = PpKind::Identifier;
        std::string text;
        std::vector<std::string> hide;
    };

    // a single #define, object-like (BUFFER) or function-like (MAX(a, b)). the
    // body is the replacement list as significant tokens, with #, ## and the
    // parameter names left in place for substitution time
    struct MacroDef
    {
        std::string name;
        bool function_like = false;
        bool variadic = false;
        std::vector<std::string> params;
        std::vector<PpToken> body;
    };

    // the set of #defines visible in a source file. built by scanning the file
    // for #define / #undef directives in order, optionally following local
    // "quote" includes one project deep. system <...> includes are ignored on
    // purpose: this is meant for a project's own macros, not libc's
    class MacroTable
    {
    public:
        auto clear() -> void;

        // scan already-loaded lines as the primary file, then chase local quote
        // includes relative to base_dir off disk. either argument may be empty
        auto build(const std::vector<std::string>& lines,
                   const std::filesystem::path& base_dir) -> void;

        auto find(std::string_view name) const -> const MacroDef*;

        auto empty() const -> bool { return m_macros.empty(); }
        auto size() const -> size_t { return m_macros.size(); }

        // bumped every rebuild, so a consumer can tell its cached expansion is
        // stale without comparing the whole table
        auto revision() const -> uint64_t { return m_revision; }

    private:
        auto scan_lines(const std::vector<std::string>& lines,
                        const std::filesystem::path& base_dir, int depth,
                        std::vector<std::filesystem::path>& seen) -> void;
        auto scan_path(const std::filesystem::path& path, int depth,
                       std::vector<std::filesystem::path>& seen) -> void;

        std::unordered_map<std::string, MacroDef> m_macros;
        uint64_t m_revision = 0;
    };

    // the result of unrolling an expression one macro layer at a time. levels[0]
    // is the input as written, levels[i] is the text after i rounds of expansion,
    // and levels.back() is the fixpoint where nothing expands any further
    struct MacroExpansion
    {
        std::vector<std::string> levels;

        // names of macros that were actually expanded going into each level, so
        // levels_expanded[i] describes what turned levels[i-1] into levels[i].
        // index 0 is always empty
        std::vector<std::vector<std::string>> expanded;

        auto fully_expanded() const -> bool { return m_reached_fixpoint; }

        bool m_reached_fixpoint = true;
    };

    // unroll input against the table, capping the number of layers so a
    // pathological macro cannot hang the ui
    auto expand_stages(const MacroTable& table, std::string_view input,
                       int max_levels = 64) -> MacroExpansion;
}
