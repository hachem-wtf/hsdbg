#include "debugger/debugger.h"

#include "core/assert.h"
#include "core/log.h"

#include <lldb/API/LLDB.h>

#include <algorithm>
#include <source_location>

// no standard way to read or write the environment
#include <cstdlib>

namespace Hsdbg
{
    namespace
    {
#ifndef HSDBG_LLVM_PREFIX
    #define HSDBG_LLVM_PREFIX ""
#endif

        auto set_environment(const char* name, const std::string& value) -> void
        {
#ifdef HSDBG_WINDOWS
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        }

        // lldb spawns a helper to control the inferior on unix. windows needs none,
        // linux ships lldb-server beside liblldb, and macos requires one entitled
        // with com.apple.private.cs.debugger, which only the xcode copy carries
        auto debug_server_candidates() -> std::vector<std::filesystem::path>
        {
            const std::filesystem::path llvm_prefix(HSDBG_LLVM_PREFIX);

#if defined(HSDBG_MACOS)
            return {
                "/Applications/Xcode.app/Contents/SharedFrameworks/LLDB.framework/Versions/A/"
                "Resources/debugserver",
                "/Library/Developer/CommandLineTools/Library/PrivateFrameworks/LLDB.framework/"
                "Resources/debugserver",
                llvm_prefix / "bin" / "debugserver",
            };
#elif defined(HSDBG_LINUX)
            return { llvm_prefix / "bin" / "lldb-server" };
#else
            return {};
#endif
        }

        auto adopt_debug_server() -> void
        {
            const std::vector<std::filesystem::path> candidates = debug_server_candidates();

            if (candidates.empty())
                return;

            if (std::getenv("LLDB_DEBUGSERVER_PATH") != nullptr)
                return;

            for (const std::filesystem::path& candidate : candidates)
            {
                std::error_code error;

                if (!std::filesystem::exists(candidate, error))
                    continue;

                set_environment("LLDB_DEBUGSERVER_PATH", candidate.string());
                Log::debug("debugger: using debug server '{}'", candidate.string());

                return;
            }

            Log::warn("debugger: no debug server found, launching a target will fail");
        }

        auto read_back(lldb::SBTarget& target, lldb::SBBreakpoint& source, Breakpoint& breakpoint) -> void
        {
            breakpoint.enabled = source.IsEnabled();
            breakpoint.hit_count = source.GetHitCount();

            // lldb only calls a location resolved once a process has it mapped, but
            // for the ui the question is whether it found somewhere to put it at all
            breakpoint.resolved = source.GetNumLocations() > 0;

            if (source.GetNumLocations() == 0)
            {
                breakpoint.address = 0;
                return;
            }

            lldb::SBBreakpointLocation location = source.GetLocationAtIndex(0);
            lldb::SBAddress address = location.GetAddress();

            const lldb::addr_t load_address = address.GetLoadAddress(target);
            breakpoint.address = load_address != LLDB_INVALID_ADDRESS ? load_address
                                                                      : address.GetFileAddress();

            lldb::SBSymbolContext context = address.GetSymbolContext(lldb::eSymbolContextEverything);

            if (const char* name = context.GetFunction().GetName(); name != nullptr)
                breakpoint.function = name;
            else if (const char* symbol = context.GetSymbol().GetName(); symbol != nullptr)
                breakpoint.function = symbol;

            lldb::SBLineEntry line_entry = context.GetLineEntry();

            if (breakpoint.line != 0 && line_entry.IsValid())
                breakpoint.line = line_entry.GetLine();
        }

        // every call site reports itself, so the stubs stay one line each until
        // there is an lldb session behind them
        auto not_implemented(std::source_location location = std::source_location::current())
            -> std::unexpected<std::string>
        {
            Log::warn("{} is not implemented yet", location.function_name());
            return fail("{} is not implemented yet", location.function_name());
        }
    }

    struct Debugger::Session
    {
        lldb::SBDebugger debugger;
        lldb::SBTarget target;
    };

    Debugger::Debugger()
        : m_session(std::make_unique<Session>())
    {
        adopt_debug_server();

        lldb::SBDebugger::Initialize();

        m_session->debugger = lldb::SBDebugger::Create();
        m_session->debugger.SetAsync(true);

        HSDBG_ASSERT(m_session->debugger.IsValid(), "failed to create the lldb debugger");

        Log::info("debugger: {}", lldb::SBDebugger::GetVersionString());
    }

