#include "ui/profiler.h"

namespace Hsdbg
{
    auto TimeSeries::push(float value) -> void
    {
        m_values[m_write] = value;
        m_write = (m_write + 1) % CAPACITY;

        if (m_count < CAPACITY)
            m_count += 1;
    }

    auto TimeSeries::clear() -> void
    {
        m_values.fill(0.0f);
        m_count = 0;
        m_write = 0;
    }

    auto TimeSeries::latest() const -> float
    {
        if (m_count == 0)
            return 0.0f;

        const int last = (m_write - 1 + CAPACITY) % CAPACITY;
        return m_values[last];
    }

    auto TimeSeries::average() const -> float
    {
        if (m_count == 0)
            return 0.0f;

        float sum = 0.0f;
        for (int i = 0; i < m_count; ++i)
            sum += m_values[i];

        return sum / static_cast<float>(m_count);
    }

    auto TimeSeries::maximum() const -> float
    {
        float peak = 0.0f;
        for (int i = 0; i < m_count; ++i)
        {
            if (m_values[i] > peak)
                peak = m_values[i];
        }

        return peak;
    }

    auto Profiler::sample_frame(float delta_seconds) -> void
    {
        if (m_paused)
            return;

        const float frame_ms = delta_seconds * 1000.0f;
        m_frame_ms.push(frame_ms);
        m_fps.push(delta_seconds > 0.0f ? 1.0f / delta_seconds : 0.0f);
    }

    auto Profiler::sample_target(uint64_t resident_bytes, bool alive) -> void
    {
        if (m_paused)
            return;

        const float megabytes = alive
            ? static_cast<float>(resident_bytes) / (1024.0f * 1024.0f)
            : 0.0f;

        m_target_memory_mb.push(megabytes);
    }

    auto Profiler::reset() -> void
    {
        m_frame_ms.clear();
        m_fps.clear();
        m_target_memory_mb.clear();
    }
}
