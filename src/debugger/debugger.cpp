#include "debugger/debugger.h"

#include "core/log.h"

#include <algorithm>
#include <source_location>

namespace Hsdbg
{
    namespace
    {
        // every call site reports itself, so the stubs stay one line each until
        // there is an lldb session behind them
        auto not_implemented(std::source_location location = std::source_location::current())
            -> std::unexpected<std::string>
        {
            Log::warn("{} is not implemented yet", location.function_name());
            return fail("{} is not implemented yet", location.function_name());
        }
    }

    Debugger::Debugger()
    {
        Log::info("debugger: ready");
    }

    Debugger::~Debugger()
    {
        unload_target();
    }

    auto Debugger::load_target(const std::filesystem::path& executable) -> Result<void>
    {
        std::error_code error;

        if (!std::filesystem::exists(executable, error))
            return fail("'{}' does not exist", executable.string());

        if (!std::filesystem::is_regular_file(executable, error))
            return fail("'{}' is not a file", executable.string());

        m_target_path = std::filesystem::absolute(executable, error);
        if (error)
            m_target_path = executable;

        m_process_id = 0;
        m_stop_reason = StopReason::None;
        set_state(TargetState::Loaded);

        Log::info("debugger: loaded target '{}'", m_target_path.string());

        return {};
    }

    auto Debugger::unload_target() -> void
    {
        m_target_path.clear();
        m_process_id = 0;
        m_stop_reason = StopReason::None;

        m_threads.clear();
        m_call_stack.clear();
        m_locals.clear();
        m_registers.clear();
        m_console_output.clear();

        m_selected_thread = 0;
        m_selected_frame = 0;

        for (Breakpoint& breakpoint : m_breakpoints)
        {
            breakpoint.resolved = false;
            breakpoint.hit_count = 0;
            breakpoint.address = 0;
        }

        set_state(TargetState::NoTarget);
    }

    auto Debugger::launch(const LaunchSpec& spec) -> Result<void>
    {
        (void)spec;
        return not_implemented();
    }

    auto Debugger::attach(uint64_t process_id) -> Result<void>
    {
        (void)process_id;
        return not_implemented();
    }

    auto Debugger::detach() -> Result<void>
    {
        return not_implemented();
    }

    auto Debugger::terminate() -> Result<void>
    {
        return not_implemented();
    }

    auto Debugger::resume() -> Result<void>
    {
        return not_implemented();
    }

    auto Debugger::pause() -> Result<void>
    {
        return not_implemented();
    }

    auto Debugger::step_over(StepMode mode) -> Result<void>
    {
        (void)mode;
        return not_implemented();
    }

    auto Debugger::step_into(StepMode mode) -> Result<void>
    {
        (void)mode;
        return not_implemented();
    }

    auto Debugger::step_out() -> Result<void>
    {
        return not_implemented();
    }

    auto Debugger::run_to(const std::filesystem::path& file, uint32_t line) -> Result<void>
    {
        (void)file;
        (void)line;
        return not_implemented();
    }

    auto Debugger::add_breakpoint(const std::filesystem::path& file, uint32_t line) -> uint32_t
    {
        const auto existing = std::ranges::find_if(m_breakpoints, [&](const Breakpoint& candidate)
        {
            return candidate.file == file && candidate.line == line;
        });

        if (existing != m_breakpoints.end())
            return existing->id;

        Breakpoint breakpoint;
        breakpoint.id = m_next_breakpoint_id++;
        breakpoint.file = file;
        breakpoint.line = line;

        m_breakpoints.push_back(std::move(breakpoint));

        Log::info("debugger: breakpoint {} at {}:{}",
                  m_breakpoints.back().id,
                  file.filename().string(),
                  line);

        return m_breakpoints.back().id;
    }

    auto Debugger::add_function_breakpoint(std::string_view function) -> uint32_t
    {
        const auto existing = std::ranges::find_if(m_breakpoints, [&](const Breakpoint& candidate)
        {
            return candidate.function == function && candidate.line == 0;
        });

        if (existing != m_breakpoints.end())
            return existing->id;

        Breakpoint breakpoint;
        breakpoint.id = m_next_breakpoint_id++;
        breakpoint.function = function;

        m_breakpoints.push_back(std::move(breakpoint));

        Log::info("debugger: breakpoint {} at {}()", m_breakpoints.back().id, function);

        return m_breakpoints.back().id;
    }

    auto Debugger::remove_breakpoint(uint32_t id) -> bool
    {
        const auto entry = std::ranges::find(m_breakpoints, id, &Breakpoint::id);
        if (entry == m_breakpoints.end())
            return false;

        m_breakpoints.erase(entry);

        Log::info("debugger: removed breakpoint {}", id);

        return true;
    }

    auto Debugger::set_breakpoint_enabled(uint32_t id, bool enabled) -> bool
    {
        Breakpoint* breakpoint = find_breakpoint(id);
        if (breakpoint == nullptr)
            return false;

        breakpoint->enabled = enabled;

        return true;
    }

    auto Debugger::clear_breakpoints() -> void
    {
        m_breakpoints.clear();
    }

    auto Debugger::find_breakpoint(uint32_t id) -> Breakpoint*
    {
        const auto entry = std::ranges::find(m_breakpoints, id, &Breakpoint::id);

        return entry == m_breakpoints.end() ? nullptr : &*entry;
    }

    auto Debugger::evaluate(std::string_view expression) -> Result<std::string>
    {
        (void)expression;
        return not_implemented();
    }

    auto Debugger::read_memory(uint64_t address, size_t size) -> Result<std::vector<uint8_t>>
    {
        (void)address;
        (void)size;
        return not_implemented();
    }

    auto Debugger::select_thread(uint64_t thread_id) -> bool
    {
        const auto entry = std::ranges::find(m_threads, thread_id, &Thread::id);
        if (entry == m_threads.end())
            return false;

        m_selected_thread = thread_id;
        m_selected_frame = 0;

        return true;
    }

    auto Debugger::select_frame(uint32_t frame_index) -> bool
    {
        if (frame_index >= m_call_stack.size())
            return false;

        m_selected_frame = frame_index;

        return true;
    }

    auto Debugger::update() -> void
    {
    }

    auto Debugger::set_state(TargetState state) -> void
    {
        if (m_state == state)
            return;

        Log::debug("debugger: {} -> {}", to_string(m_state), to_string(state));

        m_state = state;
    }
}
