#include "core/application.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/window.h"
#include "core/window_manager.h"

#include <glad/gl.h>

#include <chrono>

namespace Hsdbg
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr float MAX_DELTA_TIME = 0.25f;

        // a minimized window has no framebuffer to draw into, so idle instead of
        // burning a core on frames nobody can see
        constexpr double MINIMIZED_WAIT_TIMEOUT = 0.1;
    }

    Application* Application::s_instance = nullptr;

    Application::Application(const ApplicationSpec& spec)
        : m_spec(spec)
    {
        HSDBG_ASSERT(s_instance == nullptr, "an Application instance already exists");
        s_instance = this;

        Log::info("{}: starting up", m_spec.name);

        m_window_manager = std::make_unique<WindowManager>();

        WindowSpec window_spec;
        window_spec.title = m_spec.name;
        window_spec.width = m_spec.width;
        window_spec.height = m_spec.height;
        window_spec.vsync = m_spec.vsync;

        m_window = &m_window_manager->create_window(window_spec);
    }

    Application::~Application()
    {
        m_window = nullptr;
        m_window_manager.reset();

        Log::info("{}: shutting down after {} frames", m_spec.name, m_frame_count);

        s_instance = nullptr;
    }

    auto Application::get() -> Application&
    {
        HSDBG_ASSERT(s_instance != nullptr, "no Application instance has been created");
        return *s_instance;
    }

    auto Application::run() -> void
    {
        m_running = true;

        auto last_frame_time = Clock::now();

        while (m_running)
        {
            const auto now = Clock::now();
            const float elapsed = std::chrono::duration<float>(now - last_frame_time).count();
            last_frame_time = now;

            m_delta_time = elapsed < MAX_DELTA_TIME ? elapsed : MAX_DELTA_TIME;
            m_time += m_delta_time;
            m_frame_count += 1;

            m_window_manager->poll_events();

            if (m_window->should_close())
            {
                close();
                break;
            }

            if (m_window->is_minimized())
            {
                m_window_manager->wait_events(MINIMIZED_WAIT_TIMEOUT);
                continue;
            }

            update();
            render();

            m_window->swap_buffers();
        }
    }

    auto Application::close() -> void
    {
        m_running = false;
    }

    auto Application::update() -> void
    {
    }

    auto Application::render() -> void
    {
        glViewport(0,
                   0,
                   static_cast<GLsizei>(m_window->framebuffer_width()),
                   static_cast<GLsizei>(m_window->framebuffer_height()));

        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
}
