#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Hsdbg
{
    enum class TargetState : uint8_t
    {
        NoTarget,
        Loaded,
        Launching,
        Running,
        Stopped,
        Exited,
        Crashed,
    };

    enum class StopReason : uint8_t
    {
        None,
        Breakpoint,
        Step,
        Signal,
        Exception,
        Watchpoint,
    };

    enum class StepMode : uint8_t
    {
        Line,
        Instruction,
    };

    constexpr auto to_string(TargetState state) -> std::string_view
    {
        switch (state)
        {
            case TargetState::NoTarget:  return "no target";
            case TargetState::Loaded:    return "loaded";
            case TargetState::Launching: return "launching";
            case TargetState::Running:   return "running";
            case TargetState::Stopped:   return "stopped";
            case TargetState::Exited:    return "exited";
            case TargetState::Crashed:   return "crashed";
        }

        return "unknown";
    }

    constexpr auto to_string(StopReason reason) -> std::string_view
    {
        switch (reason)
        {
            case StopReason::None:       return "none";
            case StopReason::Breakpoint: return "breakpoint";
            case StopReason::Step:       return "step";
            case StopReason::Signal:     return "signal";
            case StopReason::Exception:  return "exception";
            case StopReason::Watchpoint: return "watchpoint";
        }

        return "unknown";
    }

    struct LaunchSpec
    {
        std::filesystem::path executable;
        std::filesystem::path working_directory;
        std::vector<std::string> arguments;
        std::vector<std::string> environment;

        // running should reach the first breakpoint, not the loader's entry point
        bool stop_at_entry = false;
    };

    struct Breakpoint
    {
        uint32_t id = 0;
        int32_t backend_id = 0;
        std::filesystem::path file;
        uint32_t line = 0;
        std::string function;
        uint64_t address = 0;
        std::string condition;

        // how many hits lldb lets through before it starts stopping
        uint32_t ignore_count = 0;
        uint32_t hit_count = 0;
        bool enabled = true;
        bool resolved = false;

        // set by name, so file and line are whatever lldb found rather than
        // what was asked for
        bool by_function = false;

        // set on an instruction, identified by where it sits in the binary so it
        // survives the process being restarted somewhere else
        bool by_address = false;
        uint64_t file_address = 0;
    };

    struct StackFrame
    {
        uint32_t index = 0;
        uint64_t program_counter = 0;
        std::string function;
        std::filesystem::path file;
        uint32_t line = 0;
    };

    struct Thread
    {
        uint64_t id = 0;
        std::string name;
        StopReason stop_reason = StopReason::None;
    };

    struct Variable
    {
        std::string name;
        std::string type;
        std::string value;
        std::vector<Variable> children;
    };

    struct Register
    {
        std::string name;
        uint64_t value = 0;
    };

    struct Instruction
    {
        // where the instruction is right now, which is the load address once a
        // process is running and the address in the binary before that
        uint64_t address = 0;

        // the same instruction across runs, so a breakpoint can point at it
        uint64_t file_address = 0;

        std::string mnemonic;
        std::string operands;
        std::string comment;
        uint32_t size = 0;

        // whether the selected frame is sitting on this one
        bool current = false;
    };

    struct Symbol
    {
        std::string name;
        uint64_t file_address = 0;
        uint64_t address = 0;
        uint64_t size = 0;
    };

    // one recorded activation of a traced function: when it was entered and how
    // long it took. duration stays zero until the matching return is seen
    struct TraceCall
    {
        double start = 0.0;    // seconds since tracing began
        double duration = 0.0; // seconds spent inside the call
    };

    // one call laid out for the flame chart: a horizontal bar whose x is the
    // start time, width is the duration and row is how deeply it was nested
    struct TimelineSpan
    {
        uint32_t trace_id = 0; // which traced function, also picks the colour
        uint64_t thread_id = 0;
        double start = 0.0;    // seconds since tracing began
        double duration = 0.0; // seconds; zero while the call is still running
        uint32_t depth = 0;    // 0 is the outermost call on its thread
    };

    // a function the user asked to time. the debugger sets an internal breakpoint
    // on it that records each call without stopping the ui
    struct FunctionTrace
    {
        uint32_t id = 0;
        std::string function;
        int32_t entry_backend_id = 0;

        uint64_t call_count = 0;      // entries seen
        uint64_t completed_count = 0; // calls whose return was matched

        // seconds, aggregated over completed calls
        double total_time = 0.0;
        double min_time = 0.0;
        double max_time = 0.0;

        std::vector<TraceCall> calls;
    };
}
