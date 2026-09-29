@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist out\quick mkdir out\quick
cl /nologo /std:c++17 /O2 /EHsc /W4 /MT /utf-8 /DNOMINMAX tools\detect_test.cpp src\grid_detect.cpp /Fo:out\quick\ /Fe:out\quick\detect_test.exe || exit /b 1
