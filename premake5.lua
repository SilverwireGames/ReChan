workspace "rechan"
    architecture "x86_64"
    configurations { "Debug", "Release", "ReleaseASan", "Shipping", "Headless" }
    location "build"
    startproject "rechan"

    filter "system:windows"
        toolset "v145"
        -- Parallel translation units race on the shared compiler PDB (C1041).
        buildoptions { "/FS" }
    filter {}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

include "vendor/libp3d"

-- rechan
project "rechan"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    targetdir "bin"
    objdir    ("%{wks.location}/obj/" .. outputdir .. "/%{prj.name}")
    debugdir  "bin"
    -- Premake builds without an MSBuild generator have no
    -- multiprocessorcompile field; guard it (Linux/Steam Deck).
    if multiprocessorcompile then
        multiprocessorcompile "on"
    end

    files {
        "src/**.h",
        "src/**.cpp",
        "vendor/cgltf/cgltf.h",
    }

    -- PSP has a separate PSPSDK Makefile and native replacements for the
    -- desktop implementations. Keep those translation units out of every
    -- desktop configuration.
    removefiles {
        "src/psp/**.h",
        "src/psp/**.cpp",
    }

    filter "system:windows"
        files { "src/pc/rechan.rc" }
    filter {}

    includedirs {
        "src",
        "vendor/libp3d",
        "vendor/libp3d/vendor/imgui",
        "vendor/miniaudio",
        "vendor/cgltf",
    }

    links {
        "libp3d",
    }
	defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"
        defines { "RC_PLATFORM_WINDOWS" }
        links {
            "opengl32",
            "dbghelp",
            "cfgmgr32",
            "imm32",
            "setupapi",
            "version",
            "winmm",
        }

    filter "system:linux"
        defines { "RC_PLATFORM_LINUX", "PLATFORM_LINUX" }
        linkoptions { "-rdynamic" }
        links {
            "GL", "X11", "Xcursor", "Xi", "Xinerama", "Xrandr",
            "SDL2", "pthread", "dl", "m",
        }

    filter "system:macosx"
        architecture "arm64"
        defines { "RC_PLATFORM_MACOS" }
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
        defines { "NDEBUG", "RC_PLATFORM_NULL" }
        runtime "Release"
        optimize "on"
        symbols "on"
        targetname "rechan-headless"

    filter { "system:linux", "configurations:Headless" }
        removelinks { "GL", "X11", "Xcursor", "Xi", "Xinerama", "Xrandr", "SDL2" }

    filter { "system:windows", "configurations:Headless" }
        removelinks { "opengl32" }

    filter { "system:macosx", "configurations:Headless" }
        removelinks {
            "Cocoa.framework", "IOKit.framework", "CoreFoundation.framework",
            "CoreVideo.framework", "CoreGraphics.framework", "QuartzCore.framework",
            "Metal.framework", "OpenGL.framework",
        }

    -- Every config shares targetdir "bin" so the game finds its assets, so each
    -- needs its own targetname or they silently overwrite each other's exe.
    -- Release and Shipping deliberately both stay "rechan": pack-win64.ps1
    -- packages bin/rechan.exe by name.
    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
        defines { "DEBUG" }
        targetname "rechan-debug"

    filter "configurations:Release*"
        defines { "NDEBUG" }
        runtime "Release"
        optimize "on"

    filter { "system:windows", "configurations:Release" }
        kind "WindowedApp"
        entrypoint "mainCRTStartup"

    -- ReleaseASan: optimized (keeps the layout-sensitive bug live) + AddressSanitizer.
    -- Kept as ConsoleApp so ASan diagnostics print to a console window.
    filter "configurations:ReleaseASan"
        symbols "on"
        targetname "rechan-asan"

    filter { "system:windows", "configurations:ReleaseASan" }
        editandcontinue "off"
        buildoptions { "/fsanitize=address" }
        -- libp3d is linked without ASan; disable STL container annotations so the
        -- instrumented rechan objects match (avoids LNK2038 annotate_* mismatches).
        defines { "_DISABLE_STL_ANNOTATION" }

    filter { "system:linux", "configurations:ReleaseASan" }
        buildoptions { "-fsanitize=address" }
        linkoptions { "-fsanitize=address" }

    filter "configurations:Shipping"
        defines { "NDEBUG" }
        runtime "Release"
        optimize "on"

    filter { "system:windows", "configurations:Shipping" }
        kind "WindowedApp"
        entrypoint "mainCRTStartup"
        postbuildcommands {
            'powershell -ExecutionPolicy Bypass -File "$(ProjectDir)../scripts/pack-win64.ps1" -Root "$(ProjectDir).."'
        }

    filter { "system:linux", "configurations:Shipping" }
        postbuildcommands {
            'bash ../scripts/pack-linux64.sh ..'
        }

    filter {}
