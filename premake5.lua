output_dir = "%{cfg.buildcfg}-%{cfg.system}"

include_dir = {
    glfw = "ext/glfw/include",
    glad = "ext/glad/include",
    imgui = "ext/imgui",
    imgui_backends = "ext/imgui/backends",
}

function setup_target()
    targetdir ("bin/" .. output_dir)
    objdir ("bin-int/" .. output_dir .. "/%{prj.name}")
    staticruntime "On"
end

function setup_c_target()
    setup_target()

    filter "configurations:debug"
        runtime "Debug"
        symbols "On"
    filter { "configurations:release", "configurations:dist" }
        runtime "Release"
        optimize "Speed"
    filter "configurations:dist"
        symbols "Off"
    filter {}
end

function setup_cpp_target()
    setup_c_target()

    cppdialect "C++23"
end

newaction {
    trigger = "clean",
    description = "Remove build output and generated project files",

    execute = function()
        os.rmdir("bin")
        os.rmdir("bin-int")
        os.remove("Makefile")

        for _, file in ipairs(os.matchfiles("*.make")) do
            os.remove(file)
        end

        print("Cleaned build output and generated project files")
    end
}


workspace "hsdbg"
    architecture "x64"
    startproject "hsdbg"
    multiprocessorcompile "On"

    configurations {
        "debug",
        "release",
        "dist",
    }

    filter "system:windows"
        defines "HSDBG_WINDOWS"
    filter "system:linux"
        defines "HSDBG_LINUX"
    filter "system:macosx"
        architecture "ARM64"
        defines "HSDBG_MACOS"
    filter {}


group "dependencies"
project "glfw"
    kind "StaticLib"
    language "C"
    warnings "Off"

    setup_c_target()

    files {
        "ext/glfw/src/context.c",
        "ext/glfw/src/init.c",
        "ext/glfw/src/input.c",
        "ext/glfw/src/monitor.c",
        "ext/glfw/src/platform.c",
        "ext/glfw/src/vulkan.c",
        "ext/glfw/src/window.c",

        "ext/glfw/src/internal.h",
        "ext/glfw/src/platform.h",
        "ext/glfw/src/mappings.h",

        "ext/glfw/src/null_init.c",
        "ext/glfw/src/null_joystick.c",
        "ext/glfw/src/null_joystick.h",
        "ext/glfw/src/null_monitor.c",
        "ext/glfw/src/null_platform.h",
        "ext/glfw/src/null_window.c",
    }

    filter "system:linux"
        pic "On"
        defines "_GLFW_X11"

        files {
            "ext/glfw/src/x11_init.c",
            "ext/glfw/src/x11_monitor.c",
            "ext/glfw/src/x11_platform.h",
            "ext/glfw/src/x11_window.c",
            "ext/glfw/src/xkb_unicode.c",
            "ext/glfw/src/xkb_unicode.h",

            "ext/glfw/src/wl_init.c",
            "ext/glfw/src/wl_monitor.c",
            "ext/glfw/src/wl_platform.h",
            "ext/glfw/src/wl_window.c",

            "ext/glfw/src/posix_module.c",
            "ext/glfw/src/posix_time.c",
            "ext/glfw/src/posix_time.h",
            "ext/glfw/src/posix_thread.c",
            "ext/glfw/src/posix_thread.h",

            "ext/glfw/src/glx_context.c",
            "ext/glfw/src/egl_context.c",
            "ext/glfw/src/osmesa_context.c",

            "ext/glfw/src/linux_joystick.c",
            "ext/glfw/src/linux_joystick.h",
        }

    filter "system:macosx"
        pic "On"
        defines "_GLFW_COCOA"

        files {
            "ext/glfw/src/cocoa_init.m",
            "ext/glfw/src/cocoa_joystick.h",
            "ext/glfw/src/cocoa_joystick.m",
            "ext/glfw/src/cocoa_monitor.m",
            "ext/glfw/src/cocoa_platform.h",
            "ext/glfw/src/macos_time.c",
            "ext/glfw/src/macos_time.h",
            "ext/glfw/src/cocoa_window.m",

            "ext/glfw/src/nsgl_context.m",
            "ext/glfw/src/egl_context.c",
            "ext/glfw/src/osmesa_context.c",

            "ext/glfw/src/posix_module.c",
            "ext/glfw/src/posix_thread.c",
            "ext/glfw/src/posix_thread.h",
        }

    filter "system:windows"
        defines {
            "_GLFW_WIN32",
            "_CRT_SECURE_NO_WARNINGS",
        }

        files {
            "ext/glfw/src/win32_init.c",
            "ext/glfw/src/win32_joystick.c",
            "ext/glfw/src/win32_joystick.h",
            "ext/glfw/src/win32_module.c",
            "ext/glfw/src/win32_monitor.c",
            "ext/glfw/src/win32_platform.h",
            "ext/glfw/src/win32_thread.c",
            "ext/glfw/src/win32_thread.h",
            "ext/glfw/src/win32_time.c",
            "ext/glfw/src/win32_time.h",
            "ext/glfw/src/win32_window.c",
            "ext/glfw/src/wgl_context.c",
            "ext/glfw/src/egl_context.c",
            "ext/glfw/src/osmesa_context.c",
        }

    filter {}
