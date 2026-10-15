@echo off
REM Generate Visual Studio solution for Earshot_reminid.
REM Prefers the local premake5 in tools\ (no fallback path).
setlocal
cd /d "%~dp0"

if exist "tools\premake5.exe" (
	"tools\premake5.exe" vs2022 %*
) else (
	echo [premake5.cmd] ERROR: tools\premake5.exe not found.
	exit /b 1
)
endlocal
