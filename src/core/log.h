#pragma once

#include <cstdint>
#include <format>
#include <iostream>
#include <ostream>
#include <print>
#include <string_view>
#include <utility>

namespace Hsdbg::Log
{
    enum class Level : uint8_t
    {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
    };

    namespace Detail
    {
        inline Level g_min_level =
#ifdef HSDBG_DIST
            Level::Info;
#else
            Level::Trace;
#endif

        constexpr std::string_view RESET = "\x1b[0m";
        constexpr std::string_view DIM = "\x1b[90m";

        inline auto label(Level level) -> std::string_view
        {
            switch (level)
            {
                case Level::Trace: return "TRACE";
                case Level::Debug: return "DEBUG";
                case Level::Info:  return "INFO ";
                case Level::Warn:  return "WARN ";
                case Level::Error: return "ERROR";
            }

            return "?????";
        }

        inline auto color(Level level) -> std::string_view
        {
            switch (level)
            {
                case Level::Trace: return "\x1b[1;90m";
                case Level::Debug: return "\x1b[1;35m";
                case Level::Info:  return "\x1b[1;36m";
                case Level::Warn:  return "\x1b[1;33m";
                case Level::Error: return "\x1b[1;31m";
            }

            return "";
        }

        // true only when the stream is a terminal that understands ansi escapes,
        // which needs platform calls, so it lives in log.cpp
        auto is_terminal(bool to_stderr) -> bool;

        inline auto write(Level level, std::string_view message) -> void
        {
            if (level < g_min_level)
                return;

            const bool to_stderr = level >= Level::Warn;
            std::ostream& stream = to_stderr ? std::cerr : std::cout;

            if (is_terminal(to_stderr))
            {
                std::println(stream,
                             "{}{}{} {}│{} {}",
                             color(level), label(level), RESET,
                             DIM, RESET,
                             message);
            }
            else
            {
                std::println(stream, "{} | {}", label(level), message);
            }

            // never let a crash swallow the last lines that explain it
            stream.flush();
        }
    }

    inline auto set_min_level(Level level) -> void
    {
        Detail::g_min_level = level;
    }

    inline auto min_level() -> Level
    {
        return Detail::g_min_level;
    }

    template <typename... Args>
    auto trace(std::format_string<Args...> fmt, Args&&... args) -> void
    {
        Detail::write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    auto debug(std::format_string<Args...> fmt, Args&&... args) -> void
    {
        Detail::write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    auto info(std::format_string<Args...> fmt, Args&&... args) -> void
    {
        Detail::write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    auto warn(std::format_string<Args...> fmt, Args&&... args) -> void
    {
        Detail::write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    auto error(std::format_string<Args...> fmt, Args&&... args) -> void
    {
        Detail::write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
    }
}
