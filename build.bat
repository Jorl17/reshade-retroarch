@echo off
rem Builds everything into out\build (Release, x64). Needs Visual Studio 2022
rem (or its Build Tools) with the C++ workload and the Windows SDK.
rem Extra arguments go to cmake --build, e.g. build.bat --target RetroArchShaders
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || (echo Could not set up the MSVC environment & exit /b 1)
cmake -S . -B out\build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build out\build %* || exit /b 1
