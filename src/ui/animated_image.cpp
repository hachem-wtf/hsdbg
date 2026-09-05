#include "ui/animated_image.h"
#include "core/log.h"

#include <glad/gl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_GIF
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace Hsdbg
{
    namespace
    {
        constexpr float MIN_FRAME_SECONDS = 0.02f;

        auto bleed_edges(unsigned char* rgba, int width, int height) -> void
        {
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    unsigned char* pixel = rgba + (static_cast<std::size_t>(y) * width + x) * 4;

                    if (pixel[3] != 0)
                        continue;

                    int red = 0;
                    int green = 0;
                    int blue = 0;
                    int count = 0;

                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = x + dx;
                            const int ny = y + dy;

                            if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                                continue;

                            const unsigned char* near = rgba + (static_cast<std::size_t>(ny) * width + nx) * 4;

                            if (near[3] == 0)
                                continue;

                            red += near[0];
                            green += near[1];
                            blue += near[2];
                            ++count;
                        }
                    }

                    if (count == 0)
                        continue;

                    pixel[0] = static_cast<unsigned char>(red / count);
                    pixel[1] = static_cast<unsigned char>(green / count);
                    pixel[2] = static_cast<unsigned char>(blue / count);
                }
            }
        }

        auto upload(const unsigned char* rgba, int width, int height) -> unsigned int
        {
            unsigned int texture = 0;

            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glBindTexture(GL_TEXTURE_2D, 0);

            return texture;
        }

        auto read_file(const std::filesystem::path& path) -> std::vector<unsigned char>
        {
            std::ifstream file(path, std::ios::binary);

            if (!file.is_open())
                return {};

            return std::vector<unsigned char>(std::istreambuf_iterator<char>(file),
                                              std::istreambuf_iterator<char>());
        }
    }

    AnimatedImage::~AnimatedImage()
    {
        unload();
    }

    auto AnimatedImage::unload() -> void
    {
        if (!m_frames.empty())
            glDeleteTextures(static_cast<int>(m_frames.size()), m_frames.data());

        m_frames.clear();
        m_delays.clear();
        m_width = 0;
        m_height = 0;
        m_current = 0;
        m_accumulator = 0.0f;
    }

    auto AnimatedImage::load(const std::filesystem::path& path) -> bool
    {
        unload();

        const std::vector<unsigned char> bytes = read_file(path);

        if (bytes.empty())
        {
            Log::warn("image: could not read '{}'", path.string());
            return false;
        }

        const auto length = static_cast<int>(bytes.size());
        int pixel_width = 0;
        int pixel_height = 0;
        int comp = 0;

        int frames = 0;
        int* delays = nullptr;

        if (stbi_uc* data = stbi_load_gif_from_memory(bytes.data(), length, &delays,
                                                      &pixel_width, &pixel_height, &frames, &comp, 4))
        {
            m_width = pixel_width;
            m_height = pixel_height;

            for (int index = 0; index < frames; ++index)
            {
                stbi_uc* frame = data + static_cast<std::size_t>(index) * pixel_width * pixel_height * 4;

                bleed_edges(frame, pixel_width, pixel_height);
                m_frames.push_back(upload(frame, pixel_width, pixel_height));

                const float seconds = delays != nullptr
                                          ? static_cast<float>(delays[index]) / 1000.0f
                                          : 0.1f;

                m_delays.push_back(std::max(seconds, MIN_FRAME_SECONDS));
            }

            stbi_image_free(data);
            std::free(delays);
        }
        else if (stbi_uc* still = stbi_load_from_memory(bytes.data(), length,
                                                        &pixel_width, &pixel_height, &comp, 4))
        {
            m_width = pixel_width;
            m_height = pixel_height;
            bleed_edges(still, pixel_width, pixel_height);
            m_frames.push_back(upload(still, pixel_width, pixel_height));
            m_delays.push_back(MIN_FRAME_SECONDS);
            stbi_image_free(still);
        }
        else
        {
            Log::warn("image: could not decode '{}': {}", path.string(), stbi_failure_reason());
            return false;
        }

        Log::info("image: loaded '{}' ({}x{}, {} frame{})",
                  path.string(),
                  m_width,
                  m_height,
                  m_frames.size(),
                  m_frames.size() == 1 ? "" : "s");

        return true;
    }

    auto AnimatedImage::advance(float delta_seconds) -> void
    {
        if (m_frames.size() < 2)
            return;

        m_accumulator += delta_seconds;

        for (int step = 0; step < 1000 && m_accumulator >= m_delays[m_current]; ++step)
        {
            m_accumulator -= m_delays[m_current];
            m_current = (m_current + 1) % m_frames.size();
        }
    }

    auto AnimatedImage::texture() const -> unsigned int
    {
        return m_frames.empty() ? 0 : m_frames[m_current];
    }
}
