#pragma once

#include "core/window.h"

#include <memory>
#include <vector>

namespace Hsdbg
{
    class WindowManager
    {
    public:
        WindowManager();
        ~WindowManager();

        WindowManager(const WindowManager&) = delete;
        WindowManager(WindowManager&&) = delete;
        auto operator=(const WindowManager&) -> WindowManager& = delete;
        auto operator=(WindowManager&&) -> WindowManager& = delete;

        static auto get() -> WindowManager&;

        auto create_window(const WindowSpec& spec) -> Window&;
        auto destroy_window(const Window& window) -> void;

        static auto poll_events() -> void;
        static auto wait_events(double timeout_seconds) -> void;

        auto window_count() const -> size_t { return m_windows.size(); }

    private:
        static auto on_error(int code, const char* description) -> void;

        static WindowManager* s_instance;

        std::vector<std::unique_ptr<Window>> m_windows;
    };
}