    Debugger::~Debugger()
    {
        unload_target();

        if (m_session->debugger.IsValid())
            lldb::SBDebugger::Destroy(m_session->debugger);

        m_session.reset();

        lldb::SBDebugger::Terminate();
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

        lldb::SBError create_error;
        lldb::SBTarget target = m_session->debugger.CreateTarget(m_target_path.string().c_str(),
                                                                 nullptr,
                                                                 nullptr,
                                                                 true,
                                                                 create_error);

        if (!target.IsValid())
        {
            const char* reason = create_error.GetCString();

            m_target_path.clear();

            return fail("could not load '{}': {}",
                        executable.string(),
                        reason != nullptr ? reason : "unknown error");
        }

        if (m_session->target.IsValid())
            m_session->debugger.DeleteTarget(m_session->target);

        m_session->target = target;

        m_process_id = 0;
        m_stop_reason = StopReason::None;
        set_state(TargetState::Loaded);

        const char* triple = m_session->target.GetTriple();

        Log::info("debugger: loaded target '{}' ({})",
                  m_target_path.string(),
                  triple != nullptr ? triple : "unknown triple");

        // breakpoints set before a target existed are only requests until now
        for (Breakpoint& breakpoint : m_breakpoints)
            resolve_breakpoint(breakpoint);

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
            breakpoint.backend_id = 0;
            breakpoint.resolved = false;
            breakpoint.hit_count = 0;
            breakpoint.address = 0;
        }

        if (m_session->target.IsValid())
        {
            m_session->target.DeleteAllBreakpoints();
            m_session->debugger.DeleteTarget(m_session->target);
            m_session->target = lldb::SBTarget();
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
        resolve_breakpoint(m_breakpoints.back());

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
        resolve_breakpoint(m_breakpoints.back());

        Log::info("debugger: breakpoint {} at {}()", m_breakpoints.back().id, function);

        return m_breakpoints.back().id;
    }

    auto Debugger::remove_breakpoint(uint32_t id) -> bool
    {
        const auto entry = std::ranges::find(m_breakpoints, id, &Breakpoint::id);
        if (entry == m_breakpoints.end())
            return false;

        if (entry->backend_id != 0 && m_session->target.IsValid())
            m_session->target.BreakpointDelete(entry->backend_id);

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

        if (breakpoint->backend_id != 0 && m_session->target.IsValid())
        {
            lldb::SBBreakpoint source = m_session->target.FindBreakpointByID(breakpoint->backend_id);

            if (source.IsValid())
                source.SetEnabled(enabled);
        }

        return true;
    }

    auto Debugger::clear_breakpoints() -> void
    {
        if (m_session->target.IsValid())
            m_session->target.DeleteAllBreakpoints();

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
        sync_breakpoints();
    }

    auto Debugger::resolve_breakpoint(Breakpoint& breakpoint) -> void
    {
        if (!m_session->target.IsValid())
            return;

        lldb::SBBreakpoint created;

        if (breakpoint.line == 0)
        {
            // unscoped, a name like main matches every module the target pulls in
            created = m_session->target.BreakpointCreateByName(
                breakpoint.function.c_str(), m_session->target.GetExecutable().GetFilename());
        }
        else
        {
            created = m_session->target.BreakpointCreateByLocation(breakpoint.file.string().c_str(),
                                                                   breakpoint.line);

            // debug info records whatever path the compiler saw, which rarely
            // matches what the user opened, so fall back to the file name alone
            if (created.GetNumLocations() == 0 && breakpoint.file.has_parent_path())
            {
                m_session->target.BreakpointDelete(created.GetID());

                created = m_session->target.BreakpointCreateByLocation(
                    breakpoint.file.filename().string().c_str(), breakpoint.line);
            }
        }

        if (!created.IsValid())
        {
            Log::warn("debugger: lldb refused breakpoint {}", breakpoint.id);
            return;
        }

        created.SetEnabled(breakpoint.enabled);

        breakpoint.backend_id = created.GetID();

        read_back(m_session->target, created, breakpoint);

        Log::debug("debugger: breakpoint {} {} ({} locations)",
                   breakpoint.id,
                   breakpoint.resolved ? "resolved" : "pending",
                   created.GetNumLocations());
    }

    auto Debugger::sync_breakpoints() -> void
    {
        if (!m_session->target.IsValid())
            return;

        for (Breakpoint& breakpoint : m_breakpoints)
        {
            if (breakpoint.backend_id == 0)
                continue;

            lldb::SBBreakpoint source = m_session->target.FindBreakpointByID(breakpoint.backend_id);
            if (!source.IsValid())
                continue;

            read_back(m_session->target, source, breakpoint);
        }
    }

    auto Debugger::set_state(TargetState state) -> void
    {
        if (m_state == state)
            return;

        Log::debug("debugger: {} -> {}", to_string(m_state), to_string(state));

        m_state = state;
    }
}
