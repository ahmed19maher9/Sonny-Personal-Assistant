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

REM Always build from the repository root: <File Source="..."> paths in
REM Product.wxs are resolved against the current working directory, and
REM <Files Include="..."> against installer\. Running wix from anywhere
REM else silently produces a package with unresolved or missing sources.
cd /d "%~dp0"

REM 3. Build application
REM    Call build.bat by absolute path: a bare "build.bat" is only resolved
REM    when the current directory is searched for executables, which
REM    NoDefaultCurrentDirectoryInExePath can disable.
call "%~dp0build.bat"
if %ERRORLEVEL% NEQ 0 goto :build_error

REM 4. Verify the payload actually exists before handing it to WiX, so a
REM    failed or skipped build is reported here instead of as a WiX error
REM    about one missing file out of ~500.
if not exist "build\Release\personal_assistant.exe" goto :payload_error
for %%F in (
    "build\Release\personal_assistant.exe"
    "build\Release\WebView2Loader.dll"
    "build\Release\sonny_relay.exe"
    "build\Release\ctranslate2.dll"
    "build\Release\onnxruntime.dll"
    "build\Release\libespeak-ng.dll"
    "build\Release\openblas.dll"
    "build\Release\cudart64_12.dll"
    "build\Release\cublas64_12.dll"
    "build\Release\cublasLt64_12.dll"
    "build\Release\web\web_speech.html"
    "build\Release\web\mesh.html"
    "build\Release\web\vis-network.min.js"
    "build\Release\avatars\sonny.glb"
    "build\Release\avatars\iron-man.glb"
    "build\Release\sounds\sonny.wav"
    "build\Release\sounds\sonny_my_name_is_sonny.wav"
    "build\Release\sounds\sonny_yes.wav"
    "build\Release\videos\logo-loading-green.wmv"
    "build\Release\videos\logo-start-green.wmv"
    "build\Release\models\silero_vad.onnx"
    "build\Release\models\bge-large-en-v1.5\model_int8.onnx"
    "build\Release\models\bge-large-en-v1.5\vocab.txt"
    "build\Release\models\whisper-base-ct2\config.json"
    "build\Release\models\whisper-base-ct2\model.bin"
    "build\Release\models\whisper-base-ct2\tokenizer.json"
    "build\Release\models\whisper-base-ct2\vocabulary.txt"
    "build\Release\models\openwakeword\melspectrogram.onnx"
    "build\Release\models\openwakeword\embedding_model.onnx"
    "build\Release\models\openwakeword\sonny.onnx"
    "build\Release\kokoro\Kokoro-82M\config.json"
    "build\Release\kokoro\onnx\kokoro_encoder.onnx"
    "build\Release\kokoro\onnx\har_generator.onnx"
    "build\Release\kokoro\onnx\kokoro_decoder.onnx"
) do (
    if not exist %%F (
        echo [ERROR] Missing staged payload: %%F
        goto :payload_error
    )
)
if not exist "build\Release\kokoro\voices_npy" goto :payload_error
if not exist "build\Release\kokoro\espeak-ng-data" goto :payload_error
if not exist "build\Release\kokoro\misaki-data" goto :payload_error

REM 5. Prepare directories
if not exist "installer_output" mkdir "installer_output"
if exist "installer_output\Sonny.msi" del /q "installer_output\Sonny.msi"

REM 6. Build installer.
REM    -arch x64 is required: personal_assistant.exe is a 64-bit binary and an
REM    x86 package would land it under Program Files (x86) via the 32-bit
REM    ProgramFilesFolder redirection. The default is x86, so this must be
REM    passed explicitly.
echo [INFO] Running WiX build (x64)...
wix build -arch x64 -o "installer_output\Sonny.msi" "installer\Product.wxs"
if %ERRORLEVEL% NEQ 0 goto :wix_build_error

if not exist "installer_output\Sonny.msi" goto :wix_build_error

for %%F in ("installer_output\Sonny.msi") do echo [SUCCESS] Build Complete: %%~fF (%%~zF bytes)
exit /b 0

:wix_error
echo [ERROR] wix.exe not found. Install the WiX Toolset v4 or newer.
exit /b 1

:build_error
echo [ERROR] Build failed.
exit /b 1

:payload_error
echo [ERROR] The staged payload in build\Release is incomplete.
echo         Run cmake.bat to rebuild and restage the resources, then retry.
exit /b 1

:wix_build_error
echo [ERROR] WiX build failed.
exit /b 1
