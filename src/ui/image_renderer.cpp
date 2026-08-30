#include "ui/image_renderer.h"

#include "core/log.h"

#include <glad/gl.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef HSDBG_ASSET_DIR
    #define HSDBG_ASSET_DIR ""
#endif

namespace Hsdbg
{
    namespace
    {
        auto read_file(const std::filesystem::path& path) -> std::string
        {
            std::ifstream file(path);

            if (!file.is_open())
            {
                Log::error("image_renderer: could not open '{}'", path.string());
                return {};
            }

            return std::string(std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>());
        }

        auto compile(unsigned int type, const char* source) -> unsigned int
        {
            const unsigned int shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);

            int ok = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

            if (ok == 0)
            {
                char log[512];
                glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                Log::error("image_renderer: shader compile failed: {}", log);
                glDeleteShader(shader);
                return 0;
            }

            return shader;
        }
    }

    ImageRenderer::~ImageRenderer()
    {
        shutdown();
    }

    auto ImageRenderer::init() -> bool
    {
        const std::filesystem::path assets(HSDBG_ASSET_DIR);
        const std::string vertex_source = read_file(assets / "texture.vert");
        const std::string fragment_source = read_file(assets / "texture.glsl");

        if (vertex_source.empty() || fragment_source.empty())
            return false;

        const unsigned int vertex = compile(GL_VERTEX_SHADER, vertex_source.c_str());
        const unsigned int fragment = compile(GL_FRAGMENT_SHADER, fragment_source.c_str());

        if (vertex == 0 || fragment == 0)
            return false;

        m_program = glCreateProgram();
        glAttachShader(m_program, vertex);
        glAttachShader(m_program, fragment);
        glBindAttribLocation(m_program, 0, "a_position");
        glBindAttribLocation(m_program, 1, "a_uv");
        glLinkProgram(m_program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        int ok = 0;
        glGetProgramiv(m_program, GL_LINK_STATUS, &ok);

        if (ok == 0)
        {
            char log[512];
            glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
            Log::error("image_renderer: program link failed: {}", log);
            glDeleteProgram(m_program);
            m_program = 0;
            return false;
        }

        m_texture_uniform = glGetUniformLocation(m_program, "u_texture");

        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_vbo);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                              reinterpret_cast<void*>(sizeof(float) * 2));
        glBindVertexArray(0);

        return true;
    }

    auto ImageRenderer::shutdown() -> void
    {
        if (m_vbo != 0)
            glDeleteBuffers(1, &m_vbo);

        if (m_vao != 0)
            glDeleteVertexArrays(1, &m_vao);

        if (m_program != 0)
            glDeleteProgram(m_program);

        m_vbo = 0;
        m_vao = 0;
        m_program = 0;
    }

    auto ImageRenderer::draw(unsigned int texture,
                             float x0, float y0, float x1, float y1,
                             int framebuffer_width, int framebuffer_height) -> void
    {
        if (m_program == 0 || texture == 0 || framebuffer_width <= 0 || framebuffer_height <= 0)
            return;

        const auto to_ndc_x = [&](float pixel) {
            return pixel / static_cast<float>(framebuffer_width) * 2.0f - 1.0f;
        };
        const auto to_ndc_y = [&](float pixel) {
            return 1.0f - pixel / static_cast<float>(framebuffer_height) * 2.0f;
        };

        const float left = to_ndc_x(x0);
        const float right = to_ndc_x(x1);
        const float top = to_ndc_y(y0);
        const float bottom = to_ndc_y(y1);

        // two triangles; the u coordinate runs 1 -> 0 left-to-right so the image
        // is mirrored horizontally, and v (0,0) stays at the top to match how the
        // texture was uploaded (stb hands back the first row as the top)
        const float vertices[24] = {
            left,  top,    1.0f, 0.0f,
            left,  bottom, 1.0f, 1.0f,
            right, bottom, 0.0f, 1.0f,
            left,  top,    1.0f, 0.0f,
            right, bottom, 0.0f, 1.0f,
            right, top,    0.0f, 0.0f,
        };

        glViewport(0, 0, framebuffer_width, framebuffer_height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(m_texture_uniform, 0);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }
}
