@echo off
REM ============================================================
REM b.bat - build dispatcher for earshot_reminid
REM
REM Optional env vars:
REM   SA_GAME_DIR   game dir to deploy bin\<cfg>\earshot_reminid.asi
REM                 into (root of the SA install). Unset => no deploy.
REM ============================================================

setlocal
cd /d "%~dp0"

call premake5.cmd %*

if not exist build\earshot_reminid.sln (
	echo [b.bat] ERROR: build\earshot_reminid.sln not found - premake generation failed.
	exit /b 1
)

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"

if not defined MSBUILD (
	echo [b.bat] ERROR: MSBuild not found via vswhere.
	exit /b 1
)

"%MSBUILD%" build\earshot_reminid.sln /p:Configuration=Release /p:Platform=Win32 /m /v:m
if errorlevel 1 (
	echo [b.bat] ERROR: build failed - nothing to deploy.
	exit /b 1
)

REM --- optional deploy: copy bin\<cfg>\earshot_reminid.asi into SA_GAME_DIR ---
if defined SA_GAME_DIR (
	if exist "bin\Release\earshot_reminid.asi" (
		copy /y "bin\Release\earshot_reminid.asi" "%SA_GAME_DIR%\earshot_reminid.asi" >nul && (
			echo [b.bat] Deployed to %SA_GAME_DIR%\earshot_reminid.asi
		) || (
			echo [b.bat] WARNING: deploy failed ^(game running? dir missing?^) - build output is in bin\Release\earshot_reminid.asi
		)
	) else (
		echo [b.bat] WARNING: bin\Release\earshot_reminid.asi not found - nothing to deploy.
	)
) else (
	echo [b.bat] SA_GAME_DIR not set - skipping deploy.
)

endlocal
