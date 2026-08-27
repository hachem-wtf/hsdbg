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
        bool stop_at_entry = true;
    };

    struct Breakpoint
    {
        uint32_t id = 0;
        std::filesystem::path file;
        uint32_t line = 0;
        std::string function;
        uint64_t address = 0;
        std::string condition;
        uint32_t hit_count = 0;
        bool enabled = true;
        bool resolved = false;
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
}
