#pragma once

namespace Hsdbg
{
    class ImageRenderer
    {
    public:
        ImageRenderer() = default;
        ~ImageRenderer();

        ImageRenderer(const ImageRenderer&) = delete;
        ImageRenderer(ImageRenderer&&) = delete;
        auto operator=(const ImageRenderer&) -> ImageRenderer& = delete;
        auto operator=(ImageRenderer&&) -> ImageRenderer& = delete;

        auto init() -> bool;
        auto shutdown() -> void;

        auto ready() const -> bool { return m_program != 0; }

        // draws the texture into the given rectangle, in framebuffer pixels with
        // the origin at the top-left, blended over whatever is already there
        auto draw(unsigned int texture,
                  float x0, float y0, float x1, float y1,
                  int framebuffer_width, int framebuffer_height) -> void;

    private:
        unsigned int m_program = 0;
        unsigned int m_vao = 0;
        unsigned int m_vbo = 0;
        int m_texture_uniform = -1;
    };
}
