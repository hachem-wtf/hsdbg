#include "debugger/debugger.h"

#include "core/assert.h"
#include "core/log.h"

#include <lldb/API/LLDB.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <set>
#include <source_location>

// no standard way to read or write the environment
#include <cstdlib>

// resident memory of the debugged process, read straight from the kernel
#include <libproc.h>

namespace Hsdbg
{
    namespace
    {
#ifndef HSDBG_LLVM_PREFIX
    #define HSDBG_LLVM_PREFIX ""
#endif

        // how many individual calls a single trace keeps before it stops growing;
        // the counters keep climbing, only the per-call history is bounded
        constexpr size_t MAX_TRACE_CALLS = 200000;

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

            // on windows the list is empty by design and this returns before the
            // warning below; cppcheck only sees the platform it runs on, where the
            // list is never empty
            // cppcheck-suppress knownConditionTrueFalse
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

        // runs a command and hands back its stdout, trimmed; empty if it could
        // not be spawned (used to ask the toolchain where it lives)
        auto capture_command(const char* command) -> std::string
        {
#ifdef HSDBG_WINDOWS
            FILE* pipe = _popen(command, "r");
#else
            FILE* pipe = popen(command, "r");
#endif
            if (pipe == nullptr)
                return {};

            std::string output;
            std::array<char, 256> chunk{};

            while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr)
                output += chunk.data();

#ifdef HSDBG_WINDOWS
            _pclose(pipe);
#else
            pclose(pipe);
#endif

