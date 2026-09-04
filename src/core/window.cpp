#include "core/window.h"

#include "core/assert.h"
#include "core/log.h"

#include <glad/gl.h>

#include <GLFW/glfw3.h>

namespace Hsdbg
{
    namespace
    {
        constexpr int GL_VERSION_MAJOR = 3;
        constexpr int GL_VERSION_MINOR = 3;

        bool g_glad_loaded = false;
    }

    Window::Window(const WindowSpec& spec)
        : m_title(spec.title)
        , m_width(spec.width)
        , m_height(spec.height)
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_VERSION_MAJOR);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_VERSION_MINOR);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

        m_handle = glfwCreateWindow(static_cast<int>(m_width),
                                    static_cast<int>(m_height),
                                    m_title.c_str(),
                                    nullptr,
                                    nullptr);

        HSDBG_ASSERT(m_handle != nullptr, "failed to create the glfw window");

        glfwSetWindowUserPointer(m_handle, this);
        glfwSetWindowSizeCallback(m_handle, on_window_size);
        glfwSetFramebufferSizeCallback(m_handle, on_framebuffer_size);

        make_context_current();
        set_vsync(spec.vsync);

        if (!g_glad_loaded)
        {
            const int version = gladLoadGL(glfwGetProcAddress);
            HSDBG_ASSERT(version != 0, "failed to load the opengl function pointers");

            g_glad_loaded = true;

            Log::info("opengl {}.{}", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));
            Log::info("  vendor:   {}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
            Log::info("  renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
            Log::info("  version:  {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        }

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(m_handle, &framebuffer_width, &framebuffer_height);

        m_framebuffer_width = static_cast<uint32_t>(framebuffer_width);
        m_framebuffer_height = static_cast<uint32_t>(framebuffer_height);

        Log::info("window '{}' created at {}x{} ({}x{} framebuffer)",
                  m_title,
                  m_width,
                  m_height,
                  m_framebuffer_width,
                  m_framebuffer_height);
    }

    Window::~Window()
    {
        if (m_handle == nullptr)
            return;

        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
    }

    auto Window::should_close() const -> bool
    {
        return glfwWindowShouldClose(m_handle) == GLFW_TRUE;
    }

    auto Window::set_should_close(bool value) -> void
    {
        glfwSetWindowShouldClose(m_handle, value ? GLFW_TRUE : GLFW_FALSE);
    }

    auto Window::make_context_current() -> void
    {
        glfwMakeContextCurrent(m_handle);
    }

    auto Window::swap_buffers() -> void
    {
        glfwSwapBuffers(m_handle);
    }

    auto Window::set_vsync(bool enabled) -> void
    {
        glfwSwapInterval(enabled ? 1 : 0);
        m_vsync = enabled;
    }

    auto Window::set_title(std::string_view window_title) -> void
    {
        m_title = window_title;
        glfwSetWindowTitle(m_handle, m_title.c_str());
    }

    auto Window::is_minimized() const -> bool
    {
        return m_framebuffer_width == 0 || m_framebuffer_height == 0;
    }

    auto Window::on_window_size(GLFWwindow* handle, int width, int height) -> void
    {
        auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle));
        if (window == nullptr)
            return;

        window->m_width = static_cast<uint32_t>(width);
        window->m_height = static_cast<uint32_t>(height);
    }

    auto Window::on_framebuffer_size(GLFWwindow* handle, int width, int height) -> void
    {
        auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle));
        if (window == nullptr)
            return;

        window->m_framebuffer_width = static_cast<uint32_t>(width);
        window->m_framebuffer_height = static_cast<uint32_t>(height);
    }
}
