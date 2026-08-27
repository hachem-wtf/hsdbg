#pragma once

#include <cstdint>
#include <format>
#include <iostream>
#include <ostream>
#include <print>
#include <string_view>
#include <utility>

// there is no standard way to ask whether a stream is a terminal
#ifdef HSDBG_WINDOWS
    #include <io.h>
#else
    #include <unistd.h>
#endif

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

        inline auto label(Level level) -> std::string_view
        {
            switch (level)
            {
                case Level::Trace: return "trace";
                case Level::Debug: return "debug";
                case Level::Info:  return "info ";
                case Level::Warn:  return "warn ";
                case Level::Error: return "error";
            }

            return "?????";
        }

        inline auto color(Level level) -> std::string_view
        {
            switch (level)
            {
                case Level::Trace: return "\x1b[90m";
                case Level::Debug: return "\x1b[36m";
                case Level::Info:  return "\x1b[32m";
                case Level::Warn:  return "\x1b[33m";
                case Level::Error: return "\x1b[31m";
            }

            return "";
        }

        inline auto is_terminal(bool to_stderr) -> bool
        {
            constexpr int STDOUT_DESCRIPTOR = 1;
            constexpr int STDERR_DESCRIPTOR = 2;

            const int descriptor = to_stderr ? STDERR_DESCRIPTOR : STDOUT_DESCRIPTOR;

#ifdef HSDBG_WINDOWS
            return _isatty(descriptor) != 0;
#else
            return isatty(descriptor) != 0;
#endif
        }

        inline auto write(Level level, std::string_view message) -> void
        {
            if (level < g_min_level)
                return;

            const bool to_stderr = level >= Level::Warn;
            std::ostream& stream = to_stderr ? std::cerr : std::cout;

            if (is_terminal(to_stderr))
                std::println(stream, "{}{}\x1b[0m {}", color(level), label(level), message);
            else
                std::println(stream, "{} {}", label(level), message);

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
