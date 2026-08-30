#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace Hsdbg
{
    class AnimatedImage
    {
    public:
        AnimatedImage() = default;
        ~AnimatedImage();

        AnimatedImage(const AnimatedImage&) = delete;
        AnimatedImage(AnimatedImage&&) = delete;
        auto operator=(const AnimatedImage&) -> AnimatedImage& = delete;
        auto operator=(AnimatedImage&&) -> AnimatedImage& = delete;

        auto load(const std::filesystem::path& path) -> bool;
        auto unload() -> void;

        auto advance(float delta_seconds) -> void;

        auto valid() const -> bool { return !m_frames.empty(); }

        auto texture() const -> unsigned int;
        auto width() const -> float { return static_cast<float>(m_width); }
        auto height() const -> float { return static_cast<float>(m_height); }

    private:
        std::vector<unsigned int> m_frames;
        std::vector<float> m_delays;
        int m_width = 0;
        int m_height = 0;
        std::size_t m_current = 0;
        float m_accumulator = 0.0f;
    };
}
