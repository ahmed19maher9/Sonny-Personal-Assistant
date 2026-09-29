@echo off
REM Fast incremental build script - assumes CMake has already been run
REM This script builds the existing project without reconfiguring

echo Building project with MSBuild (fully optimized)...
cd build
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
msbuild personal_assistant.vcxproj /p:Configuration=Release /maxcpucount

echo Build complete!
echo.
echo Optimizations applied:
echo - MSBuild with /maxcpucount (uses all CPU cores)
echo - Release configuration with full optimizations
echo - No CMake reconfiguration (faster for incremental builds)
echo.
echo Use cmake.bat for clean rebuilds when CMakeLists.txt changes
