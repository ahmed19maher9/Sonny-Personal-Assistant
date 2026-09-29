@echo off
setlocal

echo ========================================
echo Sonny WiX Installer Build
echo ========================================

REM 1. Auto-accept WiX EULA
wix eula accept wix7 >nul 2>&1

REM 2. Check for WiX
where wix >nul 2>&1
if %ERRORLEVEL% NEQ 0 goto :wix_error

REM 3. Build application
call build.bat
if %ERRORLEVEL% NEQ 0 goto :build_error

REM Return to project root (build.bat changes into build/)
cd /d "%~dp0"

REM 4. Prepare directories
if not exist "installer_output" mkdir "installer_output"

REM 5. Generate GUID
for /f %%G in ('powershell -Command "[guid]::NewGuid().ToString()"') do set UPGRADE_GUID=%%G

REM 6. Update Product.wxs using absolute path
echo [INFO] Updating GUID in Product.wxs...
powershell -Command "$path = 'installer\Product.wxs'; (Get-Content $path) -replace 'GUID-PLACEHOLDER', '%UPGRADE_GUID%' | Set-Content $path"
if %ERRORLEVEL% NEQ 0 goto :wix_build_error

REM 7. Build installer using absolute paths
echo [INFO] Running WiX build...
wix build -o "installer_output\Sonny.msi" "installer\Product.wxs"
if %ERRORLEVEL% NEQ 0 goto :wix_build_error

echo [SUCCESS] Build Complete.
pause
exit /b 0

:wix_error
echo [ERROR] wix.exe not found.
pause
exit /b 1

:build_error
echo [ERROR] Build failed.
pause
exit /b 1

:wix_build_error
echo [ERROR] WiX build failed.
pause
exit /b 1