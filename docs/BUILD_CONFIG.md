# Build Configuration Guide

## External Library Path Configuration

This project uses external libraries (CTranslate2, ONNX Runtime, etc.) organized within the project's `external/` directory.

### Path Resolution Strategy

The build system uses relative paths based on the internal directory structure:

- **Development**: External libraries are located in the `external/` subdirectory
- **Installation**: The installer bundles all required DLLs alongside the executable
- **No environment variables or hardcoded paths needed**

### Expected Directory Structure

#### Development Mode
```
sonny/                          # Project root
├── CMakeLists.txt
├── external/                   # External dependencies
│   ├── CTranslate2/           # CTranslate2 library
│   │   ├── include/
│   │   └── build-cpu/Release/
│   ├── onnxruntime/          # ONNX Runtime library
│   │   ├── include/
│   │   └── lib/
│   ├── llama.cpp/             # Llama.cpp library
│   │   ├── include/
│   │   └── build/
│   ├── kokoro-server/         # Kokoro TTS server
│   │   └── build/Release/
│   └── espeak-ng-dev/         # eSpeak NG development
│       └── lib/
├── models/                    # Model files
├── avatars/                   # Avatar files
├── sounds/                    # Sound files
└── src/                       # Source code
```

#### Installed Application
```
Program Files/Sonny/
├── personal_assistant.exe
├── ctranslate2.dll          # Bundled by installer
├── onnxruntime.dll          # Bundled by installer
├── models/
├── avatars/
└── sounds/
```

### CMake Path Configuration

The CMakeLists.txt uses these relative paths:

```cmake
# CTranslate2 (in external/CTranslate2)
set(CT2_BASE_DIR "${CMAKE_SOURCE_DIR}/external/CTranslate2")

# ONNX Runtime (in external/onnxruntime)
set(ONNXRUNTIME_BASE_DIR "${CMAKE_SOURCE_DIR}/external/onnxruntime")

# Llama.cpp (in external/llama.cpp)
set(LLAMA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/llama.cpp")

# Kokoro server (in external/kokoro-server)
set(KOKORO_MT_DIR "${CMAKE_SOURCE_DIR}/external/kokoro-server/mt")
```

### External Dependencies Setup

The `external/` directory contains copies or references to external dependencies:

- **CTranslate2**: Speech-to-text engine (Whisper)
- **ONNX Runtime**: Inference engine for VAD models
- **llama.cpp**: LLM inference engine
- **kokoro-server**: Text-to-speech engine
- **espeak-ng-dev**: Phonemizer for TTS

These are organized internally to make the project self-contained and portable.

### Runtime Path Resolution

The application automatically detects whether it's running in development or production mode:

- **Development Mode**: Resources are referenced from the project root
- **Production Mode**: Resources are bundled alongside the executable

This detection is performed in `src/core/main.cpp` by checking for the presence of expected files.

### Installation Considerations

For installation, the installer (defined in `installer/Product.wxs`) handles:
- Copying all required DLLs to the installation directory
- Setting up the proper directory structure
- Ensuring the application can find all dependencies at runtime

### Troubleshooting

**Build errors about missing libraries:**
- Verify the `external/` directory structure matches the expected layout
- Ensure external libraries are present in the correct subdirectories
- Check that the required .lib files exist in the expected locations

**Runtime errors about missing DLLs:**
- In development: Check that DLLs are copied to the build output directory
- In production: Verify the installer bundled all required DLLs
- Check the build output directory for copied DLLs