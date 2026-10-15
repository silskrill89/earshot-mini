-- Earshot_reminid build script
-- Adapted from the skygfx_plus_expIV premake5.lua pattern (SA ASI plugin):
--   * plugin-sdk include lines mirrored EXACTLY from
--     E:/dev(dave)/skygfx_plus_expIV/premake5.lua lines 41-42
--   * x86 only: GTA San Andreas 1.0 US is a 32-bit executable
-- Vendored deps: external/miniaudio (single header), external/injector (submodule).

workspace "earshot_reminid"
	configurations { "Release", "Debug" }
	platforms { "x86" }
	defaultplatform "x86"
	location "build"

	files { "src/**.*" }

	-- subhook 0.8.2 (vendored copy of external/EarShotOpenAL/source/include/
	-- subhook-0.8.2 — a copy-out, NOT an edit of the reference tree).
	-- Only subhook.c is compiled: it textually #includes subhook_x86.c and
	-- subhook_windows.c, so adding those separately would duplicate symbols.
	files { "external/subhook/subhook.c" }
	files { "external/subhook/*.h" }

	-- plugin-sdk (master) ships headers ONLY in E:/SDKs/plugin-sdk-master — no
	-- output/lib/plugin.lib like the Hzan original linked (EarShot.vcxproj:153).
	-- These shared/*.cpp provide the symbols plugin.h's inline machinery calls:
	--   DynAddress.cpp — plugin::GetGlobalAddress/GetBaseAddress (used by every
	--                    Events hook install: injector/hooking.hpp:394)
	--   GameVersion.cpp — plugin::GetGameVersion/by_version_dyn (EventList.h:137)
	--   Patch.cpp       — plugin::patch::* (GameVersion.cpp detect_game_id)
	files { "E:/SDKs/plugin-sdk-master/shared/DynAddress.cpp" }
	files { "E:/SDKs/plugin-sdk-master/shared/GameVersion.cpp" }
	files { "E:/SDKs/plugin-sdk-master/shared/Patch.cpp" }

	includedirs { "src" }
	includedirs { "external/injector/include" }
	includedirs { "external/miniaudio" }
	includedirs { "external/subhook" }

	-- plugin-sdk (v1004) — mirrored exactly from skygfx_plus_expIV/premake5.lua:41-42
	includedirs { "E:/SDKs/plugin-sdk-master/plugin_sa/game_sa" }
	includedirs { "E:/SDKs/plugin-sdk-master/shared" }
	-- The rest of the include set comes from plugin-sdk's OWN premake
	-- (tools/premake/premake5.lua:231-239) — without it its headers don't compile:
	includedirs { "E:/SDKs/plugin-sdk-master/plugin_sa" }                          -- project root (plugin_sa.h)
	includedirs { "E:/SDKs/plugin-sdk-master/shared/game" }                        -- CVector.h/CVector2D.h (also in HZ EarShot.vcxproj:142)
	includedirs { "E:/SDKs/plugin-sdk-master/plugin_sa/game_sa/enums" }            -- eWeaponType.h / eAudioEvents.h / ...
	includedirs { "E:/SDKs/plugin-sdk-master/plugin_sa/game_sa/rw" }               -- RenderWare SDK headers (RW define)
	includedirs { "E:/SDKs/plugin-sdk-master/safetyhook" }                         -- EventList.h -> injector/hooking.hpp -> safetyhook.hpp

	-- Optional deploy target: when SA_GAME_DIR is set in the environment,
	-- the built ASI is additionally copied to that directory after every
	-- build as earshot_reminid.asi. Unset => no copy step at all.
	deploydir = os.getenv("SA_GAME_DIR")

project "earshot_reminid"
	kind "SharedLib"
	language "C++"
	targetdir "bin/%{cfg.buildcfg}"
	targetextension ".asi"
	characterset ("MBCS")
	staticruntime "On"
	systemversion "10.0.26100.0"
	-- plugin-sdk master's hooking chain (EventList.h -> ../injector/hooking.hpp ->
	-- safetyhook.hpp) uses std::optional/std::expected/nodiscard => needs the MSVC
	-- latest C++ standard; default C++14 fails with C2039/C4869 storms.
	cppdialect "c++latest"
	-- SSE2 instruction set comes from Directory.Build.props
	-- (<EnableEnhancedInstructionSet>StreamingSIMDExtensions2</EnableEnhancedInstructionSet>).

	defines { "_CRT_NONSTDC_NO_WARNINGS", "_CRT_SECURE_NO_WARNINGS" }
	-- plugin-sdk target-game/version macros: exactly what plugin-sdk's own premake
	-- sets for SA (tools/premake/premake5.lua:178) — GTASA enables the game sections
	-- in Events.h/CCamera.h etc.; PLUGIN_SGV_10US restricts to SA 1.0 US (our host);
	-- RW pulls shared/RenderWare.h -> the vendored RenderWare SDK (game_sa/rw).
	defines { "GTASA", "PLUGIN_SGV_10US", "RW" }
	defines { "_CRT_NON_CONFORMING_SWPRINTFS", "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING" }
	-- subhook is linked statically (README.md:14): without SUBHOOK_STATIC the
	-- header declares __declspec(dllimport) on Windows and linking fails.
	defines { "SUBHOOK_STATIC" }
	buildoptions { "/FS" }

	filter "configurations:Debug"
		defines { "DEBUG" }
		symbols "On"

	filter "configurations:Release"
		defines { "NDEBUG" }
		optimize "On"

	filter {}
	if deploydir then
		postbuildcommands { 'copy /y "$(TargetPath)" "' .. deploydir .. '\\earshot_reminid.asi"' }
	end
