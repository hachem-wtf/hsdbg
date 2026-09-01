#pragma once

#include "core/result.h"
#include "debugger/types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Hsdbg
{
    class Debugger
    {
    public:
        Debugger();
        ~Debugger();

        Debugger(const Debugger&) = delete;
        Debugger(Debugger&&) = delete;
        auto operator=(const Debugger&) -> Debugger& = delete;
        auto operator=(Debugger&&) -> Debugger& = delete;

        // target lifetime
        auto load_target(const std::filesystem::path& executable) -> Result<void>;
        auto unload_target() -> void;
        auto launch(const LaunchSpec& spec) -> Result<void>;
        auto attach(uint64_t process_id) -> Result<void>;
        auto detach() -> Result<void>;
        auto terminate() -> Result<void>;

        // execution control
        auto resume() -> Result<void>;
        auto pause() -> Result<void>;
        auto step_over(StepMode mode = StepMode::Line) -> Result<void>;
        auto step_into(StepMode mode = StepMode::Line) -> Result<void>;
        auto step_out() -> Result<void>;
        auto run_to(const std::filesystem::path& file, uint32_t line) -> Result<void>;

        // breakpoints, the only part that keeps real state for now
        auto add_breakpoint(const std::filesystem::path& file, uint32_t line) -> uint32_t;
        auto add_function_breakpoint(std::string_view function) -> uint32_t;
        auto add_address_breakpoint(uint64_t file_address) -> uint32_t;
        auto remove_breakpoint(uint32_t id) -> bool;
        auto set_breakpoint_enabled(uint32_t id, bool enabled) -> bool;
        auto set_breakpoint_condition(uint32_t id, std::string_view condition) -> bool;
        auto set_breakpoint_ignore_count(uint32_t id, uint32_t count) -> bool;
        auto clear_breakpoints() -> void;
        auto find_breakpoint(uint32_t id) -> Breakpoint*;
        auto breakpoints() const -> std::span<const Breakpoint> { return m_breakpoints; }

        // function tracing: time how long each call of a named function takes by
        // recording entry and return without stopping the ui
        auto add_trace(std::string_view function) -> uint32_t;
        auto remove_trace(uint32_t id) -> bool;
        auto clear_traces() -> void;
        auto traces() const -> std::span<const FunctionTrace> { return m_traces; }

        // inspection
        auto threads() const -> std::span<const Thread> { return m_threads; }
        auto call_stack() const -> std::span<const StackFrame> { return m_call_stack; }
        auto locals() const -> std::span<const Variable> { return m_locals; }
        auto registers() const -> std::span<const Register> { return m_registers; }
        auto symbols() const -> std::span<const Symbol> { return m_symbols; }
        auto source_files() const -> std::span<const std::filesystem::path> { return m_source_files; }
        auto disassembly() const -> std::span<const Instruction> { return m_disassembly; }
        auto disassembly_name() const -> std::string_view { return m_disassembly_name; }
        auto evaluate(std::string_view expression) -> Result<std::string>;
        auto read_memory(uint64_t address, size_t size) -> Result<std::vector<uint8_t>>;
        auto console_output() const -> std::span<const std::string> { return m_console_output; }

        // resident set size of the debugged process in bytes, refreshed once a
        // frame while a target is alive and zero otherwise
        auto resident_memory() const -> uint64_t { return m_resident_memory; }

        // selection, what the ui is currently looking at
        auto select_thread(uint64_t thread_id) -> bool;
        auto select_frame(uint32_t frame_index) -> bool;
        auto select_symbol(uint64_t file_address) -> bool;
        auto selected_thread() const -> uint64_t { return m_selected_thread; }
        auto selected_frame() const -> uint32_t { return m_selected_frame; }
        auto selected_symbol() const -> uint64_t { return m_selected_symbol; }

        // pumps whatever the debug session has to say, called once per frame
        auto update() -> void;

        auto state() const -> TargetState { return m_state; }
        auto stop_reason() const -> StopReason { return m_stop_reason; }

        // bumped on every stop, so the ui can tell a new one from the one it
        // already followed
        auto stop_count() const -> uint64_t { return m_stop_count; }

        auto target_path() const -> const std::filesystem::path& { return m_target_path; }
        auto process_id() const -> uint64_t { return m_process_id; }

        auto has_target() const -> bool { return m_state != TargetState::NoTarget; }
        auto is_running() const -> bool { return m_state == TargetState::Running; }
        auto is_stopped() const -> bool { return m_state == TargetState::Stopped; }

    private:
        // keeps the lldb headers out of everything that talks to the debugger
        struct Session;

        auto set_state(TargetState state) -> void;
        auto resolve_breakpoint(Breakpoint& breakpoint) -> void;
        auto sync_breakpoints() -> void;

        auto require_stopped() const -> Result<void>;
        auto sync_after_start() -> void;
        auto pump_events() -> void;
        auto drain_output() -> void;
        auto on_stopped() -> void;
        auto on_exited() -> void;
        auto refresh_call_stack() -> void;
        auto refresh_frame_data() -> void;
        auto refresh_symbols() -> void;
        auto refresh_source_files() -> void;
        auto refresh_disassembly() -> void;
        auto load_disassembly(uint64_t file_address) -> void;
        auto sample_process_stats() -> void;

        auto resolve_trace(FunctionTrace& trace) -> void;

        // called for a stop that trace breakpoints took part in; returns true when
        // the stop was purely for tracing and the process was resumed
        auto handle_trace_stop() -> bool;
        auto trace_now() const -> double;

        std::unique_ptr<Session> m_session;

        TargetState m_state = TargetState::NoTarget;
        StopReason m_stop_reason = StopReason::None;

        std::filesystem::path m_target_path;
        uint64_t m_process_id = 0;

        std::vector<Breakpoint> m_breakpoints;
        uint32_t m_next_breakpoint_id = 1;

        std::vector<FunctionTrace> m_traces;
        uint32_t m_next_trace_id = 1;

        std::vector<Thread> m_threads;
        std::vector<StackFrame> m_call_stack;
        std::vector<Variable> m_locals;
        std::vector<Register> m_registers;
        std::vector<Symbol> m_symbols;
        std::vector<std::filesystem::path> m_source_files;
        std::vector<Instruction> m_disassembly;
        std::string m_disassembly_name;
        std::vector<std::string> m_console_output;

        uint64_t m_selected_thread = 0;
        uint32_t m_selected_frame = 0;
        uint64_t m_selected_symbol = 0;
        uint64_t m_stop_count = 0;
        uint64_t m_resident_memory = 0;

        std::chrono::steady_clock::time_point m_trace_epoch = std::chrono::steady_clock::now();
    };
}
