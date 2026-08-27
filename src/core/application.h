#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Hsdbg
{
    class Debugger;
    class Window;
    class WindowManager;

    struct ApplicationSpec
    {
        std::string name = "hsdbg";
        uint32_t width = 1600;
        uint32_t height = 900;
        bool vsync = true;
    };

    class Application
    {
    public:
        explicit Application(const ApplicationSpec& spec);
        ~Application();

        Application(const Application&) = delete;
        Application(Application&&) = delete;
        auto operator=(const Application&) -> Application& = delete;
        auto operator=(Application&&) -> Application& = delete;

        static auto get() -> Application&;

        auto run() -> void;
        auto close() -> void;

        auto window() -> Window& { return *m_window; }
        auto debugger() -> Debugger& { return *m_debugger; }
        auto spec() const -> const ApplicationSpec& { return m_spec; }
        auto is_running() const -> bool { return m_running; }
        auto delta_time() const -> float { return m_delta_time; }
        auto time() const -> float { return m_time; }
        auto frame_count() const -> uint64_t { return m_frame_count; }

    private:
        auto update() -> void;
        auto render() -> void;

        static Application* s_instance;

        ApplicationSpec m_spec;
        std::unique_ptr<WindowManager> m_window_manager;
        std::unique_ptr<Debugger> m_debugger;
        Window* m_window = nullptr;
        bool m_running = false;
        float m_delta_time = 0.0f;
        float m_time = 0.0f;
        uint64_t m_frame_count = 0;
    };
}
