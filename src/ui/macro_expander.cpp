#include "ui/macro_expander.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace Hsdbg
{
    namespace
    {
        // hard ceilings so a runaway macro can never take the whole ui with it
        constexpr size_t MAX_TOKENS = 200000;
        constexpr int MAX_INCLUDE_DEPTH = 8;

        auto is_ident_start(char c) -> bool
        {
            return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        auto is_ident(char c) -> bool
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        // pull one preprocessing token out of text starting at index. strings and
        // character literals are kept whole, and multi-character punctuators the
        // expander actually reasons about (##, ::, ->) are recognised so they do
        // not split. everything else falls back to a single-character punctuator
        auto lex_one(std::string_view text, size_t& at) -> PpToken
        {
            while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
                ++at;

            PpToken token;

            if (at >= text.size())
                return token;

            const char c = text[at];

            if (is_ident_start(c))
            {
                const size_t start = at;
                while (at < text.size() && is_ident(text[at]))
                    ++at;

                token.kind = PpKind::Identifier;
                token.text = std::string(text.substr(start, at - start));
                return token;
            }

            if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                (c == '.' && at + 1 < text.size() &&
                 std::isdigit(static_cast<unsigned char>(text[at + 1])) != 0))
            {
                const size_t start = at++;

                // a pp-number swallows digits, letters, dots and the exponent
                // signs, which is loose but matches how the standard lexes them
                while (at < text.size())
                {
                    const char d = text[at];

                    if ((d == 'e' || d == 'E' || d == 'p' || d == 'P') && at + 1 < text.size() &&
                        (text[at + 1] == '+' || text[at + 1] == '-'))
                    {
                        at += 2;
                        continue;
                    }

                    if (is_ident(d) || d == '.')
                        ++at;
                    else
                        break;
                }

                token.kind = PpKind::Number;
                token.text = std::string(text.substr(start, at - start));
                return token;
            }

            if (c == '"' || c == '\'')
            {
                const size_t start = at++;

                while (at < text.size())
                {
                    if (text[at] == '\\' && at + 1 < text.size())
                    {
                        at += 2;
                        continue;
                    }

                    if (text[at] == c)
                    {
                        ++at;
                        break;
                    }

                    ++at;
                }

                token.kind = c == '"' ? PpKind::String : PpKind::Char;
                token.text = std::string(text.substr(start, at - start));
                return token;
            }

            static constexpr std::string_view multis[] = {
                "...", "<<=", ">>=", "->*", "::", "->", "##", "<<", ">>", "<=", ">=",
                "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
                "^=", "++", "--", ".*",
            };

            for (const std::string_view op : multis)
            {
                if (text.compare(at, op.size(), op) == 0)
                {
                    at += op.size();
                    token.kind = PpKind::Punct;
                    token.text = std::string(op);
                    return token;
                }
            }

            token.kind = PpKind::Punct;
            token.text = std::string(1, c);
            ++at;
            return token;
        }

        auto tokenize(std::string_view text) -> std::vector<PpToken>
        {
            std::vector<PpToken> tokens;
            size_t at = 0;

            while (at < text.size() && tokens.size() < MAX_TOKENS)
            {
                PpToken token = lex_one(text, at);

                if (token.text.empty())
                    break;

                tokens.push_back(std::move(token));
            }

            return tokens;
        }

        auto hidden_by(const PpToken& token, const std::string& name) -> bool
        {
            return std::ranges::find(token.hide, name) != token.hide.end();
        }

        // whether a space belongs between two adjacent tokens when rendering back
        // to text; purely cosmetic, aimed at readable c-ish output
        auto needs_space(const PpToken& left, const PpToken& right) -> bool
        {
            const std::string& l = left.text;
            const std::string& r = right.text;

            if (r == ")" || r == "]" || r == "," || r == ";" || r == "::" ||
                r == "." || r == "->")
                return false;

            if (l == "(" || l == "[" || l == "::" || l == "." || l == "->")
                return false;

            if (r == "(" && (left.kind == PpKind::Identifier || l == ")" || l == "]"))
                return false;

            return true;
        }

        auto render(const std::vector<PpToken>& tokens) -> std::string
        {
            std::string out;

            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (i != 0 && needs_space(tokens[i - 1], tokens[i]))
                    out += ' ';

                out += tokens[i].text;
            }

            return out;
        }

        auto param_index(const MacroDef& def, const std::string& name) -> int
        {
            for (size_t i = 0; i < def.params.size(); ++i)
            {
                if (def.params[i] == name)
                    return static_cast<int>(i);
            }

            return -1;
        }

        // build a string literal token out of an argument's spelling, escaping the
        // way # is required to: backslashes and quotes inside the text survive
        auto stringize(const std::vector<PpToken>& arg) -> PpToken
        {
            std::string inner = render(arg);
            std::string escaped;

            for (const char c : inner)
            {
                if (c == '\\' || c == '"')
                    escaped += '\\';

                escaped += c;
            }

            return PpToken{ PpKind::String, "\"" + escaped + "\"", {} };
        }

        auto paste_token(const std::string& text) -> PpToken
        {
            std::vector<PpToken> lexed = tokenize(text);

            if (lexed.size() == 1)
                return lexed.front();

            // the paste did not form a single clean token; keep the joined
            // spelling so the user still sees what ## produced
            return PpToken{ PpKind::Identifier, text, {} };
        }

        // the arguments handed to a function-like macro. an object-like macro just
        // passes an empty list through
        using ArgList = std::vector<std::vector<PpToken>>;

        // run every expansion layer on tokens until nothing changes, used to
        // fully expand an argument before it is pasted into a macro body
        auto expand_full(const MacroTable& table, std::vector<PpToken> tokens)
            -> std::vector<PpToken>;

        // raw holds each argument exactly as written (what # and ## must see);
        // expanded holds the same arguments after full macro expansion (what a
        // plain parameter reference is replaced with, per the standard)
        auto substitute(const MacroDef& def, const ArgList& raw, const ArgList& expanded)
            -> std::vector<PpToken>
        {
            std::vector<PpToken> out;

            const auto raw_tokens = [&](int index) -> std::vector<PpToken>
            {
                if (index >= 0 && index < static_cast<int>(raw.size()))
                    return raw[static_cast<size_t>(index)];

                return {};
            };

            const auto expanded_tokens = [&](int index) -> std::vector<PpToken>
            {
                if (index >= 0 && index < static_cast<int>(expanded.size()))
                    return expanded[static_cast<size_t>(index)];

                return {};
            };

            // __VA_ARGS__ from the raw arguments, for # and ##
            const auto raw_varargs = [&]() -> std::vector<PpToken>
            {
                std::vector<PpToken> joined;

                for (size_t i = def.params.size(); i < raw.size(); ++i)
                {
                    if (i != def.params.size())
                        joined.push_back(PpToken{ PpKind::Punct, ",", {} });

                    joined.insert(joined.end(), raw[i].begin(), raw[i].end());
                }

                return joined;
            };

            const auto expanded_varargs = [&]() -> std::vector<PpToken>
            {
                std::vector<PpToken> joined;

                for (size_t i = def.params.size(); i < expanded.size(); ++i)
                {
                    if (i != def.params.size())
                        joined.push_back(PpToken{ PpKind::Punct, ",", {} });

                    joined.insert(joined.end(), expanded[i].begin(), expanded[i].end());
                }

                return joined;
            };

            const auto& body = def.body;

            for (size_t i = 0; i < body.size(); ++i)
            {
                const PpToken& tok = body[i];

                if (tok.text == "#" && def.function_like && i + 1 < body.size() &&
                    body[i + 1].kind == PpKind::Identifier)
                {
                    if (const int index = param_index(def, body[i + 1].text); index >= 0)
                    {
                        out.push_back(stringize(raw_tokens(index)));
                        ++i;
                        continue;
                    }

                    if (def.variadic && body[i + 1].text == "__VA_ARGS__")
                    {
                        out.push_back(stringize(raw_varargs()));
                        ++i;
                        continue;
                    }
                }

                if (tok.text == "##" && !out.empty() && i + 1 < body.size())
                {
                    const PpToken& rhs = body[i + 1];
                    std::vector<PpToken> pieces;

                    if (const int index = param_index(def, rhs.text); index >= 0)
                        pieces = raw_tokens(index);
                    else if (def.variadic && rhs.text == "__VA_ARGS__")
                        pieces = raw_varargs();
                    else
                        pieces = { rhs };

                    if (!pieces.empty())
                    {
                        out.back() = paste_token(out.back().text + pieces.front().text);
                        out.insert(out.end(), pieces.begin() + 1, pieces.end());
                    }

                    ++i;
                    continue;
                }

                // a parameter next to ## on either side keeps its raw argument;
                // everywhere else it takes the fully expanded one
                const bool paste_operand =
                    (i + 1 < body.size() && body[i + 1].text == "##") ||
                    (i > 0 && body[i - 1].text == "##");

                if (const int index = param_index(def, tok.text); index >= 0)
                {
                    const std::vector<PpToken> value =
                        paste_operand ? raw_tokens(index) : expanded_tokens(index);
                    out.insert(out.end(), value.begin(), value.end());
                    continue;
                }

                if (def.variadic && tok.text == "__VA_ARGS__")
                {
                    const std::vector<PpToken> value =
                        paste_operand ? raw_varargs() : expanded_varargs();
                    out.insert(out.end(), value.begin(), value.end());
                    continue;
                }

                out.push_back(tok);
            }

            return out;
        }

        // add the just-expanded macro's name to the hide set of every produced
        // token, folding in the hide set the invocation itself carried
        auto paint(std::vector<PpToken>& tokens, const std::vector<std::string>& carried,
                   const std::string& name) -> void
        {
            for (PpToken& token : tokens)
            {
                for (const std::string& hidden : carried)
                {
                    if (!hidden_by(token, hidden))
                        token.hide.push_back(hidden);
                }

                if (!hidden_by(token, name))
                    token.hide.push_back(name);
            }
        }

        // gather the arguments of a function-like call. open is the index of '(';
        // on success end is set past the matching ')'. returns false if the parens
        // never balance, in which case the name is left as a plain identifier
        auto collect_args(const std::vector<PpToken>& tokens, size_t open, ArgList& args,
                          size_t& end) -> bool
        {
            int depth = 0;
            std::vector<PpToken> current;
            bool any = false;

            for (size_t i = open; i < tokens.size(); ++i)
            {
                const PpToken& tok = tokens[i];

                if (tok.text == "(")
                {
                    ++depth;

                    if (depth == 1)
                        continue;
                }
                else if (tok.text == ")")
                {
                    --depth;

                    if (depth == 0)
                    {
                        if (any || !current.empty())
                            args.push_back(std::move(current));

                        end = i + 1;
                        return true;
                    }
                }
                else if (tok.text == "," && depth == 1)
                {
                    args.push_back(std::move(current));
                    current.clear();
                    any = true;
                    continue;
                }

                current.push_back(tok);
                any = true;
            }

            return false;
        }

        // whether a call supplies enough arguments for a definition. a zero-arg
        // macro invoked as NAME() lexes as a single empty argument, so treat that
        // as a match
        auto arity_ok(const MacroDef& def, const ArgList& args) -> bool
        {
            const size_t provided =
                args.size() == 1 && args.front().empty() ? 0 : args.size();

            if (def.variadic)
                return provided >= def.params.size();

            return provided == def.params.size();
        }

        // one expansion layer: every eligible macro name in the stream is replaced
        // once, and the tokens it produced are left for the next layer to rescan.
        // that "one layer per pass" rule is what the level slider steps through
        auto expand_pass(const MacroTable& table, const std::vector<PpToken>& in,
                         std::vector<PpToken>& out, std::vector<std::string>& expanded) -> bool
        {
            bool changed = false;
            size_t i = 0;

            while (i < in.size())
            {
                const PpToken& tok = in[i];

                if (tok.kind != PpKind::Identifier || hidden_by(tok, tok.text))
                {
                    out.push_back(tok);
                    ++i;
                    continue;
                }

                const MacroDef* def = table.find(tok.text);

                if (def == nullptr)
                {
                    out.push_back(tok);
                    ++i;
                    continue;
                }

                if (!def->function_like)
                {
                    std::vector<PpToken> repl = substitute(*def, {}, {});
                    paint(repl, tok.hide, def->name);
                    out.insert(out.end(), repl.begin(), repl.end());
                    expanded.push_back(def->name);
                    changed = true;
                    ++i;
                    continue;
                }

                const size_t paren = i + 1;

                if (paren >= in.size() || in[paren].text != "(")
                {
                    out.push_back(tok);
                    ++i;
                    continue;
                }

                ArgList args;
                size_t end = 0;

                if (!collect_args(in, paren, args, end) || !arity_ok(*def, args))
                {
                    out.push_back(tok);
                    ++i;
                    continue;
                }

                ArgList expanded_args;
                expanded_args.reserve(args.size());

                for (const std::vector<PpToken>& arg : args)
                    expanded_args.push_back(expand_full(table, arg));

                std::vector<PpToken> repl = substitute(*def, args, expanded_args);

                std::vector<std::string> carried = tok.hide;
                const std::vector<std::string>& close_hide = in[end - 1].hide;

                // function-like painting keeps only the names hidden at both the
                // name and the closing paren, then adds this macro
                std::erase_if(carried, [&](const std::string& name)
                {
                    return std::ranges::find(close_hide, name) == close_hide.end();
                });

                paint(repl, carried, def->name);
                out.insert(out.end(), repl.begin(), repl.end());
                expanded.push_back(def->name);
                changed = true;
                i = end;
            }

            return changed && out.size() < MAX_TOKENS;
        }

        auto expand_full(const MacroTable& table, std::vector<PpToken> tokens)
            -> std::vector<PpToken>
        {
            for (int level = 0; level < 64; ++level)
            {
                std::vector<PpToken> next;
                std::vector<std::string> expanded;

                if (!expand_pass(table, tokens, next, expanded) || expanded.empty())
                    break;

                tokens = std::move(next);

                if (tokens.size() >= MAX_TOKENS)
                    break;
            }

            return tokens;
        }
    }

    auto MacroTable::clear() -> void
    {
        m_macros.clear();
        ++m_revision;
    }

    auto MacroTable::find(std::string_view name) const -> const MacroDef*
    {
        const auto it = m_macros.find(std::string(name));
        return it == m_macros.end() ? nullptr : &it->second;
    }

    auto MacroTable::build(const std::vector<std::string>& lines,
                           const std::filesystem::path& base_dir) -> void
    {
        m_macros.clear();

        std::vector<std::filesystem::path> seen;
        scan_lines(lines, base_dir, 0, seen);

        ++m_revision;
    }

    namespace
    {
        // stitch backslash-newline continuations back into single logical lines so
        // a multi-line #define parses as one
        auto splice_lines(const std::vector<std::string>& lines) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::string pending;
            bool continuing = false;

            for (const std::string& line : lines)
            {
                std::string text = line;

                const bool cont = !text.empty() && text.back() == '\\';

                if (cont)
                    text.pop_back();

                if (continuing)
                    pending += text;
                else
                    pending = text;

                if (cont)
                {
                    continuing = true;
                    continue;
                }

                out.push_back(std::move(pending));
                pending.clear();
                continuing = false;
            }

            if (continuing)
                out.push_back(std::move(pending));

            return out;
        }

        auto directive_of(const std::string& line, std::string& rest) -> std::string
        {
            size_t at = 0;

            while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])) != 0)
                ++at;

            if (at >= line.size() || line[at] != '#')
                return {};

            ++at;

            while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])) != 0)
                ++at;

            const size_t start = at;

            while (at < line.size() && is_ident(line[at]))
                ++at;

            std::string word = line.substr(start, at - start);
            rest = line.substr(at);
            return word;
        }

        auto quote_include(const std::string& rest) -> std::string
        {
            const size_t open = rest.find('"');

            if (open == std::string::npos)
                return {};

            const size_t close = rest.find('"', open + 1);

            if (close == std::string::npos)
                return {};

            return rest.substr(open + 1, close - open - 1);
        }

        auto parse_define(const std::string& rest, MacroDef& def) -> bool
        {
            size_t at = 0;

            while (at < rest.size() && std::isspace(static_cast<unsigned char>(rest[at])) != 0)
                ++at;

            if (at >= rest.size() || !is_ident_start(rest[at]))
                return false;

            const size_t name_start = at;

            while (at < rest.size() && is_ident(rest[at]))
                ++at;

            def.name = rest.substr(name_start, at - name_start);

            // a '(' touching the name with no space makes it function-like; a
            // space means the paren is part of the body
            if (at < rest.size() && rest[at] == '(')
            {
                def.function_like = true;
                ++at;

                while (true)
                {
                    while (at < rest.size() &&
                           std::isspace(static_cast<unsigned char>(rest[at])) != 0)
                        ++at;

                    if (at < rest.size() && rest[at] == ')')
                    {
                        ++at;
                        break;
                    }

                    if (rest.compare(at, 3, "...") == 0)
                    {
                        def.variadic = true;
                        at += 3;
                        continue;
                    }

                    if (at < rest.size() && is_ident_start(rest[at]))
                    {
                        const size_t p = at;

                        while (at < rest.size() && is_ident(rest[at]))
                            ++at;

                        std::string param = rest.substr(p, at - p);

                        // a named variadic (GNU args...) collapses onto the
                        // standard __VA_ARGS__ handling
                        if (rest.compare(at, 3, "...") == 0)
                        {
                            def.variadic = true;
                            at += 3;
                        }
                        else
                        {
                            def.params.push_back(std::move(param));
                        }

                        continue;
                    }

                    if (at < rest.size() && rest[at] == ',')
                    {
                        ++at;
                        continue;
                    }

                    // something unexpected in the parameter list; give up on this
                    // definition rather than guess
                    return false;
                }
            }

            def.body = tokenize(std::string_view(rest).substr(at));
            return true;
        }
    }

    auto MacroTable::scan_lines(const std::vector<std::string>& lines,
                                const std::filesystem::path& base_dir, int depth,
                                std::vector<std::filesystem::path>& seen) -> void
    {
        const std::vector<std::string> logical = splice_lines(lines);

        for (const std::string& line : logical)
        {
            std::string rest;
            const std::string directive = directive_of(line, rest);

            if (directive == "define")
            {
                MacroDef def;

                if (parse_define(rest, def))
                    m_macros[def.name] = std::move(def);
            }
            else if (directive == "undef")
            {
                MacroDef tmp;

                if (parse_define(rest, tmp))
                    m_macros.erase(tmp.name);
            }
            else if (directive == "include" && depth < MAX_INCLUDE_DEPTH && !base_dir.empty())
            {
                const std::string name = quote_include(rest);

                if (!name.empty())
                    scan_path(base_dir / name, depth + 1, seen);
            }
        }
    }

    auto MacroTable::scan_path(const std::filesystem::path& path, int depth,
                               std::vector<std::filesystem::path>& seen) -> void
    {
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
        const std::filesystem::path key = error ? path : canonical;

        if (std::ranges::find(seen, key) != seen.end())
            return;

        seen.push_back(key);

        std::ifstream file(path);

        if (!file.is_open())
            return;

        std::vector<std::string> lines;
        std::string line;

        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            lines.push_back(std::move(line));
        }

        scan_lines(lines, path.parent_path(), depth, seen);
    }

    auto expand_stages(const MacroTable& table, std::string_view input, int max_levels)
        -> MacroExpansion
    {
        MacroExpansion result;

        std::vector<PpToken> tokens = tokenize(input);

        result.levels.push_back(render(tokens));
        result.expanded.emplace_back();

        for (int level = 0; level < max_levels; ++level)
        {
            std::vector<PpToken> next;
            std::vector<std::string> expanded;

            if (!expand_pass(table, tokens, next, expanded) || expanded.empty())
            {
                result.m_reached_fixpoint = true;
                return result;
            }

            tokens = std::move(next);
            result.levels.push_back(render(tokens));
            result.expanded.push_back(std::move(expanded));

            if (tokens.size() >= MAX_TOKENS)
            {
                result.m_reached_fixpoint = false;
                return result;
            }
        }

        // ran out of levels before settling; the last one may still expand further
        result.m_reached_fixpoint = false;
        return result;
    }
}
