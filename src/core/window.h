#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct GLFWwindow;

namespace Hsdbg
{
    struct WindowSpec
    {
        std::string title = "hsdbg";
        uint32_t width = 1600;
        uint32_t height = 900;
        bool vsync = true;
    };

    class Window
    {
    public:
        explicit Window(const WindowSpec& spec);
        ~Window();

        Window(const Window&) = delete;
        Window(Window&&) = delete;
        auto operator=(const Window&) -> Window& = delete;
        auto operator=(Window&&) -> Window& = delete;

        auto should_close() const -> bool;
        auto set_should_close(bool value) -> void;

        auto make_context_current() -> void;
        auto swap_buffers() -> void;

        auto set_vsync(bool enabled) -> void;
        auto set_title(std::string_view title) -> void;

        auto handle() const -> GLFWwindow* { return m_handle; }
        auto title() const -> const std::string& { return m_title; }
        auto is_vsync() const -> bool { return m_vsync; }
        auto is_minimized() const -> bool;

        auto width() const -> uint32_t { return m_width; }
        auto height() const -> uint32_t { return m_height; }
        auto framebuffer_width() const -> uint32_t { return m_framebuffer_width; }
        auto framebuffer_height() const -> uint32_t { return m_framebuffer_height; }

    private:
        static auto on_window_size(GLFWwindow* handle, int width, int height) -> void;
        static auto on_framebuffer_size(GLFWwindow* handle, int width, int height) -> void;

        GLFWwindow* m_handle = nullptr;
        std::string m_title;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_framebuffer_width = 0;
        uint32_t m_framebuffer_height = 0;
        bool m_vsync = true;
    };
}