project "glad"
    kind "StaticLib"
    language "C"
    warnings "Off"

    setup_c_target()

    files {
        "ext/glad/include/**.h",
        "ext/glad/src/**.c",
    }

    includedirs "%{include_dir.glad}"

    filter { "system:linux or system:macosx" }
        pic "On"
    filter {}
project "imgui"
    kind "StaticLib"
    language "C++"
    warnings "Off"

    setup_cpp_target()

    defines "GLFW_INCLUDE_NONE"

    files {
        "ext/imgui/imconfig.h",
        "ext/imgui/imgui.h",
        "ext/imgui/imgui.cpp",
        "ext/imgui/imgui_draw.cpp",
        "ext/imgui/imgui_internal.h",
        "ext/imgui/imgui_tables.cpp",
        "ext/imgui/imgui_widgets.cpp",
        "ext/imgui/imgui_demo.cpp",
        "ext/imgui/imstb_rectpack.h",
        "ext/imgui/imstb_textedit.h",
        "ext/imgui/imstb_truetype.h",

        "ext/imgui/misc/cpp/imgui_stdlib.h",
        "ext/imgui/misc/cpp/imgui_stdlib.cpp",

        "ext/imgui/backends/imgui_impl_glfw.h",
        "ext/imgui/backends/imgui_impl_glfw.cpp",
        "ext/imgui/backends/imgui_impl_opengl3.h",
        "ext/imgui/backends/imgui_impl_opengl3.cpp",
        "ext/imgui/backends/imgui_impl_opengl3_loader.h",
    }

    includedirs {
        "%{include_dir.imgui}",
        "%{include_dir.imgui_backends}",
        "%{include_dir.glfw}",
    }

    filter { "system:linux or system:macosx" }
        pic "On"
    filter "system:macosx"
        defines "GL_SILENCE_DEPRECATION"
    filter "system:windows"
        defines "_CRT_SECURE_NO_WARNINGS"
    filter {}
group ""

project "hsdbg"
    kind "ConsoleApp"
    language "C++"

    setup_cpp_target()

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "GLFW_INCLUDE_NONE",
    }

    files {
        "src/**.h",
        "src/**.cpp",
    }

    includedirs {
        "src",
        "%{include_dir.glfw}",
        "%{include_dir.glad}",
        "%{include_dir.imgui}",
        "%{include_dir.imgui_backends}",
    }

    links {
        "glfw",
        "glad",
        "imgui",
    }

    filter "system:windows"
        systemversion "latest"
        links "opengl32"
    filter "system:linux"
        links {
            "GL",
            "dl",
            "pthread",
        }
    filter "system:macosx"
        defines "GL_SILENCE_DEPRECATION"

        links {
            "Cocoa.framework",
            "IOKit.framework",
            "CoreFoundation.framework",
            "CoreVideo.framework",
            "OpenGL.framework",
            "QuartzCore.framework",
        }

    filter "configurations:debug"
        defines "HSDBG_DEBUG"
    filter "configurations:release"
        defines "HSDBG_RELEASE"
    filter "configurations:dist"
        defines "HSDBG_DIST"
    filter {}
