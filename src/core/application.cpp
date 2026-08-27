#include "core/application.h"

#include "core/assert.h"
#include "core/log.h"

#include <chrono>

namespace Hsdbg
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr float MAX_DELTA_TIME = 0.25f;

        // the run loop has nothing to block on until the window owns it, so it is
        // time boxed for now to keep the app from spinning forever
        constexpr float PLACEHOLDER_RUN_TIME = 1.0f;
    }

    Application* Application::s_instance = nullptr;

    Application::Application(const ApplicationSpec& spec)
        : m_spec(spec)
    {
        HSDBG_ASSERT(s_instance == nullptr, "an Application instance already exists");
        s_instance = this;

        Log::info("{}: starting up", m_spec.name);
    }

    Application::~Application()
    {
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

            update();
        }
    }

    auto Application::close() -> void
    {
        m_running = false;
    }

    auto Application::update() -> void
    {
        if (m_time >= PLACEHOLDER_RUN_TIME)
            close();
    }
}
