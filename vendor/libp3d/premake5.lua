local libp3d_root = path.getdirectory(_SCRIPT)

-- libp3d (includes glad + glfw)
project "libp3d"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("%{wks.location}/obj/" .. outputdir .. "/%{prj.name}")

    files {
        libp3d_root .. "/**.h",
        libp3d_root .. "/**.hpp",
        libp3d_root .. "/**.cpp",

        -- glad
        libp3d_root .. "/vendor/glad/src/gl.c",

        -- glfw
        libp3d_root .. "/vendor/glfw/src/context.c",
        libp3d_root .. "/vendor/glfw/src/init.c",
        libp3d_root .. "/vendor/glfw/src/input.c",
        libp3d_root .. "/vendor/glfw/src/monitor.c",
        libp3d_root .. "/vendor/glfw/src/null_init.c",
        libp3d_root .. "/vendor/glfw/src/null_joystick.c",
        libp3d_root .. "/vendor/glfw/src/null_monitor.c",
        libp3d_root .. "/vendor/glfw/src/null_window.c",
        libp3d_root .. "/vendor/glfw/src/platform.c",
        libp3d_root .. "/vendor/glfw/src/vulkan.c",
        libp3d_root .. "/vendor/glfw/src/window.c",
    }

    removefiles {
        libp3d_root .. "/vendor/sdl2/**.h",
        libp3d_root .. "/vendor/sdl2/**.hpp",
        libp3d_root .. "/vendor/sdl2/**.cpp",
        libp3d_root .. "/vendor/sdl2/**.cc",
        libp3d_root .. "/vendor/sdl2/**.mm",
        libp3d_root .. "/vendor/sdl2/**.m",

        libp3d_root .. "/pddi/gles/**.h",
        libp3d_root .. "/pddi/gles/**.cpp",
        libp3d_root .. "/pddi/null/**.h",
        libp3d_root .. "/pddi/null/**.cpp",
        libp3d_root .. "/pddi/metal/**.h",
        libp3d_root .. "/pddi/metal/**.cpp",
        libp3d_root .. "/pddi/metal/**.mm",
        libp3d_root .. "/pddi/psp/**.h",
        libp3d_root .. "/pddi/psp/**.cpp",
    }

    includedirs {
        libp3d_root,
        libp3d_root .. "/vendor/glad/include",
        libp3d_root .. "/vendor/glfw/include",
        libp3d_root .. "/vendor/imgui",
        libp3d_root .. "/vendor/imgui/backends",
        libp3d_root .. "/vendor/sdl2/include",
    }

    filter "system:windows"
        systemversion "latest"
        files {
            libp3d_root .. "/vendor/glfw/src/win32_init.c",
            libp3d_root .. "/vendor/glfw/src/win32_joystick.c",
            libp3d_root .. "/vendor/glfw/src/win32_module.c",
            libp3d_root .. "/vendor/glfw/src/win32_monitor.c",
            libp3d_root .. "/vendor/glfw/src/win32_thread.c",
            libp3d_root .. "/vendor/glfw/src/win32_time.c",
            libp3d_root .. "/vendor/glfw/src/win32_window.c",
            libp3d_root .. "/vendor/glfw/src/wgl_context.c",
            libp3d_root .. "/vendor/glfw/src/egl_context.c",
            libp3d_root .. "/vendor/glfw/src/osmesa_context.c",

            libp3d_root .. "/vendor/sdl2/src/*.c",
            libp3d_root .. "/vendor/sdl2/src/atomic/*.c",
            libp3d_root .. "/vendor/sdl2/src/audio/*.c",
            libp3d_root .. "/vendor/sdl2/src/audio/directsound/*.c",
            libp3d_root .. "/vendor/sdl2/src/audio/disk/*.c",
            libp3d_root .. "/vendor/sdl2/src/audio/dummy/*.c",
            libp3d_root .. "/vendor/sdl2/src/audio/wasapi/*.c",
            libp3d_root .. "/vendor/sdl2/src/audio/winmm/*.c",
            libp3d_root .. "/vendor/sdl2/src/core/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/cpuinfo/*.c",
            libp3d_root .. "/vendor/sdl2/src/dynapi/*.c",
            libp3d_root .. "/vendor/sdl2/src/events/*.c",
            libp3d_root .. "/vendor/sdl2/src/file/*.c",
            libp3d_root .. "/vendor/sdl2/src/filesystem/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/haptic/*.c",
            libp3d_root .. "/vendor/sdl2/src/haptic/dummy/*.c",
            libp3d_root .. "/vendor/sdl2/src/haptic/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/hidapi/*.c",
            libp3d_root .. "/vendor/sdl2/src/joystick/*.c",
            libp3d_root .. "/vendor/sdl2/src/joystick/dummy/*.c",
            libp3d_root .. "/vendor/sdl2/src/joystick/hidapi/*.c",
            libp3d_root .. "/vendor/sdl2/src/joystick/virtual/*.c",
            libp3d_root .. "/vendor/sdl2/src/joystick/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/libm/*.c",
            libp3d_root .. "/vendor/sdl2/src/loadso/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/locale/*.c",
            libp3d_root .. "/vendor/sdl2/src/locale/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/misc/*.c",
            libp3d_root .. "/vendor/sdl2/src/misc/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/power/*.c",
            libp3d_root .. "/vendor/sdl2/src/power/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/direct3d/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/direct3d11/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/direct3d12/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/opengl/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/opengles2/*.c",
            libp3d_root .. "/vendor/sdl2/src/render/software/*.c",
            libp3d_root .. "/vendor/sdl2/src/sensor/*.c",
            libp3d_root .. "/vendor/sdl2/src/sensor/dummy/*.c",
            libp3d_root .. "/vendor/sdl2/src/sensor/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/stdlib/*.c",
            libp3d_root .. "/vendor/sdl2/src/thread/*.c",
            libp3d_root .. "/vendor/sdl2/src/thread/generic/SDL_syscond.c",
            libp3d_root .. "/vendor/sdl2/src/thread/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/timer/*.c",
            libp3d_root .. "/vendor/sdl2/src/timer/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/video/*.c",
            libp3d_root .. "/vendor/sdl2/src/video/dummy/*.c",
            libp3d_root .. "/vendor/sdl2/src/video/windows/*.c",
            libp3d_root .. "/vendor/sdl2/src/video/yuv2rgb/*.c",
        }
        defines {
            "_GLFW_WIN32",
            "_CRT_SECURE_NO_WARNINGS",
            "P3D_USE_VENDORED_SDL2",
            "SDL_MAIN_HANDLED",
            "SDL_STATIC",
        }

    filter "system:linux"
        files {
            libp3d_root .. "/vendor/glfw/src/egl_context.c",
            libp3d_root .. "/vendor/glfw/src/glx_context.c",
            libp3d_root .. "/vendor/glfw/src/linux_joystick.c",
            libp3d_root .. "/vendor/glfw/src/osmesa_context.c",
            libp3d_root .. "/vendor/glfw/src/posix_module.c",
            libp3d_root .. "/vendor/glfw/src/posix_poll.c",
            libp3d_root .. "/vendor/glfw/src/posix_thread.c",
            libp3d_root .. "/vendor/glfw/src/posix_time.c",
            libp3d_root .. "/vendor/glfw/src/x11_init.c",
            libp3d_root .. "/vendor/glfw/src/x11_monitor.c",
            libp3d_root .. "/vendor/glfw/src/x11_window.c",
            libp3d_root .. "/vendor/glfw/src/xkb_unicode.c",
        }
        defines {
            "_GLFW_X11",
            "P3D_USE_VENDORED_SDL2",
            "SDL_MAIN_HANDLED",
        }

    filter "system:macosx"
        architecture "arm64"
        files {
            -- GLFW Cocoa backend (see vendor/glfw/src/CMakeLists.txt).
            libp3d_root .. "/vendor/glfw/src/cocoa_time.c",
            libp3d_root .. "/vendor/glfw/src/posix_module.c",
            libp3d_root .. "/vendor/glfw/src/posix_thread.c",
            libp3d_root .. "/vendor/glfw/src/cocoa_init.m",
            libp3d_root .. "/vendor/glfw/src/cocoa_joystick.m",
            libp3d_root .. "/vendor/glfw/src/cocoa_monitor.m",
            libp3d_root .. "/vendor/glfw/src/cocoa_window.m",
            libp3d_root .. "/vendor/glfw/src/nsgl_context.m",
            libp3d_root .. "/vendor/glfw/src/egl_context.c",
            libp3d_root .. "/vendor/glfw/src/osmesa_context.c",

            -- Metal backend.
            libp3d_root .. "/pddi/metal/**.h",
            libp3d_root .. "/pddi/metal/**.cpp",
            libp3d_root .. "/pddi/metal/**.mm",

            -- ImGui Metal backend.
            libp3d_root .. "/vendor/imgui/backends/imgui_impl_metal.mm",
        }
        removefiles {
            libp3d_root .. "/pddi/gl/**.h",
            libp3d_root .. "/pddi/gl/**.cpp",
        }
        defines {
            "_GLFW_COCOA",
        }
        links {
            "Cocoa.framework",
            "IOKit.framework",
            "CoreFoundation.framework",
            "CoreVideo.framework",
            "CoreGraphics.framework",
            "QuartzCore.framework",
            "Metal.framework",
            "OpenGL.framework",
        }

    filter "configurations:Headless"
        removefiles {
            libp3d_root .. "/pddi/gl/**.h",
            libp3d_root .. "/pddi/gl/**.cpp",
            libp3d_root .. "/pddi/metal/**.h",
            libp3d_root .. "/pddi/metal/**.cpp",
            libp3d_root .. "/pddi/metal/**.mm",
        }
        files {
            libp3d_root .. "/pddi/null/**.h",
            libp3d_root .. "/pddi/null/**.cpp",
        }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release*"
        runtime "Release"
        optimize "on"

    filter "configurations:Headless"
        runtime "Release"
        optimize "on"