            while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back())) != 0)
                output.pop_back();

            return output;
        }

        // rust ships lldb data formatters (the same ones rust-lldb sources) that
        // teach lldb how to print String, Vec, Option, enums and friends. without
        // them those show as raw structs, so pull them in if a toolchain is around.
        // harmless for c/c++ targets, and a no-op if lldb has no python scripting
        auto load_rust_formatters(lldb::SBDebugger& debugger) -> void
        {
            const std::string sysroot = capture_command("rustc --print sysroot 2>/dev/null");

            if (sysroot.empty())
                return;

            const std::filesystem::path etc =
                std::filesystem::path(sysroot) / "lib" / "rustlib" / "etc";

            std::error_code error;
            if (!std::filesystem::exists(etc / "lldb_commands", error))
                return;

            lldb::SBCommandInterpreter interpreter = debugger.GetCommandInterpreter();
            lldb::SBCommandReturnObject result;

            const std::string import =
                std::format("command script import \"{}\"", (etc / "lldb_lookup.py").string());
            interpreter.HandleCommand(import.c_str(), result);

            if (!result.Succeeded())
            {
                Log::warn("debugger: rust formatters unavailable ({})",
                          text_or(result.GetError(), "lldb has no python scripting"));
                return;
            }

            const std::string source =
                std::format("command source \"{}\"", (etc / "lldb_commands").string());
            interpreter.HandleCommand(source.c_str(), result);

            if (result.Succeeded())
                Log::info("debugger: loaded rust type formatters from {}", etc.string());
            else
                Log::warn("debugger: could not load rust formatters ({})",
                          text_or(result.GetError(), "unknown error"));
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

        auto read_back(const lldb::SBTarget& target, lldb::SBBreakpoint& source, Breakpoint& breakpoint) -> void
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

        auto instruction_of(const lldb::SBTarget& target,
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

        auto instructions_of(const lldb::SBTarget& target,
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

        load_rust_formatters(m_session->debugger);
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

        std::ranges::transform(spec.arguments, std::back_inserter(arguments),
                               [](const std::string& argument) { return argument.c_str(); });

        arguments.push_back(nullptr);

        lldb::SBLaunchInfo info(arguments.data());
        info.SetListener(m_session->listener);

        if (!spec.environment.empty())
        {
            std::vector<const char*> environment;
            environment.reserve(spec.environment.size() + 1);

            std::ranges::transform(spec.environment, std::back_inserter(environment),
                                   [](const std::string& entry) { return entry.c_str(); });

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

        // each run is timed from its own start, so drop whatever the last run left,
        // including return breakpoints whose addresses die with the old process
        m_trace_epoch = std::chrono::steady_clock::now();
        clear_return_breakpoints();
        m_timeline.clear();

        // instrumentation lives in the process, so a fresh run re-resolves it and
        // starts its record count and per-thread stacks over
        m_instr_checked = false;
        m_instr_available = false;
        m_instr_read_count = 0;
        m_instr_base_set = false;
        m_instr_functions.clear();
        m_instr_names.clear();
        m_instr_stacks.clear();

        m_sample_pending = false;
        m_sample_functions.clear();
        m_sample_stacks.clear();

        for (FunctionTrace& trace : m_traces)
        {
            trace.call_count = 0;
            trace.completed_count = 0;
            trace.total_time = 0.0;
            trace.min_time = 0.0;
            trace.max_time = 0.0;
            trace.calls.clear();
        }

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

    auto Debugger::attach(uint64_t pid) -> Result<void>
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

        lldb::SBAttachInfo info(static_cast<lldb::pid_t>(pid));
        info.SetListener(m_session->listener);

        set_state(TargetState::Launching);

        lldb::SBError error;
        lldb::SBProcess process = m_session->target.Attach(info, error);

        if (error.Fail() || !process.IsValid())
        {
            set_state(m_target_path.empty() ? TargetState::NoTarget : TargetState::Loaded);

            return fail("could not attach to pid {}: {}",
                        pid,
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

        // the injected code is compiled by lldb's clang and jitted into the
        // inferior, so any side effect it has (a = 60, *p = ...) writes real
        // process memory and survives the resume, exactly as if the source had
        // run it. the options here keep that from derailing the session: a fault
        // in the snippet is unwound instead of left mid-flight, breakpoints the
        // snippet reaches are ignored so it cannot stop inside itself, and a
        // timeout stops a runaway call from hanging the ui
        lldb::SBExpressionOptions options;
        options.SetUnwindOnError(true);
        options.SetIgnoreBreakpoints(true);
        options.SetTryAllThreads(true);
        options.SetAutoApplyFixIts(true);
        options.SetTimeoutInMicroSeconds(5'000'000);

        const std::string text(expression);
        lldb::SBValue value = frame.EvaluateExpression(text.c_str(), options);

        if (lldb::SBError error = value.GetError(); error.Fail())
            return fail("{}", message_of(error, "could not evaluate"));

        // the snippet may have printed, and it may have moved memory the panels
        // are showing, so surface both before returning to the caller
        drain_output();
        refresh_frame_data();

        const std::string display = display_of(value);

        // a pure statement (a plain assignment counts) yields no value to show,
        // but it still ran and its side effects landed, so report success
        if (display.empty())
            return std::format("{} applied", text);

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
        sample_process_stats();
        maybe_request_sample();
    }

    auto Debugger::sample_process_stats() -> void
    {
        if (!is_alive(m_session->process))
        {
            m_resident_memory = 0;
            return;
        }

        rusage_info_v2 usage{};
        if (proc_pid_rusage(static_cast<int>(m_process_id), RUSAGE_INFO_V2,
                            reinterpret_cast<rusage_info_t*>(&usage)) == 0)
        {
            m_resident_memory = usage.ri_resident_size;
        }
    }

    auto Debugger::trace_now() const -> double
    {
        const auto elapsed = std::chrono::steady_clock::now() - m_trace_epoch;
        return std::chrono::duration<double>(elapsed).count();
    }

    auto Debugger::add_trace(std::string_view function) -> uint32_t
    {
        const auto existing = std::ranges::find_if(m_traces, [&](const FunctionTrace& candidate)
        {
            return candidate.function == function;
        });

        if (existing != m_traces.end())
            return existing->id;

        FunctionTrace trace;
        trace.id = m_next_trace_id++;
        trace.function = function;

        m_traces.push_back(std::move(trace));
        resolve_trace(m_traces.back());

        Log::info("debugger: tracing {}()", m_traces.back().function);

        return m_traces.back().id;
    }

    auto Debugger::resolve_trace(FunctionTrace& trace) -> void
    {
        if (!m_session->target.IsValid())
            return;

        // scoped to the executable so a bare name does not also catch the same
        // symbol pulled in from a shared library
        lldb::SBBreakpoint created = m_session->target.BreakpointCreateByName(
            trace.function.c_str(), m_session->target.GetExecutable().GetFilename());

        if (!created.IsValid() || created.GetNumLocations() == 0)
        {
            Log::warn("debugger: could not trace {}()", trace.function);
            return;
        }

        created.SetEnabled(true);
        trace.entry_backend_id = created.GetID();

        Log::debug("debugger: trace on {}() ({} locations)",
                   trace.function, created.GetNumLocations());
    }

    auto Debugger::remove_trace(uint32_t id) -> bool
    {
        const auto entry = std::ranges::find(m_traces, id, &FunctionTrace::id);
        if (entry == m_traces.end())
            return false;

        if (entry->entry_backend_id != 0 && m_session->target.IsValid())
            m_session->target.BreakpointDelete(entry->entry_backend_id);

        m_traces.erase(entry);
        return true;
    }

    auto Debugger::clear_traces() -> void
    {
        if (m_session->target.IsValid())
        {
            for (const FunctionTrace& trace : m_traces)
            {
                if (trace.entry_backend_id != 0)
                    m_session->target.BreakpointDelete(trace.entry_backend_id);
            }
        }

        m_traces.clear();
    }

    auto Debugger::ensure_return_breakpoint(uint64_t address) -> int32_t
    {
        if (const auto entry = m_return_breakpoints.find(address); entry != m_return_breakpoints.end())
            return entry->second;

        lldb::SBBreakpoint created = m_session->target.BreakpointCreateByAddress(address);
        if (!created.IsValid())
            return 0;

        created.SetEnabled(true);

        const int32_t id = created.GetID();
        m_return_breakpoints.emplace(address, id);
        return id;
    }

    auto Debugger::clear_return_breakpoints() -> void
    {
        if (m_session->target.IsValid())
        {
            for (const auto& [address, id] : m_return_breakpoints)
                m_session->target.BreakpointDelete(id);
        }

        m_return_breakpoints.clear();
        m_pending_calls.clear();
    }

    auto Debugger::handle_trace_stop() -> bool
    {
        if (m_traces.empty())
            return false;

        bool any_trace = false;
        bool any_other = false;

        const uint32_t thread_count = m_session->process.GetNumThreads();

        for (uint32_t index = 0; index < thread_count; ++index)
        {
            lldb::SBThread thread = m_session->process.GetThreadAtIndex(index);
            if (!thread.IsValid())
                continue;

            const lldb::StopReason reason = thread.GetStopReason();

            if (reason != lldb::eStopReasonBreakpoint)
            {
                // a signal, exception or completed step: a genuine stop
                if (reason != lldb::eStopReasonNone && reason != lldb::eStopReasonInvalid)
                    any_other = true;

                continue;
            }

            // the stop reason carries (breakpoint id, location id) pairs
            const size_t pairs = thread.GetStopReasonDataCount() / 2;

            for (size_t pair = 0; pair < pairs; ++pair)
            {
                const auto backend_id =
                    static_cast<int32_t>(thread.GetStopReasonDataAtIndex(pair * 2));

                const auto trace = std::ranges::find(m_traces, backend_id,
                                                     &FunctionTrace::entry_backend_id);

                const bool is_return = std::ranges::any_of(m_return_breakpoints,
                                                           [&](const auto& kv) { return kv.second == backend_id; });

                if (trace != m_traces.end() && backend_id != 0)
                {
                    record_trace_entry(*trace, thread);
                    any_trace = true;
                }
                else if (is_return)
                {
                    record_trace_return(thread);
                    any_trace = true;
                }
                else
                {
                    // a real breakpoint the user set
                    any_other = true;
                }
            }
        }

        // only slip the process back into motion when nothing but tracing happened
        if (any_trace && !any_other)
        {
            m_session->process.Continue();
            return true;
        }

        return false;
    }

    auto Debugger::record_trace_entry(FunctionTrace& trace, lldb::SBThread& thread) -> void
    {
        const double start = trace_now();

        trace.call_count += 1;

        size_t call_index = MAX_TRACE_CALLS;
        if (trace.calls.size() < MAX_TRACE_CALLS)
        {
            call_index = trace.calls.size();
            trace.calls.push_back(TraceCall{ start, 0.0 });
        }

        // the breakpoint sits past the prologue, so frame 1 is a settled caller
        // whose pc is the address this call will return to
        lldb::SBFrame caller = thread.GetFrameAtIndex(1);
        if (!caller.IsValid())
            return;

        const uint64_t thread_id = thread.GetThreadID();

        // nesting is how many traced calls are already open on this thread; the
        // outermost one sits on row zero and children stack above it
        const auto depth = static_cast<uint32_t>(std::ranges::count(
            m_pending_calls, thread_id, &PendingCall::thread_id));

        size_t span_index = MAX_TRACE_CALLS;
        if (m_timeline.size() < MAX_TRACE_CALLS)
        {
            span_index = m_timeline.size();
            m_timeline.push_back(TimelineSpan{ trace.id, thread_id, start, 0.0, depth });
        }

        PendingCall pending;
        pending.trace_id = trace.id;
        pending.thread_id = thread_id;
        pending.return_pc = caller.GetPC();
        pending.frame_sp = caller.GetSP();
        pending.start = start;
        pending.call_index = call_index;
        pending.span_index = span_index;

        m_pending_calls.push_back(pending);
        ensure_return_breakpoint(pending.return_pc);
    }

    auto Debugger::record_trace_return(lldb::SBThread& thread) -> void
    {
        lldb::SBFrame frame = thread.GetFrameAtIndex(0);
        if (!frame.IsValid())
            return;

        const uint64_t pc = frame.GetPC();
        const uint64_t sp = frame.GetSP();
        const uint64_t thread_id = thread.GetThreadID();
        const double now = trace_now();

        // walk newest first so a recursive call matches its own activation
        for (auto entry = m_pending_calls.rbegin(); entry != m_pending_calls.rend(); ++entry)
        {
            if (entry->thread_id != thread_id || entry->return_pc != pc || entry->frame_sp != sp)
                continue;

            const double duration = now - entry->start;

            if (const auto trace = std::ranges::find(m_traces, entry->trace_id, &FunctionTrace::id);
                trace != m_traces.end())
            {
                if (entry->call_index < trace->calls.size())
                    trace->calls[entry->call_index].duration = duration;

                if (entry->span_index < m_timeline.size())
                    m_timeline[entry->span_index].duration = duration;

                trace->completed_count += 1;
                trace->total_time += duration;
                trace->max_time = std::max(trace->max_time, duration);
                trace->min_time = trace->completed_count == 1 ? duration
                                                              : std::min(trace->min_time, duration);
            }

            m_pending_calls.erase(std::next(entry).base());
            break;
        }
    }

    // mirrors HsdbgTraceRecord in ext/hsdbg_trace/hsdbg_trace.h; that header is the
    // source of truth for this 32-byte layout
    struct Debugger::InstrRecord
    {
        uint64_t timestamp_ns;
        uint64_t function;
        uint64_t thread_id;
        uint32_t kind;
        uint32_t reserved;
    };

    auto Debugger::symbol_load_address(const char* name) -> uint64_t
    {
        lldb::SBSymbolContextList list = m_session->target.FindSymbols(name);

        for (uint32_t index = 0; index < list.GetSize(); ++index)
        {
            lldb::SBSymbol symbol = list.GetContextAtIndex(index).GetSymbol();
            if (!symbol.IsValid())
                continue;

            const uint64_t load = symbol.GetStartAddress().GetLoadAddress(m_session->target);
            if (load != LLDB_INVALID_ADDRESS && load != 0)
                return load;
        }

        return 0;
    }

    auto Debugger::resolve_instrumentation() -> void
    {
        const uint64_t head = symbol_load_address("hsdbg_trace_head");
        const uint64_t records = symbol_load_address("hsdbg_trace_records");
        const uint64_t capacity_addr = symbol_load_address("hsdbg_trace_capacity");

        if (head == 0 || records == 0 || capacity_addr == 0)
        {
            // the target simply was not built with the trace runtime
            m_instr_checked = true;
            m_instr_available = false;
            return;
        }

        lldb::SBError error;
        uint64_t capacity = 0;
        m_session->process.ReadMemory(capacity_addr, &capacity, sizeof(capacity), error);

        if (!error.Success() || capacity == 0)
            return; // memory not readable yet, try again on the next stop

        m_instr_head_addr = head;
        m_instr_records_addr = records;
        m_instr_capacity = capacity;
        m_instr_available = true;
        m_instr_checked = true;

        Log::info("debugger: function instrumentation active ({} record buffer)", capacity);
    }

    auto Debugger::read_instrumentation() -> void
    {
        if (!is_alive(m_session->process))
            return;

        if (!m_instr_checked)
            resolve_instrumentation();

        if (!m_instr_available)
            return;

        lldb::SBError error;
        uint64_t head = 0;
        m_session->process.ReadMemory(m_instr_head_addr, &head, sizeof(head), error);

        if (!error.Success() || head <= m_instr_read_count)
            return;

        const uint64_t capacity = m_instr_capacity;

        // if we fell further behind than the ring holds, only the newest survive
        uint64_t start = m_instr_read_count;
        if (head - start > capacity)
            start = head - capacity;

        const uint64_t total = head - start;
        std::vector<InstrRecord> buffer(total);

        // the live records wrap around the ring, so read up to two flat chunks
        uint64_t read = 0;
        while (read < total)
        {
            const uint64_t position = (start + read) % capacity;
            const uint64_t chunk = std::min(total - read, capacity - position);
            const uint64_t address = m_instr_records_addr + position * sizeof(InstrRecord);

            lldb::SBError chunk_error;
            const size_t got = m_session->process.ReadMemory(
                address, buffer.data() + read, chunk * sizeof(InstrRecord), chunk_error);

            if (!chunk_error.Success() || got != chunk * sizeof(InstrRecord))
                return; // leave m_instr_read_count untouched and retry next stop

            read += chunk;
        }

        for (const InstrRecord& record : buffer)
            apply_instr_record(record);

        m_instr_read_count = head;
    }

    auto Debugger::apply_instr_record(const InstrRecord& record) -> void
    {
        if (!m_instr_base_set)
        {
            m_instr_base_ns = record.timestamp_ns;
            m_instr_base_set = true;
        }

        const double start = static_cast<double>(record.timestamp_ns - m_instr_base_ns) / 1.0e9;
        const uint32_t id = intern_instr_function(record.function);

        std::vector<InstrOpenCall>& stack = m_instr_stacks[record.thread_id];

        if (record.kind == 0 /* enter */)
        {
            const auto depth = static_cast<uint32_t>(stack.size());

            size_t span_index = m_timeline.size();
            if (m_timeline.size() < MAX_TRACE_CALLS)
                m_timeline.push_back(TimelineSpan{ id, record.thread_id, start, 0.0, depth });

            stack.push_back(InstrOpenCall{ id, record.timestamp_ns, span_index });
        }
        else if (!stack.empty())
        {
            const InstrOpenCall open = stack.back();
            stack.pop_back();

            const double duration =
                static_cast<double>(record.timestamp_ns - open.start_ns) / 1.0e9;

            if (open.span_index < m_timeline.size())
                m_timeline[open.span_index].duration = duration;
        }
    }

    auto Debugger::intern_instr_function(uint64_t address) -> uint32_t
    {
        if (const auto entry = m_instr_functions.find(address); entry != m_instr_functions.end())
            return entry->second;

        std::string name;
        lldb::SBAddress resolved = m_session->target.ResolveLoadAddress(address);

        if (resolved.IsValid())
        {
            if (lldb::SBFunction function = resolved.GetFunction();
                function.IsValid() && function.GetName() != nullptr)
            {
                name = function.GetName();
            }
            else if (lldb::SBSymbol symbol = resolved.GetSymbol();
                     symbol.IsValid() && symbol.GetName() != nullptr)
            {
                name = symbol.GetName();
            }
        }

        if (name.empty())
            name = std::format("{:#x}", address);

        const uint32_t id = m_next_trace_id++;
        m_instr_functions.emplace(address, id);
        m_instr_names.emplace(id, std::move(name));
        return id;
    }

    auto Debugger::span_label(uint32_t trace_id) const -> const char*
    {
        if (const auto entry = m_instr_names.find(trace_id); entry != m_instr_names.end())
            return entry->second.c_str();

        const auto trace = std::ranges::find(m_traces, trace_id, &FunctionTrace::id);
        if (trace != m_traces.end())
            return trace->function.c_str();

        return "?";
    }

    auto Debugger::intern_named_function(std::string_view name) -> uint32_t
    {
        std::string key(name);

        if (const auto entry = m_sample_functions.find(key); entry != m_sample_functions.end())
            return entry->second;

        const uint32_t id = m_next_trace_id++;
        m_sample_functions.emplace(key, id);
        m_instr_names.emplace(id, std::move(key)); // shared id -> name store used by span_label
        return id;
    }

    auto Debugger::maybe_request_sample() -> void
    {
        // instrumentation, when present, is the exact source and wins
        if (!m_sampling_enabled || m_instr_available)
            return;

        if (!is_alive(m_session->process) || m_session->process.GetState() != lldb::eStateRunning)
            return;

        if (m_sample_pending)
            return;

        // update() runs once a frame, so this caps out near the frame rate; a
        // short interval just means "as often as we can"
        const auto now = std::chrono::steady_clock::now();
        if (now - m_sample_last < std::chrono::milliseconds(5))
            return;

        m_sample_last = now;
        m_session->process.Stop();
        m_sample_pending = true;
    }

    auto Debugger::take_sample_and_resume() -> bool
    {
        // a real breakpoint landing at the same time is a genuine stop, not a sample
        const uint32_t thread_count = m_session->process.GetNumThreads();
        for (uint32_t index = 0; index < thread_count; ++index)
        {
            lldb::SBThread thread = m_session->process.GetThreadAtIndex(index);
            if (thread.IsValid() && thread.GetStopReason() == lldb::eStopReasonBreakpoint)
                return false;
        }

        take_sample();
        m_session->process.Continue();
        return true;
    }

    auto Debugger::take_sample() -> void
    {
        const double now = trace_now();
        const uint32_t thread_count = m_session->process.GetNumThreads();

        std::vector<uint32_t> stack;

        for (uint32_t index = 0; index < thread_count; ++index)
        {
            lldb::SBThread thread = m_session->process.GetThreadAtIndex(index);
            if (!thread.IsValid())
                continue;

            stack.clear();

            // lldb numbers frames innermost-first, so walk from the top down to put
            // the outermost call (main) at depth zero
            const uint32_t frames = std::min(thread.GetNumFrames(), 64u);
            for (int frame_index = static_cast<int>(frames) - 1; frame_index >= 0; --frame_index)
            {
                lldb::SBFrame frame = thread.GetFrameAtIndex(static_cast<uint32_t>(frame_index));
                if (frame.IsValid())
                    stack.push_back(intern_named_function(name_of(frame)));
            }

            fold_sample(thread.GetThreadID(), stack, now);
        }
    }

    auto Debugger::fold_sample(uint64_t thread_id, const std::vector<uint32_t>& stack, double now) -> void
    {
        std::vector<OpenSample>& open = m_sample_stacks[thread_id];

        // how deep the new stack still matches the bars already open
        size_t match = 0;
        while (match < open.size() && match < stack.size() && open[match].trace_id == stack[match])
            ++match;

        // close every bar below the divergence point, deepest first
        for (size_t depth = open.size(); depth-- > match;)
        {
            const size_t span = open[depth].span_index;
            if (span < m_timeline.size())
                m_timeline[span].duration = now - m_timeline[span].start;
        }
        open.resize(match);

        // open a fresh bar for each newly seen frame
        for (size_t depth = match; depth < stack.size(); ++depth)
        {
            size_t span_index = m_timeline.size();
            if (m_timeline.size() < MAX_TRACE_CALLS)
            {
                m_timeline.push_back(TimelineSpan{ stack[depth], thread_id, now, 0.0,
                                                   static_cast<uint32_t>(depth) });
            }

            open.push_back(OpenSample{ stack[depth], span_index });
        }

        // let still-open bars grow up to the current sample so they render live
        for (const OpenSample& entry : open)
        {
            if (entry.span_index < m_timeline.size())
                m_timeline[entry.span_index].duration = now - m_timeline[entry.span_index].start;
        }
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
                    {
                        // our own Stop() for a sample resolves here; clear the flag
                        // up front so a coincident breakpoint cannot strand it
                        const bool was_sampling = m_sample_pending;
                        m_sample_pending = false;

                        // a trace-only stop records the call and resumes without
                        // ever surfacing to the user as a stop
                        if (handle_trace_stop())
                            break;

                        // likewise a sampling stop grabs the stacks and resumes
                        if (was_sampling && take_sample_and_resume())
                            break;

                        on_stopped();
                    }

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

        // the process is settled, so drain whatever the instrumentation buffer has
        // gathered since the last stop into the timeline
        read_instrumentation();

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

    auto Debugger::set_state(TargetState next_state) -> void
    {
        if (m_state == next_state)
            return;

        Log::debug("debugger: {} -> {}", to_string(m_state), to_string(next_state));

        m_state = next_state;
    }
}
