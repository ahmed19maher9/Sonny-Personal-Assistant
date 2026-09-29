@echo off
REM Build script for Personal Assistant with optimizations
REM Configures (first run only / when CMakeLists.txt changes) and builds incrementally.
REM Usage: cmake.bat          -> incremental configure + build
REM        cmake.bat clean    -> wipe build directory and rebuild from scratch

if /i "%~1"=="clean" (
    echo Cleaning build directory...
    rmdir /s /q build 2>nul
)

if not exist build mkdir build

echo Configuring with CMake (Release mode with optimizations)...
cd build
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" .. -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail

echo Building project with parallel jobs...
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build . --config Release -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 goto :fail
cd ..

echo.
echo Build complete!
echo.
echo Build configuration:
echo - Visual Studio 2022 generator with x64 architecture
echo - Multi-process compilation (/MP) and precompiled headers
echo - Release optimizations (/O2 /Oi /Ot /GL /arch:AVX2)
echo - Parallel jobs using all CPU cores (-j flag)
goto :eof

:fail
cd ..
echo.
echo Build FAILED. See the CMake/MSBuild output above for details.
exit /b 1
