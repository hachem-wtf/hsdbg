#pragma once

#include <array>
#include <cstdint>

namespace Hsdbg
{
    // a small fixed-size ring buffer of floats, sized for a live scrolling
    // graph; new samples overwrite the oldest once it fills up
    class TimeSeries
    {
    public:
        // roughly four seconds of history at 60 fps
        static constexpr int CAPACITY = 240;

        auto push(float value) -> void;
        auto clear() -> void;

        auto values() const -> const float* { return m_values.data(); }
        auto count() const -> int { return m_count; }

        // where the oldest sample sits, so ImGui::PlotLines can unwrap the ring
        auto offset() const -> int { return m_count == CAPACITY ? m_write : 0; }

        auto latest() const -> float;
        auto average() const -> float;
        auto maximum() const -> float;

    private:
        std::array<float, CAPACITY> m_values{};
        int m_count = 0;
        int m_write = 0;
    };

    // collects live performance samples for the profiler panel. this first pass
    // only tracks the debugger's own frame timing; sampling the debugged target
    // comes in a later step
    class Profiler
    {
    public:
        auto sample_frame(float delta_seconds) -> void;

        // resident set size of the target in bytes; alive says whether a process
        // is actually running so the graph can hold flat while nothing is loaded
        auto sample_target(uint64_t resident_bytes, bool alive) -> void;
        auto reset() -> void;

        auto frame_times() const -> const TimeSeries& { return m_frame_ms; }
        auto frame_rates() const -> const TimeSeries& { return m_fps; }
        auto target_memory_mb() const -> const TimeSeries& { return m_target_memory_mb; }

        auto paused() const -> bool { return m_paused; }
        auto set_paused(bool paused) -> void { m_paused = paused; }

    private:
        TimeSeries m_frame_ms;
        TimeSeries m_fps;
        TimeSeries m_target_memory_mb;
        bool m_paused = false;
    };
}
