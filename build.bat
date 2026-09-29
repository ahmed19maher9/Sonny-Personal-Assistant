@echo off
REM Fast incremental build script - assumes CMake has already been run
REM This script builds the existing project without reconfiguring
REM
REM Exits with the MSBuild exit code so callers (notably build_installer.bat)
REM can detect a failed compile instead of packaging stale binaries.

echo Building project with MSBuild (fully optimized)...
pushd "%~dp0build" || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
msbuild personal_assistant.vcxproj /p:Configuration=Release /maxcpucount
set BUILD_RC=%ERRORLEVEL%
popd
if not "%BUILD_RC%"=="0" (
    echo.
    echo [ERROR] MSBuild failed with exit code %BUILD_RC%.
    exit /b %BUILD_RC%
)

echo Build complete!
echo.
echo Optimizations applied:
echo - MSBuild with /maxcpucount (uses all CPU cores)
echo - Release configuration with full optimizations
echo - No CMake reconfiguration (faster for incremental builds)
echo.
echo Use cmake.bat for clean rebuilds when CMakeLists.txt changes
exit /b 0
