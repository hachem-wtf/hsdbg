#include "core/window_manager.h"

#include "core/assert.h"
#include "core/log.h"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace Hsdbg
{
    WindowManager* WindowManager::s_instance = nullptr;

    WindowManager::WindowManager()
    {
        HSDBG_ASSERT(s_instance == nullptr, "a WindowManager instance already exists");
        s_instance = this;

        glfwSetErrorCallback(on_error);

        const bool initialized = glfwInit() == GLFW_TRUE;
        HSDBG_ASSERT(initialized, "failed to initialize glfw");

        Log::info("glfw {}", glfwGetVersionString());
    }

    WindowManager::~WindowManager()
    {
        m_windows.clear();

        glfwTerminate();

        s_instance = nullptr;
    }

    auto WindowManager::get() -> WindowManager&
    {
        HSDBG_ASSERT(s_instance != nullptr, "no WindowManager instance has been created");
        return *s_instance;
    }

    auto WindowManager::create_window(const WindowSpec& spec) -> Window&
    {
        return *m_windows.emplace_back(std::make_unique<Window>(spec));
    }

    auto WindowManager::destroy_window(const Window& window) -> void
    {
        const auto entry = std::ranges::find_if(m_windows, [&window](const auto& candidate)
        {
            return candidate.get() == &window;
        });

        if (entry == m_windows.end())
        {
            Log::warn("tried to destroy a window that this manager does not own");
            return;
        }

        m_windows.erase(entry);
    }

    auto WindowManager::poll_events() -> void
    {
        glfwPollEvents();
    }

    auto WindowManager::wait_events(double timeout_seconds) -> void
    {
        glfwWaitEventsTimeout(timeout_seconds);
    }

    auto WindowManager::on_error(int code, const char* description) -> void
    {
        Log::error("glfw error {}: {}", code, description);
    }
}
