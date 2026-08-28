output_dir = "%{cfg.buildcfg}-%{cfg.system}"

llvm_version = "22.1.0"
llvm_vendored_dir = "ext/llvm"

function llvm_release_asset()
    local host = os.host()
    local arm = os.hostarch() == "ARM64" or os.hostarch() == "arm64"

    if host == "macosx" then
        return "LLVM-" .. llvm_version .. "-macOS-ARM64.tar.xz"
    elseif host == "linux" then
        return "LLVM-" .. llvm_version .. (arm and "-Linux-ARM64" or "-Linux-X64") .. ".tar.xz"
    elseif host == "windows" then
        return "clang+llvm-" .. llvm_version ..
            (arm and "-aarch64-pc-windows-msvc" or "-x86_64-pc-windows-msvc") .. ".tar.xz"
    end

    return nil
end

function llvm_prefix()
    local function usable(candidate)
        if candidate == nil or candidate == "" then
            return nil
        end

        local prefix = path.getabsolute(
            (candidate:gsub("%s+$", "")):gsub("\\", "/"), _MAIN_SCRIPT_DIR)

        return os.isdir(prefix .. "/include/lldb") and prefix or nil
    end

    local function newest_of(pattern)
        local matches = os.matchdirs(pattern)
        table.sort(matches)

        for index = #matches, 1, -1 do
            local prefix = usable(matches[index])

            if prefix ~= nil then
                return prefix
            end
        end

        return nil
    end

    local prefix = usable(os.getenv("LLVM_PREFIX"))
        or usable(llvm_vendored_dir)
        or usable(os.outputof("llvm-config --prefix"))

    if prefix == nil and os.host() == "windows" then
        prefix = usable(os.getenv("ProgramFiles") and os.getenv("ProgramFiles") .. "/LLVM")
            or newest_of("C:/Program Files/LLVM*")
    end

    if prefix == nil and os.host() == "macosx" then
        prefix = usable(os.outputof("brew --prefix llvm"))
    end

    if prefix == nil and os.host() == "linux" then
        prefix = newest_of("/usr/lib/llvm-*") or newest_of("/usr/local/llvm*")
    end

    return prefix
end

llvm_dir = llvm_prefix()

if llvm_dir == nil and _ACTION ~= nil and _ACTION ~= "fetch-llvm" and _ACTION ~= "clean" then
    error("no llvm with lldb headers found. install one and set LLVM_PREFIX, " ..
        "or run `premake5 fetch-llvm` to download the pinned " .. llvm_version .. " release")
end

llvm_dir = llvm_dir or ""

include_dir = {
    glfw = "ext/glfw/include",
    glad = "ext/glad/include",
    imgui = "ext/imgui",
    imgui_backends = "ext/imgui/backends",
    lldb = llvm_dir .. "/include",
}

library_dir = {
    lldb = llvm_dir .. "/lib",
}

function lldb_library()
    if llvm_dir == "" then
        return ""
    end

    local names = { "liblldb.dylib", "liblldb.so", "liblldb.lib" }

    for _, name in ipairs(names) do
        local candidate = library_dir.lldb .. "/" .. name

        if os.isfile(candidate) then
            return candidate
        end
    end

    local versioned = os.matchfiles(library_dir.lldb .. "/liblldb.so.*")
    table.sort(versioned)

    return versioned[#versioned]
        or error("llvm at " .. llvm_dir .. " has lldb headers but no liblldb in " .. library_dir.lldb)
end

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
        os.rmdir(".vs")
        os.remove("Makefile")

        local patterns = { "*.make", "*.sln", "*.vcxproj", "*.vcxproj.*" }

        for _, pattern in ipairs(patterns) do
            for _, file in ipairs(os.matchfiles(pattern)) do
                os.remove(file)
            end
        end

        print("Cleaned build output and generated project files")
    end
}

newaction {
    trigger = "fetch-llvm",
    description = "Download the pinned llvm release into " .. llvm_vendored_dir,

    execute = function()
        if os.isdir(llvm_vendored_dir .. "/include/lldb") then
            print(llvm_vendored_dir .. " already has an llvm with lldb headers")
            return
        end

        local asset = llvm_release_asset()

        if asset == nil then
            error("no pinned llvm release for " .. os.host() .. " " .. os.hostarch() ..
                ", install llvm yourself and set LLVM_PREFIX")
        end

        local archive = "ext/" .. asset
        local staging = "ext/llvm-staging"

        if not os.isfile(archive) then
            local url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-" ..
                llvm_version .. "/" .. asset

            print("downloading " .. url)

            local reported = 0
            local result, code = http.download(url, archive, {
                progress = function(total, current)
                    if total > 0 and current - reported >= total / 20 then
                        reported = current
                        printf("  %d%% (%.0f mb)", math.floor(current / total * 100), current / 1048576)
                    end
                end
            })

            if result ~= "OK" then
                os.remove(archive)
                error("download failed with " .. tostring(code) .. ": " .. tostring(result))
            end
        end

        os.rmdir(staging)
        os.mkdir(staging)

        print("extracting " .. asset)

        local extracted = os.execute('tar -xf "' .. archive .. '" -C "' .. staging .. '"')

        if extracted ~= true and extracted ~= 0 then
            error("could not extract " .. archive .. ", unpack it into " .. llvm_vendored_dir .. " by hand")
        end

        local unpacked = os.matchdirs(staging .. "/*")[1]

        if unpacked == nil then
            error(archive .. " did not contain an llvm directory")
        end

        os.rmdir(llvm_vendored_dir)

        if not os.rename(unpacked, llvm_vendored_dir) then
            error("could not move " .. unpacked .. " to " .. llvm_vendored_dir)
        end

        os.rmdir(staging)
        os.remove(archive)

        print("llvm " .. llvm_version .. " is ready in " .. llvm_vendored_dir)
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
        "%{include_dir.lldb}",
    }

    links {
        "glfw",
        "glad",
        "imgui",
    }

    defines { "HSDBG_LLVM_PREFIX=\"" .. llvm_dir .. "\"" }

    linkoptions { lldb_library() }

    filter "system:windows"
        systemversion "latest"

        links "opengl32"

        postbuildcommands {
            '{COPYFILE} "' .. llvm_dir .. '/bin/liblldb.dll" "%{cfg.targetdir}"',
        }
    filter "system:not windows"
        linkoptions { "-Wl,-rpath," .. library_dir.lldb }
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
