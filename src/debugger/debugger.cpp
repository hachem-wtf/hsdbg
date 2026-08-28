#include "debugger/debugger.h"

#include "core/assert.h"
#include "core/log.h"

#include <lldb/API/LLDB.h>

#include <algorithm>
#include <array>
#include <set>
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
            return {
                llvm_prefix / "bin" / "lldb-server",
                "/usr/bin/lldb-server",
                "/usr/local/bin/lldb-server",
            };
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

            Log::warn("debugger: no debug server found, leaving lldb to locate one");
        }

        auto text_or(const char* text, std::string_view fallback) -> std::string
        {
            return text != nullptr && text[0] != '\0' ? std::string(text) : std::string(fallback);
        }

        auto path_of(lldb::SBFileSpec spec) -> std::filesystem::path
        {
            if (!spec.IsValid())
                return {};

            std::array<char, 1024> buffer{};

            if (spec.GetPath(buffer.data(), buffer.size()) == 0)
                return {};

            return std::filesystem::path(buffer.data());
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

            if (!line_entry.IsValid())
                return;

            breakpoint.line = line_entry.GetLine();

            // only a file and line breakpoint was told where it belongs, the rest
            // learn it from wherever lldb landed
            if (breakpoint.by_function || breakpoint.by_address)
                breakpoint.file = path_of(line_entry.GetFileSpec());
        }

        // every call site reports itself, so the stubs stay one line each until
        // there is an lldb session behind them
        auto not_implemented(std::source_location location = std::source_location::current())
            -> std::unexpected<std::string>
        {
            Log::warn("{} is not implemented yet", location.function_name());
            return fail("{} is not implemented yet", location.function_name());
        }

        auto is_alive(lldb::SBProcess& process) -> bool
        {
            if (!process.IsValid())
                return false;

            switch (process.GetState())
            {
                case lldb::eStateInvalid:
                case lldb::eStateUnloaded:
                case lldb::eStateDetached:
                case lldb::eStateExited:
                    return false;
                default:
                    return true;
            }
        }

        auto to_stop_reason(lldb::StopReason reason) -> StopReason
        {
            switch (reason)
            {
                case lldb::eStopReasonBreakpoint:   return StopReason::Breakpoint;
                case lldb::eStopReasonPlanComplete: return StopReason::Step;
                case lldb::eStopReasonSignal:       return StopReason::Signal;
                case lldb::eStopReasonException:    return StopReason::Exception;
                case lldb::eStopReasonWatchpoint:   return StopReason::Watchpoint;
                default:                            return StopReason::None;
            }
        }

        auto message_of(const lldb::SBError& error, const char* fallback) -> const char*
        {
            const char* message = error.GetCString();

            return message != nullptr && message[0] != '\0' ? message : fallback;
        }

        // a local can be a whole object graph, and every level costs a read from
        // the inferior, so stop well before anything recursive gets interesting
        constexpr uint32_t MAX_VARIABLE_DEPTH = 3;
        constexpr uint32_t MAX_VARIABLE_CHILDREN = 64;

        // only used for frames lldb cannot name, where there is no function to
        // bound the listing
        constexpr uint32_t DISASSEMBLY_WINDOW = 64;

        auto frame_of(lldb::SBProcess& process, uint64_t thread_id, uint32_t frame_index) -> lldb::SBFrame
        {
            lldb::SBThread thread = process.GetThreadByID(thread_id);

            if (!thread.IsValid())
                return {};

            return thread.GetFrameAtIndex(frame_index);
        }

        auto instruction_of(lldb::SBTarget& target,
                            lldb::SBInstruction source,
                            lldb::addr_t program_counter) -> Instruction
        {
            Instruction entry;

            if (!source.IsValid())
                return entry;

            lldb::SBAddress address = source.GetAddress();
            const lldb::addr_t load = address.GetLoadAddress(target);

            entry.file_address = address.GetFileAddress();
            entry.address = load != LLDB_INVALID_ADDRESS ? load : entry.file_address;
            entry.mnemonic = text_or(source.GetMnemonic(target), "?");
            entry.operands = text_or(source.GetOperands(target), "");
            entry.comment = text_or(source.GetComment(target), "");
            entry.size = static_cast<uint32_t>(source.GetByteSize());
            entry.current = program_counter != LLDB_INVALID_ADDRESS &&
                            entry.address == program_counter;

            return entry;
        }

        auto instructions_of(lldb::SBTarget& target,
                             lldb::SBInstructionList source,
                             lldb::addr_t program_counter) -> std::vector<Instruction>
        {
            std::vector<Instruction> instructions;

            for (uint32_t index = 0; index < source.GetSize(); ++index)
            {
                Instruction entry = instruction_of(target, source.GetInstructionAtIndex(index), program_counter);

                if (entry.file_address == 0 && entry.mnemonic.empty())
                    continue;

                instructions.push_back(std::move(entry));
            }

            return instructions;
        }

        auto module_symbols(lldb::SBTarget& target) -> std::vector<Symbol>
        {
            std::vector<Symbol> symbols;

            lldb::SBModule module = target.FindModule(target.GetExecutable());

            if (!module.IsValid())
                return symbols;

            struct Candidate
            {
                uint64_t file_address = 0;
                uint64_t size = 0;
                lldb::SBSymbol symbol;
            };

            std::vector<Candidate> candidates;

            for (size_t index = 0; index < module.GetNumSymbols(); ++index)
            {
                lldb::SBSymbol symbol = module.GetSymbolAtIndex(index);

                if (!symbol.IsValid() || symbol.GetType() != lldb::eSymbolTypeCode)
                    continue;

                lldb::SBAddress start = symbol.GetStartAddress();

                if (!start.IsValid())
                    continue;

                candidates.push_back({ start.GetFileAddress(), symbol.GetSize(), symbol });
            }

            std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right)
            {
                if (left.file_address != right.file_address)
                    return left.file_address < right.file_address;

                return left.size > right.size;
            });

            uint64_t previous = LLDB_INVALID_ADDRESS;

            for (Candidate& candidate : candidates)
            {
                if (candidate.file_address == previous)
                    continue;

                previous = candidate.file_address;

                const lldb::addr_t load = candidate.symbol.GetStartAddress().GetLoadAddress(target);

                Symbol entry;
                entry.name = text_or(candidate.symbol.GetDisplayName(),
                                     text_or(candidate.symbol.GetName(), "?"));
                entry.file_address = candidate.file_address;
                entry.address = load != LLDB_INVALID_ADDRESS ? load : candidate.file_address;
                entry.size = candidate.size;

                symbols.push_back(std::move(entry));
            }

            return symbols;
        }

        auto is_system_source(const std::filesystem::path& path) -> bool
        {
            const std::string text = path.generic_string();

            constexpr std::string_view markers[] = {
                "/usr/",
                "/opt/",
                "/Library/",
                "/Applications/",
                "/System/",
                "/c++/v1/",
                "\\Program Files",
                "\\Windows Kits",
            };

            return std::ranges::any_of(markers, [&](std::string_view marker)
            {
                return text.find(marker) != std::string::npos;
            });
        }

        auto normalize_source(const std::filesystem::path& path) -> std::filesystem::path
        {
            std::error_code error;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);

            return error ? path.lexically_normal() : canonical;
        }

        auto module_source_files(lldb::SBTarget& target) -> std::vector<std::filesystem::path>
        {
            std::set<std::filesystem::path> files;

            lldb::SBModule module = target.FindModule(target.GetExecutable());

            if (!module.IsValid())
                return {};

            for (uint32_t index = 0; index < module.GetNumCompileUnits(); ++index)
            {
                lldb::SBCompileUnit unit = module.GetCompileUnitAtIndex(index);

                if (!unit.IsValid())
                    continue;

                if (std::filesystem::path cu = path_of(unit.GetFileSpec()); !cu.empty())
                {
                    cu = normalize_source(cu);

                    if (std::filesystem::is_regular_file(cu))
                        files.insert(cu);
                }

                for (uint32_t support = 0; support < unit.GetNumSupportFiles(); ++support)
                {
                    std::filesystem::path path = path_of(unit.GetSupportFileAtIndex(support));

                    if (path.empty() || is_system_source(path))
                        continue;

                    path = normalize_source(path);

                    if (!std::filesystem::is_regular_file(path))
                        continue;

                    files.insert(std::move(path));
                }
            }

            return { files.begin(), files.end() };
        }

        auto display_of(lldb::SBValue& value) -> std::string
        {
            const char* text = value.GetValue();
            const char* summary = value.GetSummary();

            if (text != nullptr && summary != nullptr)
                return std::format("{} {}", text, summary);

            if (text != nullptr)
                return text;

            return summary != nullptr ? summary : "";
        }

        auto to_variable(lldb::SBValue value, uint32_t depth) -> Variable
        {
            Variable variable;
            variable.name = text_or(value.GetName(), "?");
            variable.type = text_or(value.GetDisplayTypeName(), "");
            variable.value = display_of(value);

            if (depth >= MAX_VARIABLE_DEPTH)
                return variable;

            const uint32_t children = value.GetNumChildren(MAX_VARIABLE_CHILDREN);

            for (uint32_t index = 0; index < children; ++index)
            {
                lldb::SBValue child = value.GetChildAtIndex(index);

                if (child.IsValid())
                    variable.children.push_back(to_variable(child, depth + 1));
            }

            return variable;
        }

        auto name_of(lldb::SBFrame& frame) -> std::string
        {
            if (const char* display = frame.GetDisplayFunctionName(); display != nullptr)
                return display;

            if (const char* name = frame.GetFunctionName(); name != nullptr)
                return name;

            return "??";
        }
    }

    struct Debugger::Session
    {
        lldb::SBDebugger debugger;
        lldb::SBTarget target;
        lldb::SBProcess process;
        lldb::SBListener listener;

        // process output arrives in chunks that do not respect line endings
        std::string partial_output;
    };

    Debugger::Debugger()
        : m_session(std::make_unique<Session>())
    {
        adopt_debug_server();

        lldb::SBDebugger::Initialize();

        m_session->debugger = lldb::SBDebugger::Create();
        m_session->debugger.SetAsync(true);

        HSDBG_ASSERT(m_session->debugger.IsValid(), "failed to create the lldb debugger");

        // async means lldb reports everything through here instead of blocking
        m_session->listener = lldb::SBListener("hsdbg");

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

        m_disassembly.clear();
        m_disassembly_name.clear();
        m_selected_symbol = 0;
        refresh_source_files();
        refresh_disassembly();

        return {};
    }

    auto Debugger::unload_target() -> void
    {
        if (is_alive(m_session->process))
            m_session->process.Kill();

        m_session->process = lldb::SBProcess();
        m_session->partial_output.clear();

        m_target_path.clear();
        m_process_id = 0;
        m_stop_reason = StopReason::None;

        m_threads.clear();
        m_call_stack.clear();
        m_locals.clear();
        m_registers.clear();
        m_symbols.clear();
        m_source_files.clear();
        m_disassembly.clear();
        m_disassembly_name.clear();
        m_console_output.clear();

        m_selected_thread = 0;
        m_selected_frame = 0;
        m_selected_symbol = 0;

        for (Breakpoint& breakpoint : m_breakpoints)
        {
            breakpoint.backend_id = 0;
            breakpoint.resolved = false;
            breakpoint.hit_count = 0;
            breakpoint.address = breakpoint.by_address ? breakpoint.file_address : 0;

            // whatever lldb told us about a named breakpoint belonged to the
            // binary that just went away
            if (breakpoint.by_function || breakpoint.by_address)
            {
                breakpoint.file.clear();
                breakpoint.line = 0;
            }
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
        if (!m_session->target.IsValid())
            return fail("no target loaded");

        if (is_alive(m_session->process))
            return fail("'{}' is already running", m_target_path.filename().string());

        std::vector<const char*> arguments;
        arguments.reserve(spec.arguments.size() + 1);

        for (const std::string& argument : spec.arguments)
            arguments.push_back(argument.c_str());

        arguments.push_back(nullptr);

        lldb::SBLaunchInfo info(arguments.data());
        info.SetListener(m_session->listener);

        if (!spec.environment.empty())
        {
            std::vector<const char*> environment;
            environment.reserve(spec.environment.size() + 1);

            for (const std::string& entry : spec.environment)
                environment.push_back(entry.c_str());

            environment.push_back(nullptr);

            info.SetEnvironmentEntries(environment.data(), true);
        }

        if (!spec.working_directory.empty())
            info.SetWorkingDirectory(spec.working_directory.string().c_str());

        if (spec.stop_at_entry)
            info.SetLaunchFlags(info.GetLaunchFlags() | lldb::eLaunchFlagStopAtEntry);

        // lldb counts an ignore count down as hits are consumed, so a rerun has
        // to start it over or it would only ever skip once
        for (const Breakpoint& breakpoint : m_breakpoints)
        {
            if (breakpoint.backend_id == 0)
                continue;

            lldb::SBBreakpoint source = m_session->target.FindBreakpointByID(breakpoint.backend_id);

            if (source.IsValid())
                source.SetIgnoreCount(breakpoint.ignore_count);
        }

        set_state(TargetState::Launching);

        lldb::SBError error;
        lldb::SBProcess process = m_session->target.Launch(info, error);

        if (error.Fail() || !process.IsValid())
        {
            set_state(TargetState::Loaded);

            return fail("could not launch '{}': {}",
                        m_target_path.filename().string(),
                        message_of(error, "unknown error"));
        }

        m_session->process = process;
        m_process_id = process.GetProcessID();
        m_stop_reason = StopReason::None;

        Log::info("debugger: launched '{}' as pid {}",
                  m_target_path.filename().string(),
                  m_process_id);

        // a launch that runs on is briefly stopped here before lldb lets it go,
        // and reporting that would show the user a stop that never happened
        if (spec.stop_at_entry)
            sync_after_start();
        else
            set_state(TargetState::Running);

        return {};
    }

    auto Debugger::attach(uint64_t process_id) -> Result<void>
    {
        if (is_alive(m_session->process))
            return fail("already attached to pid {}", m_process_id);

        // attaching needs no binary on disk, lldb reads it back off the process
        if (!m_session->target.IsValid())
        {
            m_session->target = m_session->debugger.CreateTarget("");

            if (!m_session->target.IsValid())
                return fail("could not create a target to attach with");
        }

        lldb::SBAttachInfo info(static_cast<lldb::pid_t>(process_id));
        info.SetListener(m_session->listener);

        set_state(TargetState::Launching);

        lldb::SBError error;
        lldb::SBProcess process = m_session->target.Attach(info, error);

        if (error.Fail() || !process.IsValid())
        {
            set_state(m_target_path.empty() ? TargetState::NoTarget : TargetState::Loaded);

            return fail("could not attach to pid {}: {}",
                        process_id,
                        message_of(error, "unknown error"));
        }

        m_session->process = process;
        m_process_id = process.GetProcessID();

        if (m_target_path.empty())
            m_target_path = path_of(m_session->target.GetExecutable());

        for (Breakpoint& breakpoint : m_breakpoints)
        {
            if (breakpoint.backend_id == 0)
                resolve_breakpoint(breakpoint);
        }

        Log::info("debugger: attached to pid {}", m_process_id);

        if (m_source_files.empty())
            refresh_source_files();

        sync_after_start();

        return {};
    }

    auto Debugger::detach() -> Result<void>
    {
        if (!is_alive(m_session->process))
            return fail("nothing to detach from");

        const lldb::SBError error = m_session->process.Detach();

        if (error.Fail())
            return fail("could not detach: {}", message_of(error, "unknown error"));

        Log::info("debugger: detached from pid {}", m_process_id);

        return {};
    }

    auto Debugger::terminate() -> Result<void>
    {
        if (!is_alive(m_session->process))
            return fail("nothing to stop");

        const lldb::SBError error = m_session->process.Kill();

        if (error.Fail())
            return fail("could not stop pid {}: {}", m_process_id, message_of(error, "unknown error"));

        return {};
    }

    auto Debugger::resume() -> Result<void>
    {
        if (!is_alive(m_session->process))
            return fail("no running process");

        if (m_session->process.GetState() == lldb::eStateRunning)
            return fail("already running");

        const lldb::SBError error = m_session->process.Continue();

        if (error.Fail())
            return fail("could not continue: {}", message_of(error, "unknown error"));

        return {};
    }

    auto Debugger::pause() -> Result<void>
    {
        if (!is_alive(m_session->process))
            return fail("no running process");

        if (m_session->process.GetState() == lldb::eStateStopped)
            return fail("already stopped");

        const lldb::SBError error = m_session->process.Stop();

        if (error.Fail())
            return fail("could not pause: {}", message_of(error, "unknown error"));

        return {};
    }

    auto Debugger::step_over(StepMode mode) -> Result<void>
    {
        if (const Result<void> ready = require_stopped(); !ready)
            return ready;

        lldb::SBThread thread = m_session->process.GetSelectedThread();

        if (!thread.IsValid())
            return fail("no thread selected");

        lldb::SBError error;

        if (mode == StepMode::Instruction)
            thread.StepInstruction(true, error);
        else
            thread.StepOver(lldb::eOnlyDuringStepping, error);

        if (error.Fail())
            return fail("could not step over: {}", message_of(error, "unknown error"));

        return {};
    }

    auto Debugger::step_into(StepMode mode) -> Result<void>
    {
        if (const Result<void> ready = require_stopped(); !ready)
            return ready;

        lldb::SBThread thread = m_session->process.GetSelectedThread();

        if (!thread.IsValid())
            return fail("no thread selected");

        lldb::SBError error;

        if (mode == StepMode::Instruction)
            thread.StepInstruction(false, error);
        else
            thread.StepInto(nullptr, LLDB_INVALID_LINE_NUMBER, error, lldb::eOnlyDuringStepping);

        if (error.Fail())
            return fail("could not step into: {}", message_of(error, "unknown error"));

        return {};
    }

    auto Debugger::step_out() -> Result<void>
    {
        if (const Result<void> ready = require_stopped(); !ready)
            return ready;

        lldb::SBThread thread = m_session->process.GetSelectedThread();

        if (!thread.IsValid())
            return fail("no thread selected");

        lldb::SBError error;
        thread.StepOut(error);

        if (error.Fail())
            return fail("could not step out: {}", message_of(error, "unknown error"));

        return {};
    }

    auto Debugger::run_to(const std::filesystem::path& file, uint32_t line) -> Result<void>
    {
        if (const Result<void> ready = require_stopped(); !ready)
            return ready;

        lldb::SBThread thread = m_session->process.GetSelectedThread();

        if (!thread.IsValid())
            return fail("no thread selected");

        lldb::SBFrame frame = thread.GetFrameAtIndex(m_selected_frame);

        if (!frame.IsValid())
            return fail("no frame selected");

        lldb::SBFileSpec spec(file.filename().string().c_str());
        const lldb::SBError error = thread.StepOverUntil(frame, spec, line);

        if (error.Fail())
        {
            return fail("could not run to {}:{}: {}",
                        file.filename().string(),
                        line,
                        message_of(error, "unknown error"));
        }

        return {};
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
            return candidate.by_function && candidate.function == function;
        });

        if (existing != m_breakpoints.end())
            return existing->id;

        Breakpoint breakpoint;
        breakpoint.id = m_next_breakpoint_id++;
        breakpoint.function = function;
        breakpoint.by_function = true;

        m_breakpoints.push_back(std::move(breakpoint));
        resolve_breakpoint(m_breakpoints.back());

        Log::info("debugger: breakpoint {} at {}()", m_breakpoints.back().id, function);

        return m_breakpoints.back().id;
    }

    auto Debugger::add_address_breakpoint(uint64_t file_address) -> uint32_t
    {
        const auto existing = std::ranges::find_if(m_breakpoints, [&](const Breakpoint& candidate)
        {
            return candidate.by_address && candidate.file_address == file_address;
        });

        if (existing != m_breakpoints.end())
            return existing->id;

        Breakpoint breakpoint;
        breakpoint.id = m_next_breakpoint_id++;
        breakpoint.by_address = true;
        breakpoint.file_address = file_address;
        breakpoint.address = file_address;

        m_breakpoints.push_back(std::move(breakpoint));
        resolve_breakpoint(m_breakpoints.back());

        Log::info("debugger: breakpoint {} at 0x{:x}", m_breakpoints.back().id, file_address);

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

    auto Debugger::set_breakpoint_condition(uint32_t id, std::string_view condition) -> bool
    {
        Breakpoint* breakpoint = find_breakpoint(id);
        if (breakpoint == nullptr)
            return false;

        breakpoint->condition = condition;

        if (breakpoint->backend_id != 0 && m_session->target.IsValid())
        {
            lldb::SBBreakpoint source = m_session->target.FindBreakpointByID(breakpoint->backend_id);

            if (source.IsValid())
                source.SetCondition(breakpoint->condition.c_str());
        }

        if (breakpoint->condition.empty())
            Log::info("debugger: breakpoint {} is unconditional", id);
        else
            Log::info("debugger: breakpoint {} stops when '{}'", id, breakpoint->condition);

        return true;
    }

    auto Debugger::set_breakpoint_ignore_count(uint32_t id, uint32_t count) -> bool
    {
        Breakpoint* breakpoint = find_breakpoint(id);
        if (breakpoint == nullptr)
            return false;

        breakpoint->ignore_count = count;

        if (breakpoint->backend_id != 0 && m_session->target.IsValid())
        {
            lldb::SBBreakpoint source = m_session->target.FindBreakpointByID(breakpoint->backend_id);

            if (source.IsValid())
                source.SetIgnoreCount(count);
        }

        Log::info("debugger: breakpoint {} ignores {} hits", id, count);

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
        if (const Result<void> ready = require_stopped(); !ready)
            return std::unexpected(ready.error());

        lldb::SBFrame frame = frame_of(m_session->process, m_selected_thread, m_selected_frame);

        if (!frame.IsValid())
            return fail("no frame selected");

        const std::string text(expression);
        lldb::SBValue value = frame.EvaluateExpression(text.c_str());

        if (lldb::SBError error = value.GetError(); error.Fail())
            return fail("{}", message_of(error, "could not evaluate"));

        const std::string display = display_of(value);

        if (display.empty())
            return fail("'{}' has no value", text);

        const std::string type = text_or(value.GetDisplayTypeName(), "");

        return type.empty() ? display : std::format("({}) {}", type, display);
    }

    auto Debugger::read_memory(uint64_t address, size_t size) -> Result<std::vector<uint8_t>>
    {
        if (!is_alive(m_session->process))
            return fail("no running process");

        std::vector<uint8_t> buffer(size);

        lldb::SBError error;
        const size_t read = m_session->process.ReadMemory(address, buffer.data(), size, error);

        if (error.Fail())
        {
            return fail("could not read {} bytes at {:#x}: {}",
                        size,
                        address,
                        message_of(error, "unknown error"));
        }

        buffer.resize(read);

        return buffer;
    }

    auto Debugger::select_thread(uint64_t thread_id) -> bool
    {
        const auto entry = std::ranges::find(m_threads, thread_id, &Thread::id);
        if (entry == m_threads.end())
            return false;

        m_selected_thread = thread_id;
        m_selected_frame = 0;
        m_stop_reason = entry->stop_reason;

        if (is_alive(m_session->process))
            m_session->process.SetSelectedThreadByID(m_selected_thread);

        refresh_call_stack();
        refresh_frame_data();

        return true;
    }

    auto Debugger::select_frame(uint32_t frame_index) -> bool
    {
        if (frame_index >= m_call_stack.size())
            return false;

        m_selected_frame = frame_index;

        if (is_alive(m_session->process))
        {
            lldb::SBThread thread = m_session->process.GetThreadByID(m_selected_thread);

            if (thread.IsValid())
                thread.SetSelectedFrame(frame_index);
        }

        refresh_frame_data();

        return true;
    }

    auto Debugger::select_symbol(uint64_t file_address) -> bool
    {
        if (!m_session->target.IsValid() || file_address == 0)
            return false;

        load_disassembly(file_address);

        return !m_disassembly.empty();
    }

    auto Debugger::update() -> void
    {
        pump_events();
        sync_breakpoints();
    }

    auto Debugger::require_stopped() const -> Result<void>
    {
        if (!is_alive(m_session->process))
            return fail("no running process");

        if (m_session->process.GetState() != lldb::eStateStopped)
            return fail("the process is not stopped");

        return {};
    }

    // lldb settles the process before launch or attach returns and keeps that
    // first stop to itself, so there is no event coming for it
    auto Debugger::sync_after_start() -> void
    {
        if (!is_alive(m_session->process))
            return;

        if (m_session->process.GetState() == lldb::eStateStopped)
            on_stopped();
        else
            set_state(TargetState::Running);
    }

    auto Debugger::pump_events() -> void
    {
        if (!m_session->listener.IsValid())
            return;

        lldb::SBEvent event;

        while (m_session->listener.GetNextEvent(event))
        {
            if (!lldb::SBProcess::EventIsProcessEvent(event))
                continue;

            const uint32_t type = event.GetType();

            if ((type & (lldb::SBProcess::eBroadcastBitSTDOUT |
                         lldb::SBProcess::eBroadcastBitSTDERR)) != 0)
            {
                drain_output();
            }

            if ((type & lldb::SBProcess::eBroadcastBitStateChanged) == 0)
                continue;

            // lldb reports a stop it is about to undo, following it would show
            // the user a location the process never really sat at
            if (lldb::SBProcess::GetRestartedFromEvent(event))
                continue;

            switch (lldb::SBProcess::GetStateFromEvent(event))
            {
                // frame data belongs to a frame that no longer exists
                case lldb::eStateRunning:
                case lldb::eStateStepping:
                    m_locals.clear();
                    m_registers.clear();

                    // the instructions are still the ones on screen, only the
                    // program counter stopped meaning anything
                    for (Instruction& instruction : m_disassembly)
                        instruction.current = false;
                    set_state(TargetState::Running);
                    break;

                // a real stop is always announced as running first, so anything
                // else is lldb repeating one that was already handled
                case lldb::eStateStopped:
                    if (m_state != TargetState::Stopped)
                        on_stopped();

                    break;

                case lldb::eStateCrashed:
                    on_stopped();
                    set_state(TargetState::Crashed);
                    break;

                case lldb::eStateExited:
                    on_exited();
                    break;

                case lldb::eStateDetached:
                    drain_output();
                    m_session->process = lldb::SBProcess();
                    m_process_id = 0;
                    m_threads.clear();
                    m_call_stack.clear();
                    set_state(m_session->target.IsValid() ? TargetState::Loaded
                                                          : TargetState::NoTarget);
                    break;

                default:
                    break;
            }
        }
    }

    auto Debugger::drain_output() -> void
    {
        if (!m_session->process.IsValid())
            return;

        std::array<char, 1024> buffer{};

        while (const size_t read = m_session->process.GetSTDOUT(buffer.data(), buffer.size()))
            m_session->partial_output.append(buffer.data(), read);

        while (const size_t read = m_session->process.GetSTDERR(buffer.data(), buffer.size()))
            m_session->partial_output.append(buffer.data(), read);

        size_t start = 0;

        for (size_t end = m_session->partial_output.find('\n', start);
             end != std::string::npos;
             end = m_session->partial_output.find('\n', start))
        {
            m_console_output.emplace_back(m_session->partial_output, start, end - start);
            start = end + 1;
        }

        m_session->partial_output.erase(0, start);
    }

    auto Debugger::on_stopped() -> void
    {
        drain_output();

        m_threads.clear();

        const uint32_t count = m_session->process.GetNumThreads();

        for (uint32_t index = 0; index < count; ++index)
        {
            lldb::SBThread source = m_session->process.GetThreadAtIndex(index);

            if (!source.IsValid())
                continue;

            Thread thread;
            thread.id = source.GetThreadID();
            thread.stop_reason = to_stop_reason(source.GetStopReason());

            if (const char* name = source.GetName(); name != nullptr)
                thread.name = name;
            else if (const char* queue = source.GetQueueName(); queue != nullptr)
                thread.name = queue;

            m_threads.push_back(std::move(thread));
        }

        lldb::SBThread selected = m_session->process.GetSelectedThread();

        // lldb picks the thread that caused the stop, which is the one the user
        // wants to look at unless they already chose another that is still alive
        const bool keep_selection = std::ranges::any_of(m_threads, [&](const Thread& thread)
        {
            return thread.id == m_selected_thread && thread.stop_reason != StopReason::None;
        });

        if (!keep_selection && selected.IsValid())
            m_selected_thread = selected.GetThreadID();
        else if (keep_selection)
            m_session->process.SetSelectedThreadByID(m_selected_thread);

        m_stop_reason = StopReason::None;

        if (const auto entry = std::ranges::find(m_threads, m_selected_thread, &Thread::id);
            entry != m_threads.end())
        {
            m_stop_reason = entry->stop_reason;
        }

        m_selected_frame = 0;
        refresh_call_stack();
        refresh_frame_data();

        set_state(TargetState::Stopped);

        ++m_stop_count;
    }

    auto Debugger::on_exited() -> void
    {
        drain_output();

        if (!m_session->partial_output.empty())
        {
            m_console_output.push_back(std::move(m_session->partial_output));
            m_session->partial_output.clear();
        }

        const int status = m_session->process.GetExitStatus();
        const char* description = m_session->process.GetExitDescription();

        Log::info("debugger: pid {} exited with status {}{}",
                  m_process_id,
                  status,
                  description != nullptr ? std::format(" ({})", description) : "");

        m_console_output.push_back(std::format("[process {} exited with status {}]",
                                               m_process_id,
                                               status));

        m_session->process = lldb::SBProcess();
        m_process_id = 0;
        m_stop_reason = StopReason::None;

        m_threads.clear();
        m_call_stack.clear();
        m_locals.clear();
        m_registers.clear();

        m_selected_thread = 0;
        m_selected_frame = 0;

        // the target outlives the process, so fall back to reading the binary
        refresh_disassembly();

        set_state(TargetState::Exited);
    }

    auto Debugger::refresh_call_stack() -> void
    {
        m_call_stack.clear();

        if (!is_alive(m_session->process))
            return;

        lldb::SBThread thread = m_session->process.GetThreadByID(m_selected_thread);

        if (!thread.IsValid())
            return;

        const uint32_t count = thread.GetNumFrames();

        for (uint32_t index = 0; index < count; ++index)
        {
            lldb::SBFrame source = thread.GetFrameAtIndex(index);

            if (!source.IsValid())
                continue;

            StackFrame frame;
            frame.index = index;
            frame.program_counter = source.GetPC();
            frame.function = name_of(source);

            if (lldb::SBLineEntry entry = source.GetLineEntry(); entry.IsValid())
            {
                frame.file = path_of(entry.GetFileSpec());
                frame.line = entry.GetLine();
            }

            m_call_stack.push_back(std::move(frame));
        }
    }

    auto Debugger::refresh_frame_data() -> void
    {
        m_locals.clear();
        m_registers.clear();

        refresh_disassembly();

        if (!is_alive(m_session->process))
            return;

        lldb::SBFrame frame = frame_of(m_session->process, m_selected_thread, m_selected_frame);

        if (!frame.IsValid())
            return;

        lldb::SBValueList variables = frame.GetVariables(true, true, false, true);

        for (uint32_t index = 0; index < variables.GetSize(); ++index)
        {
            lldb::SBValue value = variables.GetValueAtIndex(index);

            if (value.IsValid())
                m_locals.push_back(to_variable(value, 0));
        }

        lldb::SBValueList sets = frame.GetRegisters();

        for (uint32_t set_index = 0; set_index < sets.GetSize(); ++set_index)
        {
            lldb::SBValue set = sets.GetValueAtIndex(set_index);
            const uint32_t count = set.GetNumChildren();

            for (uint32_t index = 0; index < count; ++index)
            {
                lldb::SBValue source = set.GetChildAtIndex(index);

                // vector registers do not fit the one word a Register holds
                if (!source.IsValid() || source.GetByteSize() > sizeof(uint64_t))
                    continue;

                Register entry;
                entry.name = text_or(source.GetName(), "?");
                entry.value = source.GetValueAsUnsigned();

                m_registers.push_back(std::move(entry));
            }
        }
    }

    auto Debugger::refresh_symbols() -> void
    {
        m_symbols.clear();

        if (!m_session->target.IsValid())
            return;

        m_symbols = module_symbols(m_session->target);
    }

    auto Debugger::refresh_source_files() -> void
    {
        m_source_files.clear();

        if (!m_session->target.IsValid())
            return;

        m_source_files = module_source_files(m_session->target);
    }

    auto Debugger::load_disassembly(uint64_t file_address) -> void
    {
        m_disassembly.clear();
        m_disassembly_name.clear();
        m_selected_symbol = 0;

        if (!m_session->target.IsValid() || file_address == 0)
            return;

        lldb::SBAddress address = m_session->target.ResolveFileAddress(file_address);

        if (!address.IsValid())
            return;

        lldb::addr_t program_counter = LLDB_INVALID_ADDRESS;

        if (is_alive(m_session->process))
        {
            lldb::SBFrame frame = frame_of(m_session->process, m_selected_thread, m_selected_frame);

            if (frame.IsValid())
                program_counter = frame.GetPC();
        }

        lldb::SBSymbolContext context = address.GetSymbolContext(lldb::eSymbolContextEverything);
        lldb::SBInstructionList instructions;
        std::string name;

        if (lldb::SBFunction function = context.GetFunction(); function.IsValid())
        {
            instructions = function.GetInstructions(m_session->target);
            name = text_or(function.GetDisplayName(), text_or(function.GetName(), "?"));
        }
        else if (lldb::SBSymbol symbol = context.GetSymbol(); symbol.IsValid())
        {
            instructions = symbol.GetInstructions(m_session->target);
            name = text_or(symbol.GetDisplayName(), text_or(symbol.GetName(), "?"));
        }
        else
        {
            instructions = m_session->target.ReadInstructions(address, DISASSEMBLY_WINDOW);
            name = std::format("{:#x}", file_address);
        }

        m_disassembly = instructions_of(m_session->target, instructions, program_counter);
        m_disassembly_name = std::move(name);
        m_selected_symbol = file_address;
    }

    auto Debugger::refresh_disassembly() -> void
    {
        if (!m_session->target.IsValid())
        {
            m_symbols.clear();
            m_source_files.clear();
            m_disassembly.clear();
            m_disassembly_name.clear();
            m_selected_symbol = 0;
            return;
        }

        if (m_symbols.empty())
            refresh_symbols();

        if (m_source_files.empty())
            refresh_source_files();

        for (Symbol& symbol : m_symbols)
        {
            lldb::SBAddress address = m_session->target.ResolveFileAddress(symbol.file_address);
            const lldb::addr_t load = address.IsValid()
                ? address.GetLoadAddress(m_session->target)
                : LLDB_INVALID_ADDRESS;

            symbol.address = load != LLDB_INVALID_ADDRESS ? load : symbol.file_address;
        }

        lldb::SBFrame frame;
        lldb::addr_t program_counter = LLDB_INVALID_ADDRESS;
        lldb::addr_t file_pc = LLDB_INVALID_ADDRESS;

        if (is_alive(m_session->process))
            frame = frame_of(m_session->process, m_selected_thread, m_selected_frame);

        if (frame.IsValid())
        {
            program_counter = frame.GetPC();
            file_pc = frame.GetPCAddress().GetFileAddress();
        }

        uint64_t wanted = m_selected_symbol;

        if (file_pc != LLDB_INVALID_ADDRESS)
        {
            const auto containing = std::ranges::find_if(m_symbols, [&](const Symbol& symbol)
            {
                if (symbol.size == 0)
                    return symbol.file_address == file_pc;

                return file_pc >= symbol.file_address &&
                       file_pc < symbol.file_address + symbol.size;
            });

            if (containing != m_symbols.end())
            {
                wanted = containing->file_address;
            }
            else if (lldb::SBFunction function = frame.GetFunction(); function.IsValid())
            {
                wanted = function.GetStartAddress().GetFileAddress();
            }
            else if (lldb::SBSymbol symbol = frame.GetSymbol(); symbol.IsValid())
            {
                wanted = symbol.GetStartAddress().GetFileAddress();
            }
        }

        if (wanted == 0)
        {
            const auto main = std::ranges::find(m_symbols, "main", &Symbol::name);
            wanted = main != m_symbols.end() ? main->file_address
                                             : (m_symbols.empty() ? 0 : m_symbols.front().file_address);
        }

        if (wanted != 0 && (wanted != m_selected_symbol || m_disassembly.empty()))
        {
            load_disassembly(wanted);
            return;
        }

        for (Instruction& instruction : m_disassembly)
        {
            lldb::SBAddress address = m_session->target.ResolveFileAddress(instruction.file_address);
            const lldb::addr_t load = address.IsValid()
                ? address.GetLoadAddress(m_session->target)
                : LLDB_INVALID_ADDRESS;

            instruction.address = load != LLDB_INVALID_ADDRESS ? load : instruction.file_address;
            instruction.current = program_counter != LLDB_INVALID_ADDRESS &&
                                  instruction.address == program_counter;
        }
    }

    auto Debugger::resolve_breakpoint(Breakpoint& breakpoint) -> void
    {
        if (!m_session->target.IsValid())
            return;

        lldb::SBBreakpoint created;

        if (breakpoint.by_address)
        {
            // a section relative address keeps pointing at the same instruction
            // when the next run loads the binary somewhere else
            lldb::SBAddress address = m_session->target.ResolveFileAddress(breakpoint.file_address);

            if (address.IsValid())
                created = m_session->target.BreakpointCreateBySBAddress(address);
        }
        else if (breakpoint.by_function)
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

        // a breakpoint can be conditioned before any target exists to carry it
        if (!breakpoint.condition.empty())
            created.SetCondition(breakpoint.condition.c_str());

        if (breakpoint.ignore_count != 0)
            created.SetIgnoreCount(breakpoint.ignore_count);

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
