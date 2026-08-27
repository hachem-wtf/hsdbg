#pragma once

#include <expected>
#include <format>
#include <string>
#include <utility>

namespace Hsdbg
{
    template <typename T>
    using Result = std::expected<T, std::string>;

    template <typename... Args>
    auto fail(std::format_string<Args...> fmt, Args&&... args) -> std::unexpected<std::string>
    {
        return std::unexpected(std::format(fmt, std::forward<Args>(args)...));
    }
}
