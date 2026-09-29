cmake_minimum_required(VERSION 3.14)
project(PersonalAssistant)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    # C/C++ only: these flags must not leak into CUDA compilation, because /MP
    # makes MSBuild hand several .cu files to a single nvcc call ("nvcc fatal :
    # A single input file is required for a non-link phase").
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/EHsc>")
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/MP>")
    # Conforming preprocessor — faster variadic-macro expansion, required for
    # full C++20 compliance (kokoro-mt already uses C++20 internally).
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/Zc:preprocessor>")
    add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
    add_compile_definitions(NOMINMAX)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /O2 /Oi /Ot /Oy /GL /GF /Gy /arch:AVX2")
    set(CMAKE_LINK_FLAGS_RELEASE "${CMAKE_LINK_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    set(CMAKE_LINK_FLAGS_DEBUG "${CMAKE_LINK_FLAGS_DEBUG} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Every third-party dependency lives under external/ (see "External dependencies").
set(SONNY_EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")
set(LLAMA_SOURCE_DIR "${SONNY_EXTERNAL_DIR}/llama.cpp")
set(LLAMA_BUILD_DIR "${LLAMA_SOURCE_DIR}/build")
set(LLAMA_INCLUDE_DIR "${LLAMA_SOURCE_DIR}/include")
set(LLAMA_LIB_DIR "${LLAMA_BUILD_DIR}/src/Release")
set(LLAMA_BIN_DIR "${LLAMA_BUILD_DIR}/bin/Release")

# ---- CTranslate2 (STT: Whisper large-v3-turbo via ctranslate2) ----
# External library located in external/CTranslate2
set(CT2_BASE_DIR "${SONNY_EXTERNAL_DIR}/CTranslate2")
set(CT2_INCLUDE_DIR "${CT2_BASE_DIR}/include")
set(CT2_LIB "${CT2_BASE_DIR}/build-cpu/Release/ctranslate2.lib")
set(CT2_CPUFEATURES_LIB "${CT2_BASE_DIR}/build-cpu/third_party/cpu_features/Release/cpu_features.lib")
set(CT2_DLL "${CT2_BASE_DIR}/build-cpu/Release/ctranslate2.dll")

# ---- ONNX Runtime (Silero VAD) ----
# External library located in external/onnxruntime
set(ONNXRUNTIME_BASE_DIR "${SONNY_EXTERNAL_DIR}/onnxruntime")
set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_BASE_DIR}/include")
set(ONNXRUNTIME_LIB "${ONNXRUNTIME_BASE_DIR}/lib/onnxruntime.lib")
set(ONNXRUNTIME_DLL "${ONNXRUNTIME_BASE_DIR}/lib/onnxruntime.dll")

# ============================================================================
# External dependencies
# ----------------------------------------------------------------------------
# Everything the build needs lives in external/. Source projects that are
# missing are git-cloned at a pinned revision, and build artifacts that are
# missing are produced by this project - no setup scripts and no manual steps.
# ============================================================================
set(KOKORO_BASE_DIR "${SONNY_EXTERNAL_DIR}/kokoro-server")
set(KOKORO_MT_DIR "${KOKORO_BASE_DIR}/mt")
set(OPENBLAS_DIR "${SONNY_EXTERNAL_DIR}/OpenBLAS")
set(ESPEAK_DIR "${SONNY_EXTERNAL_DIR}/espeak-ng-dev")
set(SONNY_CUDA_BIN_DIR "${SONNY_EXTERNAL_DIR}/cuda/bin")
# CUDA / Vulkan link-time dependencies of the llama.cpp GPU backends. The
# ggml-cuda and ggml-vulkan static archives are linked by file path (they are
# built out-of-tree by the llama.cpp configure below), so the import libraries
# their CMake targets use (CUDA::cudart, CUDA::cublas, CUDA::cuda_driver,
# Vulkan::Vulkan) have to be added here as well.
set(SONNY_CUDA_LIB_DIR "$ENV{CUDA_PATH}/lib/x64")
set(SONNY_CUDA_TOOLKIT_BIN "$ENV{CUDA_PATH}/bin")
set(SONNY_VULKAN_LIB_DIR "$ENV{VULKAN_SDK}/Lib")
set(SONNY_VULKAN_BIN_DIR "$ENV{VULKAN_SDK}/Bin")

# Pinned upstream revisions the prebuilt artifacts in external/ were built from.
set(SONNY_LLAMA_REPO "https://github.com/ggml-org/llama.cpp" CACHE STRING "llama.cpp git remote")
set(SONNY_LLAMA_REF "9f70b2cecd1a9a3f73ac525c47ca22a6ee9a7b69" CACHE STRING "llama.cpp pinned revision (b11194 - upstream no longer implements the Mllama / Llama 3.2 Vision architecture)")
set(SONNY_KOKORO_REPO "https://github.com/marty1885/kokoro-server" CACHE STRING "kokoro-server git remote")
set(SONNY_KOKORO_REF "952be751c3c4ee325a8049fa931eae037ef55f3b" CACHE STRING "kokoro-server pinned revision")

# Clone a dependency (with its submodules) when the sources are not there yet.
function(sonny_clone_project dir repo ref)
    if (EXISTS "${dir}/CMakeLists.txt")
        message(STATUS "external: ${dir} present")
        return()
    endif()
    find_package(Git REQUIRED)
    message(STATUS "external: cloning ${repo} -> ${dir}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" clone --recurse-submodules "${repo}" "${dir}"
        RESULT_VARIABLE _clone_result)
    if (NOT _clone_result EQUAL 0)
        message(FATAL_ERROR "git clone ${repo} failed (exit code ${_clone_result})")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" checkout --recurse-submodules "${ref}"
        RESULT_VARIABLE _checkout_result)
    if (NOT _checkout_result EQUAL 0)
        message(FATAL_ERROR "git checkout ${ref} failed in ${dir} (exit code ${_checkout_result})")
    endif()
endfunction()

# Prebuilt binary packages cannot be cloned: report the exact missing path.
function(sonny_require_dependency name path)
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "${name} is missing: ${path}\nPut the dependency inside external/.")
    endif()
endfunction()

sonny_clone_project("${LLAMA_SOURCE_DIR}" "${SONNY_LLAMA_REPO}" "${SONNY_LLAMA_REF}")
sonny_clone_project("${KOKORO_BASE_DIR}" "${SONNY_KOKORO_REPO}" "${SONNY_KOKORO_REF}")

# ----------------------------------------------------------------------------
# Supported GGUF architectures, extracted from the pinned llama.cpp sources.
# llama.cpp rejects a model whose `general.architecture` it does not implement
# ("unknown model architecture: '<name>'"), which Sonny could previously only
# report as "failed to load model". The header generated here is consumed by
# LlamaWrapper (see inspect_model_file()) so the failure names the architecture,
# explains what the model is and lists what this build actually loads. Extracting
# the names keeps the list in sync with the linked library - no manual table.
# ----------------------------------------------------------------------------
set(SONNY_LLAMA_ARCHS_HEADER "${CMAKE_BINARY_DIR}/generated/sonny_llama_archs.h")
set(_sonny_archs "")
if (EXISTS "${LLAMA_SOURCE_DIR}/src/llama-arch.cpp")
    file(READ "${LLAMA_SOURCE_DIR}/src/llama-arch.cpp" _sonny_arch_src)
    string(REGEX MATCHALL "LLM_ARCH_[A-Z0-9_]+ *, *\"[a-z0-9_.-]+\"" _sonny_arch_entries "${_sonny_arch_src}")
    foreach(_sonny_arch_entry IN LISTS _sonny_arch_entries)
        string(REGEX REPLACE ".*\"([a-z0-9_.-]+)\"$" "\\1" _sonny_arch "${_sonny_arch_entry}")
        # "clip" is a dummy architecture only llama-quantize accepts; a GGUF
        # carrying it is a projector, which LlamaWrapper reports separately.
        if (NOT _sonny_arch STREQUAL "clip")
            list(APPEND _sonny_archs "${_sonny_arch}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _sonny_archs)
    list(SORT _sonny_archs)
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
set(_sonny_arch_header_text
"// Generated by CMake from external/llama.cpp/src/llama-arch.cpp - do not edit.\n// GGUF architectures (general.architecture) implemented by the llama.cpp\n// revision Sonny links against; used by LlamaWrapper::inspect_model_file().\n#pragma once\n\nstatic const char * const SONNY_LLAMA_SUPPORTED_ARCHS[] = {\n")
foreach(_sonny_arch IN LISTS _sonny_archs)
    string(APPEND _sonny_arch_header_text "    \"${_sonny_arch}\",\n")
endforeach()
string(APPEND _sonny_arch_header_text "    nullptr,\n};\n")
file(WRITE "${SONNY_LLAMA_ARCHS_HEADER}" "${_sonny_arch_header_text}")

list(LENGTH _sonny_archs _sonny_arch_count)
if (_sonny_arch_count EQUAL 0)
    message(WARNING "could not extract the llama.cpp architecture list from ${LLAMA_SOURCE_DIR}/src/llama-arch.cpp - unsupported models will be reported without the list of loadable architectures")
else()
    message(STATUS "llama.cpp architectures: ${_sonny_arch_count} extracted from src/llama-arch.cpp")
endif()
# ----------------------------------------------------------------------------
# Self-provisioning of the remaining dependencies. Each one is pinned to the
# upstream revision the artifacts in external/ were originally produced from,
# so a fresh checkout ("delete external/ + run cmake.bat") builds everything
# from source or downloads an official binary - no files outside this repo
# are required.
# ----------------------------------------------------------------------------
set(SONNY_CT2_REPO "https://github.com/OpenNMT/CTranslate2" CACHE STRING "CTranslate2 git remote")
set(SONNY_CT2_REF "d44d2d069eb88c7b7804da864c10c201501cb4a9" CACHE STRING "CTranslate2 pinned revision (v4.8.2)")
set(SONNY_ESPEAK_REPO "https://github.com/espeak-ng/espeak-ng" CACHE STRING "espeak-ng git remote")
set(SONNY_ESPEAK_REF "f1354994057fa9b85001675732e7fed2d437292b" CACHE STRING "espeak-ng pinned revision")
set(SONNY_OPENBLAS_REPO "https://github.com/OpenMathLib/OpenBLAS" CACHE STRING "OpenBLAS git remote")
set(SONNY_OPENBLAS_REF "v0.3.29" CACHE STRING "OpenBLAS pinned revision")
set(SONNY_ORT_VERSION "1.28.0" CACHE STRING "ONNX Runtime release version (matches ORT API v28)")
set(SONNY_CUDA_RUNTIME_VERSION "12.6" CACHE STRING "CUDA runtime DLL set used by ggml-cuda")

# Configure (first call) and build (Release) a cloned project when a specific
# artifact is still missing. Extra arguments are forwarded as -D flags to the
# configure step. The result is cached, so later builds are incremental.
function(sonny_build_from_source artifact src_dir build_dir)
    if (EXISTS "${artifact}")
        return()
    endif()
    message(STATUS "external: ${artifact} missing - building ${src_dir} from source")
    if (NOT EXISTS "${build_dir}")
        message(STATUS "external: configuring ${src_dir}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${src_dir}" -B "${build_dir}"
            -G "Visual Studio 17 2022" -A x64
            -DCMAKE_BUILD_TYPE=Release ${ARGN}
            RESULT_VARIABLE _configure_result)
        if (NOT _configure_result EQUAL 0)
            message(FATAL_ERROR "CMake configure failed for ${src_dir} (exit code ${_configure_result})")
        endif()
    endif()
    message(STATUS "external: building ${src_dir} (Release) - this may take a while")
    # Use all available cores for parallel dependency compilation.
    if (NOT CMAKE_BUILD_PARALLEL_LEVEL)
        cmake_host_system_information(RESULT _cpu_count QUERY NUMBER_OF_LOGICAL_CORES)
    else()
        set(_cpu_count "${CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release
        --parallel "${_cpu_count}"
        RESULT_VARIABLE _build_result)
    if (NOT _build_result EQUAL 0)
        message(FATAL_ERROR "CMake build failed for ${src_dir} (exit code ${_build_result})")
    endif()
    if (NOT EXISTS "${artifact}")
        message(FATAL_ERROR "Build of ${src_dir} did not produce the expected artifact: ${artifact}")
    endif()
endfunction()
# Non-fatal variant of sonny_build_from_source: reports the configure/build
# exit code in <out_var> instead of aborting the configure step, so callers
# can retry with a different configuration (used by the llama.cpp CUDA
# fallback below).
function(sonny_build_from_source_try out_var artifact src_dir build_dir)
    if (EXISTS "${artifact}")
        set(${out_var} 0 PARENT_SCOPE)
        return()
    endif()
    message(STATUS "external: ${artifact} missing - building ${src_dir} from source")
    if (NOT EXISTS "${build_dir}")
        message(STATUS "external: configuring ${src_dir}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${src_dir}" -B "${build_dir}"
            -G "Visual Studio 17 2022" -A x64
            -DCMAKE_BUILD_TYPE=Release ${ARGN}
            RESULT_VARIABLE _try_configure_result)
        if (NOT _try_configure_result EQUAL 0)
            set(${out_var} "${_try_configure_result}" PARENT_SCOPE)
            return()
        endif()
    endif()
    message(STATUS "external: building ${src_dir} (Release) - this may take a while")
    if (NOT CMAKE_BUILD_PARALLEL_LEVEL)
        cmake_host_system_information(RESULT _try_cpu_count QUERY NUMBER_OF_LOGICAL_CORES)
    else()
        set(_try_cpu_count "${CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release
        --parallel "${_try_cpu_count}"
        RESULT_VARIABLE _try_build_result)
    set(_try_result "${_try_build_result}")
    if (_try_result EQUAL 0 AND NOT EXISTS "${artifact}")
        set(_try_result 1)  # build "succeeded" but the artifact is still missing
    endif()
    set(${out_var} "${_try_result}" PARENT_SCOPE)
endfunction()


# ---- OpenBLAS (BLAS kernels used by the Kokoro iSTFT) ------------------------
if (NOT EXISTS "${OPENBLAS_DIR}/lib/openblas.lib")
    set(SONNY_OPENBLAS_SRC "${SONNY_EXTERNAL_DIR}/OpenBLAS-src")
    sonny_clone_project("${SONNY_OPENBLAS_SRC}" "${SONNY_OPENBLAS_REPO}" "${SONNY_OPENBLAS_REF}")
    sonny_build_from_source(
        "${SONNY_OPENBLAS_SRC}/build/lib/RELEASE/openblas.lib"
        "${SONNY_OPENBLAS_SRC}" "${SONNY_OPENBLAS_SRC}/build"
        -DBUILD_SHARED_LIBS=ON -DNOFORTRAN=ON -DC_LAPACK=ON)
    file(MAKE_DIRECTORY "${OPENBLAS_DIR}/lib" "${OPENBLAS_DIR}/bin" "${OPENBLAS_DIR}/include")
    file(GLOB SONNY_OPENBLAS_LIBS "${SONNY_OPENBLAS_SRC}/build/lib/RELEASE/openblas*.lib")
    file(COPY ${SONNY_OPENBLAS_LIBS} DESTINATION "${OPENBLAS_DIR}/lib")
    file(GLOB SONNY_OPENBLAS_DLLS "${SONNY_OPENBLAS_SRC}/build/lib/RELEASE/openblas*.dll")
    file(COPY ${SONNY_OPENBLAS_DLLS} DESTINATION "${OPENBLAS_DIR}/bin")
    file(GLOB SONNY_OPENBLAS_HEADERS "${SONNY_OPENBLAS_SRC}/*.h" "${SONNY_OPENBLAS_SRC}/build/*.h")
    file(COPY ${SONNY_OPENBLAS_HEADERS} DESTINATION "${OPENBLAS_DIR}/include")
endif()

# ---- eSpeak NG (phonemizer used by Kokoro/misaki) ----------------------------
if (NOT EXISTS "${ESPEAK_DIR}/lib/espeak-ng.lib")
    set(SONNY_ESPEAK_SRC "${SONNY_EXTERNAL_DIR}/espeak-ng-src")
    sonny_clone_project("${SONNY_ESPEAK_SRC}" "${SONNY_ESPEAK_REPO}" "${SONNY_ESPEAK_REF}")
    sonny_build_from_source(
        "${SONNY_ESPEAK_SRC}/build/src/libespeak-ng/Release/espeak-ng.lib"
        "${SONNY_ESPEAK_SRC}" "${SONNY_ESPEAK_SRC}/build"
        -DBUILD_SHARED_LIBS=ON -DUSE_ASYNC=OFF -DBUILD_TESTS=OFF)
    file(MAKE_DIRECTORY "${ESPEAK_DIR}/lib" "${ESPEAK_DIR}/bin" "${ESPEAK_DIR}/include")
    file(GLOB SONNY_ESPEAK_LIBS "${SONNY_ESPEAK_SRC}/build/src/libespeak-ng/Release/*.lib")
    file(COPY ${SONNY_ESPEAK_LIBS} DESTINATION "${ESPEAK_DIR}/lib")
    file(GLOB SONNY_ESPEAK_DLLS "${SONNY_ESPEAK_SRC}/build/src/Release/*.dll")
    file(COPY ${SONNY_ESPEAK_DLLS} DESTINATION "${ESPEAK_DIR}/bin")
    file(COPY "${SONNY_ESPEAK_SRC}/src/include/espeak-ng" DESTINATION "${ESPEAK_DIR}/include")
endif()

# ---- CTranslate2 (STT: Whisper large-v3-turbo) --------------------------------
sonny_clone_project("${CT2_BASE_DIR}" "${SONNY_CT2_REPO}" "${SONNY_CT2_REF}")
if (NOT EXISTS "${CT2_LIB}")
    sonny_build_from_source(
        "${CT2_LIB}"
        "${CT2_BASE_DIR}" "${CT2_BASE_DIR}/build-cpu"
        -DWITH_CUDA=OFF -DWITH_DNNL=OFF -DWITH_MKL=OFF
        -DWITH_OPENBLAS=ON
        "-DOPENBLAS_INCLUDE_DIR=${OPENBLAS_DIR}/include"
        "-DOPENBLAS_LIB_DIR=${OPENBLAS_DIR}/lib"
        -DBUILD_CLI=OFF -DLIB_ONLY=ON
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded)
endif()

# ---- ONNX Runtime (Silero VAD + Kokoro decoder) -------------------------------
# Pulled from the official release archive when the extracted package is not
# present in external/onnxruntime.
if (NOT EXISTS "${ONNXRUNTIME_BASE_DIR}/include/onnxruntime_cxx_api.h")
    set(SONNY_ORT_DIR_NAME "onnxruntime-win-x64-${SONNY_ORT_VERSION}")
    set(SONNY_ORT_ZIP "${SONNY_EXTERNAL_DIR}/${SONNY_ORT_DIR_NAME}.zip")
    set(SONNY_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${SONNY_ORT_VERSION}/${SONNY_ORT_DIR_NAME}.zip")
    message(STATUS "external: downloading ONNX Runtime ${SONNY_ORT_VERSION}")
    file(DOWNLOAD "${SONNY_ORT_URL}" "${SONNY_ORT_ZIP}" SHOW_PROGRESS STATUS _ort_status)
    list(GET _ort_status 0 _ort_code)
    if (NOT _ort_code EQUAL 0)
        file(REMOVE "${SONNY_ORT_ZIP}")
        message(FATAL_ERROR "Downloading ONNX Runtime failed: ${_ort_status} (${SONNY_ORT_URL})")
    endif()
    set(SONNY_ORT_STAGING "${SONNY_EXTERNAL_DIR}/_onnxruntime_extract")
    file(REMOVE_RECURSE "${SONNY_ORT_STAGING}")
    file(MAKE_DIRECTORY "${SONNY_ORT_STAGING}")
    file(ARCHIVE_EXTRACT INPUT "${SONNY_ORT_ZIP}" DESTINATION "${SONNY_ORT_STAGING}")
    file(REMOVE_RECURSE "${ONNXRUNTIME_BASE_DIR}")
    file(RENAME "${SONNY_ORT_STAGING}/${SONNY_ORT_DIR_NAME}" "${ONNXRUNTIME_BASE_DIR}")
    file(REMOVE_RECURSE "${SONNY_ORT_STAGING}")
    file(REMOVE "${SONNY_ORT_ZIP}")
endif()

# ---- CUDA runtime DLLs (cudart/cublas required by ggml-cuda.dll) --------------
# Copied once from a local CUDA toolkit install into external/cuda/bin so the
# runtime package does not depend on a system-wide CUDA install.
if (NOT EXISTS "${SONNY_CUDA_BIN_DIR}/cudart64_12.dll")
    if (EXISTS "${SONNY_CUDA_TOOLKIT_BIN}/cudart64_12.dll")
        message(STATUS "external: copying CUDA runtime DLLs from ${SONNY_CUDA_TOOLKIT_BIN}")
        file(MAKE_DIRECTORY "${SONNY_CUDA_BIN_DIR}")
        file(GLOB SONNY_CUDA_RUNTIME_DLLS
            "${SONNY_CUDA_TOOLKIT_BIN}/cudart64_12.dll"
            "${SONNY_CUDA_TOOLKIT_BIN}/cublas64_12.dll"
            "${SONNY_CUDA_TOOLKIT_BIN}/cublasLt64_12.dll")
        file(COPY ${SONNY_CUDA_RUNTIME_DLLS} DESTINATION "${SONNY_CUDA_BIN_DIR}")
    else()
        message(STATUS "external: CUDA toolkit not found - ggml-cuda runtime DLLs must be provided elsewhere (Vulkan backend still works)")
    endif()
endif()

# ---- WebView2 SDK (MeshDialog / mesh topology visualization) ----------------
# The header and import library ship as a NuGet package (Microsoft.Web.WebView2).
# Auto-fetch it so the build works on a fresh machine. The runtime DLL itself
# is loaded dynamically at runtime by the Edge WebView2 runtime already on
# Windows — only the compile-time header and a 3 KB import library are needed.
set(SONNY_WEBVIEW2_VERSION "1.0.4191.47" CACHE STRING "WebView2 NuGet version")
set(WEBVIEW2_BASE_DIR "${SONNY_EXTERNAL_DIR}/webview2")
set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_BASE_DIR}/build/native/include")
set(WEBVIEW2_LIB_DIR "${WEBVIEW2_BASE_DIR}/build/native/x64")
set(WEBVIEW2_DLL_LIB "${WEBVIEW2_LIB_DIR}/WebView2Loader.dll.lib")

if(NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/webview2.h")
    if(NOT EXISTS "${WEBVIEW2_BASE_DIR}/package.zip")
        message(STATUS "external: downloading WebView2 SDK ${SONNY_WEBVIEW2_VERSION}")
        file(DOWNLOAD
            "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${SONNY_WEBVIEW2_VERSION}"
            "${WEBVIEW2_BASE_DIR}/package.zip"
            SHOW_PROGRESS STATUS _wv_status)
        list(GET _wv_status 0 _wv_code)
        if(NOT _wv_code EQUAL 0)
            file(REMOVE "${WEBVIEW2_BASE_DIR}/package.zip")
            message(FATAL_ERROR "Downloading WebView2 SDK failed: ${_wv_status}")
        endif()
    endif()
    message(STATUS "external: extracting WebView2 SDK")
    file(ARCHIVE_EXTRACT
        INPUT "${WEBVIEW2_BASE_DIR}/package.zip"
        DESTINATION "${WEBVIEW2_BASE_DIR}")
    file(REMOVE "${WEBVIEW2_BASE_DIR}/package.zip")
endif()

message(STATUS "LLAMA_SOURCE_DIR: ${LLAMA_SOURCE_DIR}")
message(STATUS "LLAMA_BUILD_DIR: ${LLAMA_BUILD_DIR}")
message(STATUS "LLAMA_INCLUDE_DIR: ${LLAMA_INCLUDE_DIR}")
message(STATUS "LLAMA_LIB_DIR: ${LLAMA_LIB_DIR}")
message(STATUS "LLAMA_BIN_DIR: ${LLAMA_BIN_DIR}")

include_directories(${CMAKE_SOURCE_DIR}/resources/avatars)
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/core)
include_directories(${CMAKE_SOURCE_DIR}/src/audio)
include_directories(${CMAKE_SOURCE_DIR}/src/ai)
include_directories(${CMAKE_SOURCE_DIR}/src/net)
include_directories(${CMAKE_SOURCE_DIR}/src/browser)
include_directories(${CMAKE_SOURCE_DIR}/src/tools)
include_directories(${CMAKE_SOURCE_DIR}/src/tools/system)
include_directories(${CMAKE_SOURCE_DIR}/src/tools/media)
include_directories(${CMAKE_SOURCE_DIR}/src/tools/information)
include_directories(${CMAKE_SOURCE_DIR}/src/tools/search)
include_directories(${CMAKE_SOURCE_DIR}/src/tools/file)
include_directories(${CMAKE_SOURCE_DIR}/src/tools/productivity)
include_directories(${CMAKE_SOURCE_DIR}/src/ui)
include_directories(${CMAKE_SOURCE_DIR}/src/utils)
include_directories(${CMAKE_SOURCE_DIR}/src/resources)
include_directories(${LLAMA_INCLUDE_DIR})
include_directories(${LLAMA_SOURCE_DIR}/ggml/include)
include_directories(${ONNXRUNTIME_INCLUDE_DIR})
# NOTE: CTranslate2 headers are NOT added globally on purpose - they bundle a
# newer nlohmann/json than llama.cpp's common/ uses, and a global include
# directory breaks the llama-common build with "ambiguous symbol" errors.
# They are attached PRIVATE to personal_assistant below instead.

set(ASSISTANT_SOURCES
    # Core
    src/core/main.cpp
    src/core/AssistantOrchestrator.cpp
    src/core/ConfigManager.cpp
    src/core/SystemTray.cpp
    src/core/SettingsDialog.cpp
    src/core/FirstRunWizard.cpp
    
    # Audio
    src/audio/AudioCapture.cpp
    src/audio/AudioPlayback.cpp
    src/audio/STTEngineWrapper.cpp
    src/audio/WakeWordEngine.cpp
    src/audio/VADEngine.cpp
    src/audio/WebSpeechWrapper.cpp
    
    # AI
    src/ai/LlamaWrapper.cpp
    src/ai/KokoroWrapper.cpp
    src/ai/RagEngine.cpp
    src/ai/EmbeddingEngine.cpp
    
    # Browser
    src/browser/BrowserTool.cpp
    src/browser/BrowserServiceImpl.cpp
    src/browser/SiteFlows.cpp
    src/browser/BrowserPageScripts.cpp
    src/browser/BrowserWebSocket.cpp
    src/browser/ChromiumCdpSession.cpp
    src/browser/ChromiumLauncher.cpp
    src/browser/BrowserCrypto.cpp
    src/browser/UserProfileStore.cpp
    
    # Tools - System
    src/tools/system/SystemCommandTool.cpp
    src/tools/system/ProcessTool.cpp
    src/tools/system/ScreenControlTool.cpp
    src/tools/system/BatteryTool.cpp
    src/tools/system/WifiTool.cpp
    src/tools/system/VolumeControlTool.cpp
    
    # Tools - Media
    src/tools/media/MusicPlayerTool.cpp
    src/tools/media/AddMediaDirectoryTool.cpp
    src/tools/media/RemoveMediaDirectoryTool.cpp
    src/tools/media/ListMediaLibraryTool.cpp
    src/tools/media/CameraCaptureTool.cpp
    src/tools/media/FileIndex.cpp
    
    # Tools - Information
    src/tools/information/CalculatorTool.cpp
    src/tools/information/TimeTool.cpp
    src/tools/information/WeatherTool.cpp
    src/tools/information/NewsTool.cpp
    src/tools/information/UnitConverterTool.cpp
    src/tools/information/NoteTool.cpp
    
    # Tools - Search
    src/tools/search/WebSearchTool.cpp
    src/tools/search/GoogleSearchTool.cpp
    src/tools/search/YouTubeSearchTool.cpp
    src/tools/search/YouTubeVideoTool.cpp
    
    # Tools - File
    src/tools/file/FileTool.cpp
    src/tools/file/FileExplorerTool.cpp
    src/tools/file/ClipboardTool.cpp
    
    # Tools - Productivity
    src/tools/productivity/JokeTool.cpp
    src/tools/productivity/SpellingTool.cpp
    src/tools/productivity/StopwatchTool.cpp
    src/tools/productivity/ReminderTool.cpp
    src/tools/productivity/AppLauncherTool.cpp
    
    # UI
    src/ui/AvatarOverlay.cpp
    src/ui/VideoPlayer.cpp
    src/ui/CavaVisualizer.cpp
    src/core/MeshDialog.cpp
    src/core/CameraCapture.cpp
    src/core/VideoRecorder.cpp
    
    # Utils
    src/utils/Tool.cpp
    src/utils/PerfTimer.cpp
    src/utils/Trie.cpp

    # Net (WinHTTP client + local peer transport)
    src/net/HttpClient.cpp
    src/net/VectorsTypes.cpp
    src/net/VectorsCrypto.cpp
    src/net/PeerTransport.cpp
    src/net/VectorsSync.cpp
    
    # Resources
    src/resources/resources.rc
)

# ---------------------------------------------------------------------------
# Optional: compile the RAG embedding model into the executable.
#
# The assistant never downloads content, so the embedding model is either
# shipped in resources/models/ (staged next to the exe by the POST_BUILD copy
# below) or embedded here. Embedding removes the last loose file:
#
#   cmake -DSONNY_EMBED_MODEL_FILE=resources/models/all-MiniLM-L6-v2/model.onnx
#
# A ~23 MB int8 MiniLM export grows the .exe by ~23 MB and needs no filesystem
# access at runtime: EmbeddingEngine reads it back with FindResource().
# ---------------------------------------------------------------------------
set(SONNY_EMBED_MODEL_FILE "" CACHE FILEPATH "ONNX embedding model to compile into the executable (optional)")
set(SONNY_EMBED_VOCAB_FILE "" CACHE FILEPATH "WordPiece vocab.txt for SONNY_EMBED_MODEL_FILE (default: next to the model)")

if(SONNY_EMBED_MODEL_FILE AND EXISTS "${SONNY_EMBED_MODEL_FILE}")
    if(NOT SONNY_EMBED_VOCAB_FILE)
        get_filename_component(_embed_model_dir "${SONNY_EMBED_MODEL_FILE}" DIRECTORY)
        get_filename_component(_embed_model_parent "${_embed_model_dir}" DIRECTORY)
        if(EXISTS "${_embed_model_dir}/vocab.txt")
            set(SONNY_EMBED_VOCAB_FILE "${_embed_model_dir}/vocab.txt")
        elseif(EXISTS "${_embed_model_parent}/vocab.txt")
            set(SONNY_EMBED_VOCAB_FILE "${_embed_model_parent}/vocab.txt")
        endif()
    endif()

    if(SONNY_EMBED_VOCAB_FILE AND EXISTS "${SONNY_EMBED_VOCAB_FILE}")
        # rc.exe parses C-style escapes, so the paths must use forward slashes.
        file(TO_CMAKE_PATH "${SONNY_EMBED_MODEL_FILE}" _embed_model_fwd)
        file(TO_CMAKE_PATH "${SONNY_EMBED_VOCAB_FILE}" _embed_vocab_fwd)
        set(SONNY_EMBED_RC "${CMAKE_BINARY_DIR}/embedded_embedding_model.rc")
        file(WRITE "${SONNY_EMBED_RC}"
            "#include <windows.h>\n"
            "#include \"resource_ids.h\"\n"
            "IDR_EMBEDDING_MODEL RCDATA \"${_embed_model_fwd}\"\n"
            "IDR_EMBEDDING_VOCAB RCDATA \"${_embed_vocab_fwd}\"\n")
        set_source_files_properties("${SONNY_EMBED_RC}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        list(APPEND ASSISTANT_SOURCES "${SONNY_EMBED_RC}")
        message(STATUS "embedding: RAG embedding model compiled into the executable")
        message(STATUS "embedding:   model ${_embed_model_fwd}")
        message(STATUS "embedding:   vocab ${_embed_vocab_fwd}")
    else()
        message(WARNING "SONNY_EMBED_MODEL_FILE is set but no vocab.txt was found next to it - "
                        "the model will be loaded from disk instead")
    endif()
elseif(SONNY_EMBED_MODEL_FILE)
    message(WARNING "SONNY_EMBED_MODEL_FILE='${SONNY_EMBED_MODEL_FILE}' does not exist - ignored")
endif()

# Vision/multimodal support is conditionally compiled when mtmd is available.
# The mtmd static library is built by llama.cpp when LLAMA_BUILD_MTMD=ON.
# Set the path here so it is available for both the compile-definitions check
# and the later link step.
set(SONNY_MTMD_LIB "${LLAMA_BUILD_DIR}/tools/mtmd/Release/mtmd.lib")

add_executable(personal_assistant ${ASSISTANT_SOURCES})

if (EXISTS "${SONNY_MTMD_LIB}")
    target_compile_definitions(personal_assistant PRIVATE SONNY_HAS_MTMD)
endif()

# Everything under external/ is built with the static MSVC runtime (/MT).
set_target_properties(personal_assistant PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ---------------------------------------------------------------------------
# Unity (Jumbo) build — merges groups of 8 TUs into one compilation unit so
# the compiler runs ~8× instead of 59× on a clean build.  Files with
# conflicting anonymous-namespace symbols ("using json = nlohmann::json",
# "namespace fs = std::filesystem", etc.) are excluded individually so they
# compile normally while still benefiting from the PCH.
# ---------------------------------------------------------------------------
set_target_properties(personal_assistant PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 8)

# Files excluded from unity build (anonymous-namespace ODR conflicts)
set_source_files_properties(
    # Multiple browser/search files each define `using json = nlohmann::json`
    # inside an anonymous namespace — merging them would cause redefinition errors.
    src/browser/BrowserTool.cpp
    src/browser/BrowserServiceImpl.cpp
    src/tools/search/GoogleSearchTool.cpp
    src/tools/search/YouTubeSearchTool.cpp
    src/tools/search/YouTubeVideoTool.cpp
    # EmbeddingEngine.cpp defines `using json = nlohmann::json` in an
    # anonymous namespace (see line 10 of that file) — merging it into a
    # unity batch causes redefinition errors.
    src/ai/EmbeddingEngine.cpp
    # RagEngine.cpp defines `namespace fs = std::filesystem` in a non-
    # anonymous namespace — merging it into a unity batch causes redefinition
    # errors with other files that do the same.
    src/ai/RagEngine.cpp
    # VectorsTypes.cpp defines `using VectorsJson = nlohmann::json`
    # in an anonymous namespace — merging it into a unity batch causes
    # redefinition errors with other files that do the same.
    src/net/VectorsTypes.cpp
    # VectorsSync.cpp defines `using VectorsJson = nlohmann::json`
    # in an anonymous namespace — merging it into a unity batch causes
    # redefinition errors with other files that do the same.
    src/net/VectorsSync.cpp
    # MeshDialog.cpp pulls in <wrl/client.h> and <webview2.h>, which conflict
    # with PCH macros when merged into a unity batch; compile it standalone.
    src/core/MeshDialog.cpp
    # CameraCapture.cpp includes <initguid.h> before <dshow.h>; the unity
    # merge would reorder includes and break GUID definition ordering.
    src/core/CameraCapture.cpp
    # VideoRecorder.cpp defines helpers (ComPtr, RGB->I420 conversion) in an
    # anonymous namespace and pulls in the Media Foundation headers; keep it
    # out of the unity batches.
    src/core/VideoRecorder.cpp
    # CameraCaptureTool.cpp does the same and also aliases std::filesystem as
    # `fs`, which other media tools define as well.
    src/tools/media/CameraCaptureTool.cpp
    # RC files are never unity-compiled (not C++), but list them explicitly
    # so the intent is clear and cmake does not try to batch them.
    src/resources/resources.rc
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# ---------------------------------------------------------------------------
# Precompiled header — parses <windows.h>, the full STL, and Logger.h once;
# all 59 TUs reuse the pre-parsed result.  Expected: ~35-50% faster clean builds.
# The PCH is applied AFTER the unity-build exclusions so excluded TUs still use it.
# ---------------------------------------------------------------------------
    target_precompile_headers(personal_assistant PRIVATE src/sonny_pch.h)

target_link_libraries(personal_assistant "${CT2_LIB}")
target_link_libraries(personal_assistant "${CT2_CPUFEATURES_LIB}")
target_link_libraries(personal_assistant "${ONNXRUNTIME_LIB}")

if(EXISTS "${ONNXRUNTIME_DLL}")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_DLL}"
            $<TARGET_FILE_DIR:personal_assistant>)
endif()

if(EXISTS "${CT2_DLL}")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CT2_DLL}"
            $<TARGET_FILE_DIR:personal_assistant>)
endif()

# ---- llama.cpp (LLM) --------------------------------------------------------
# Prebuilt libraries in external/llama.cpp/build are used when present;
# otherwise llama.cpp is built from the sources in external/llama.cpp (which
# are git-cloned above when the directory was missing).
set(SONNY_LLAMA_PREBUILT_LIB "${LLAMA_LIB_DIR}/llama.lib")
set(SONNY_LLAMA_PREBUILT_COMMON "${LLAMA_BUILD_DIR}/common/Release/llama-common.lib")

if (NOT EXISTS "${SONNY_LLAMA_PREBUILT_LIB}")
    message(STATUS "external: prebuilt llama.cpp libraries missing - building llama.cpp from source")
    find_program(SONNY_NVCC nvcc HINTS "$ENV{CUDA_PATH}/bin")
    find_program(SONNY_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin")

    # GPU backends compiled into the static ggml. SONNY_LLAMA_CUDA and
    # SONNY_LLAMA_VULKAN accept ON, OFF or AUTO; AUTO enables a backend when
    # its toolchain is detected (nvcc / Vulkan SDK glslc) and skips it with a
    # status message otherwise.
    set(SONNY_LLAMA_CUDA "AUTO" CACHE STRING "llama.cpp CUDA backend: ON / OFF / AUTO (AUTO = on when nvcc is found)")
    set(SONNY_LLAMA_VULKAN "AUTO" CACHE STRING "llama.cpp Vulkan backend: ON / OFF / AUTO (AUTO = on when Vulkan SDK + glslc are found)")
    # native = build exactly for the GPU present at build time (sm_61 on the
    # GTX 1060), keeping the .cu compile times and the fat binary small.
    set(SONNY_LLAMA_CUDA_ARCH "native" CACHE STRING "CUDA architectures for the llama.cpp CUDA backend")

    set(SONNY_LLAMA_CUDA_BACKEND OFF)
    if ("${SONNY_LLAMA_CUDA}" STREQUAL "AUTO")
        if (SONNY_NVCC)
            set(SONNY_LLAMA_CUDA_BACKEND ON)
        endif()
    elseif (SONNY_LLAMA_CUDA)
        if (NOT SONNY_NVCC)
            message(FATAL_ERROR "SONNY_LLAMA_CUDA=ON but nvcc was not found - install the CUDA toolkit or set CUDA_PATH")
        endif()
        set(SONNY_LLAMA_CUDA_BACKEND ON)
    endif()

    set(SONNY_LLAMA_VULKAN_BACKEND OFF)
    if ("${SONNY_LLAMA_VULKAN}" STREQUAL "AUTO")
        if (SONNY_GLSLC)
            set(SONNY_LLAMA_VULKAN_BACKEND ON)
        endif()
    elseif (SONNY_LLAMA_VULKAN)
        if (NOT SONNY_GLSLC)
            message(FATAL_ERROR "SONNY_LLAMA_VULKAN=ON but glslc was not found - install the Vulkan SDK or set VULKAN_SDK")
        endif()
        set(SONNY_LLAMA_VULKAN_BACKEND ON)
    endif()

    set(SONNY_LLAMA_BACKENDS "CPU")
    if (SONNY_LLAMA_VULKAN_BACKEND)
        list(APPEND SONNY_LLAMA_BACKENDS "Vulkan")
    endif()
    if (SONNY_LLAMA_CUDA_BACKEND)
        list(APPEND SONNY_LLAMA_BACKENDS "CUDA")
    endif()

    set(LLAMA_CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF
        -DLLAMA_BUILD_COMMON=ON
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_BUILD_TOOLS=OFF
        -DLLAMA_BUILD_APP=OFF
        -DLLAMA_BUILD_SERVER=OFF
        -DLLAMA_BUILD_MTMD=ON
        -DLLAMA_CURL=OFF
        -DLLAMA_USE_SYSTEM_GGML=OFF
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    )
    if (SONNY_LLAMA_VULKAN_BACKEND)
        message(STATUS "external: llama.cpp Vulkan backend ON (glslc: ${SONNY_GLSLC})")
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_VULKAN=ON)
    else()
        message(STATUS "external: llama.cpp Vulkan backend OFF - install the Vulkan SDK (VULKAN_SDK) to enable it")
    endif()
    if (SONNY_LLAMA_CUDA_BACKEND)
        message(STATUS "external: llama.cpp CUDA backend ON (nvcc: ${SONNY_NVCC}, arch: ${SONNY_LLAMA_CUDA_ARCH})")
        # nvcc < 12.9 hard-rejects MSVC 14.4x (VS 17.14) as host compiler;
        # -allow-unsupported-compiler lets it try anyway. If it still crashes
        # (known cudafe++ crash with CUDA 12.6 + MSVC 17.14) the build is
        # retried without CUDA below.
        list(APPEND LLAMA_CMAKE_ARGS
            -DGGML_CUDA=ON
            -DCMAKE_CUDA_ARCHITECTURES=${SONNY_LLAMA_CUDA_ARCH}
            -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler)
    else()
        message(STATUS "external: llama.cpp CUDA backend OFF - install the CUDA toolkit (CUDA_PATH) to enable it")
    endif()
    message(STATUS "external: llama.cpp backends: ${SONNY_LLAMA_BACKENDS}")

    # Build directly in external/llama.cpp/build so the prebuilt check works on
    # the next run. First attempt includes every requested GPU backend; when
    # CUDA breaks the build and SONNY_LLAMA_CUDA=AUTO, wipe the build tree and
    # retry without CUDA (Vulkan/CPU still work). An explicit
    # SONNY_LLAMA_CUDA=ON fails loudly instead of silently dropping CUDA.
    sonny_build_from_source_try(SONNY_LLAMA_BUILD_RESULT
        "${SONNY_LLAMA_PREBUILT_LIB}"
        "${LLAMA_SOURCE_DIR}" "${LLAMA_BUILD_DIR}"
        ${LLAMA_CMAKE_ARGS})
    if (NOT SONNY_LLAMA_BUILD_RESULT EQUAL 0 AND SONNY_LLAMA_CUDA_BACKEND
            AND "${SONNY_LLAMA_CUDA}" STREQUAL "AUTO")
        message(WARNING "external: llama.cpp build with CUDA failed (exit ${SONNY_LLAMA_BUILD_RESULT}) - retrying without the CUDA backend (Vulkan/CPU only). Upgrade the CUDA toolkit to 12.9+ to build the CUDA backend against this MSVC.")
        file(REMOVE_RECURSE "${LLAMA_BUILD_DIR}")
        list(FILTER LLAMA_CMAKE_ARGS EXCLUDE REGEX "GGML_CUDA=|CUDA_ARCHITECTURES|CMAKE_CUDA_FLAGS")
        set(SONNY_LLAMA_CUDA_BACKEND OFF)
        list(REMOVE_ITEM SONNY_LLAMA_BACKENDS "CUDA")
        message(STATUS "external: llama.cpp backends: ${SONNY_LLAMA_BACKENDS}")
        sonny_build_from_source_try(SONNY_LLAMA_BUILD_RESULT
            "${SONNY_LLAMA_PREBUILT_LIB}"
            "${LLAMA_SOURCE_DIR}" "${LLAMA_BUILD_DIR}"
            ${LLAMA_CMAKE_ARGS})
    endif()
    if (NOT SONNY_LLAMA_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "llama.cpp build failed (exit code ${SONNY_LLAMA_BUILD_RESULT})")
    endif()
endif()

# When llama.lib is present but a GPU backend archive the llama.cpp cache
# enabled is still missing (e.g. the first build was interrupted), re-run the
# build step - MSBuild only compiles the remaining objects and links the
# missing archives.
# ggml-cuda / ggml-vulkan are separate static archives. Depending on the
# llama.cpp version they land either next to ggml.lib or in their own
# sub-project output directories - check both locations.
set(SONNY_LLAMA_GGML_CUDA_LIB "")
foreach(_sonny_cuda_candidate
        "${LLAMA_BUILD_DIR}/ggml/src/ggml-cuda/Release/ggml-cuda.lib"
        "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml-cuda.lib")
    if (EXISTS "${_sonny_cuda_candidate}")
        set(SONNY_LLAMA_GGML_CUDA_LIB "${_sonny_cuda_candidate}")
        break()
    endif()
endforeach()
set(SONNY_LLAMA_GGML_VULKAN_LIB "")
foreach(_sonny_vulkan_candidate
        "${LLAMA_BUILD_DIR}/ggml/src/ggml-vulkan/Release/ggml-vulkan.lib"
        "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml-vulkan.lib")
    if (EXISTS "${_sonny_vulkan_candidate}")
        set(SONNY_LLAMA_GGML_VULKAN_LIB "${_sonny_vulkan_candidate}")
        break()
    endif()
endforeach()
if (EXISTS "${SONNY_LLAMA_PREBUILT_LIB}" AND EXISTS "${LLAMA_BUILD_DIR}/CMakeCache.txt")
    file(STRINGS "${LLAMA_BUILD_DIR}/CMakeCache.txt" _sonny_cache_cuda REGEX "^GGML_CUDA:BOOL=ON$")
    file(STRINGS "${LLAMA_BUILD_DIR}/CMakeCache.txt" _sonny_cache_vulkan REGEX "^GGML_VULKAN:BOOL=ON$")
    set(_sonny_topup "")
    if (_sonny_cache_cuda AND NOT SONNY_LLAMA_GGML_CUDA_LIB)
        list(APPEND _sonny_topup "CUDA")
    endif()
    if (_sonny_cache_vulkan AND NOT SONNY_LLAMA_GGML_VULKAN_LIB)
        list(APPEND _sonny_topup "Vulkan")
    endif()
    if (_sonny_topup)
        message(STATUS "external: llama.cpp GPU backend archives missing (${_sonny_topup}) - resuming the llama.cpp build")
        if (NOT CMAKE_BUILD_PARALLEL_LEVEL)
            cmake_host_system_information(RESULT _topup_jobs QUERY NUMBER_OF_LOGICAL_CORES)
        else()
            set(_topup_jobs "${CMAKE_BUILD_PARALLEL_LEVEL}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${LLAMA_BUILD_DIR}" --config Release
            --parallel "${_topup_jobs}"
            RESULT_VARIABLE _topup_result)
        if (NOT _topup_result EQUAL 0)
            message(FATAL_ERROR "llama.cpp incremental build failed (exit code ${_topup_result})")
        endif()
        # Re-scan for the freshly linked archives in both output locations.
        set(SONNY_LLAMA_GGML_CUDA_LIB "")
        foreach(_sonny_cuda_candidate
                "${LLAMA_BUILD_DIR}/ggml/src/ggml-cuda/Release/ggml-cuda.lib"
                "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml-cuda.lib")
            if (EXISTS "${_sonny_cuda_candidate}")
                set(SONNY_LLAMA_GGML_CUDA_LIB "${_sonny_cuda_candidate}")
                break()
            endif()
        endforeach()
        set(SONNY_LLAMA_GGML_VULKAN_LIB "")
        foreach(_sonny_vulkan_candidate
                "${LLAMA_BUILD_DIR}/ggml/src/ggml-vulkan/Release/ggml-vulkan.lib"
                "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml-vulkan.lib")
            if (EXISTS "${_sonny_vulkan_candidate}")
                set(SONNY_LLAMA_GGML_VULKAN_LIB "${_sonny_vulkan_candidate}")
                break()
            endif()
        endforeach()
        if (_sonny_cache_cuda AND NOT SONNY_LLAMA_GGML_CUDA_LIB)
            message(FATAL_ERROR "llama.cpp build finished but ggml-cuda.lib is still missing")
        endif()
        if (_sonny_cache_vulkan AND NOT SONNY_LLAMA_GGML_VULKAN_LIB)
            message(FATAL_ERROR "llama.cpp build finished but ggml-vulkan.lib is still missing")
        endif()
    endif()
endif()

if (EXISTS "${SONNY_LLAMA_PREBUILT_LIB}")
    target_link_libraries(personal_assistant "${SONNY_LLAMA_PREBUILT_LIB}")
    if (EXISTS "${SONNY_LLAMA_PREBUILT_COMMON}")
        target_link_libraries(personal_assistant "${SONNY_LLAMA_PREBUILT_COMMON}")
    endif()
    # Also link ggml libraries which contain symbols referenced by llama.lib
    target_link_libraries(personal_assistant
        "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml.lib"
        "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml-base.lib"
        "${LLAMA_BUILD_DIR}/ggml/src/Release/ggml-cpu.lib")
    # mtmd (multimodal) library for vision / audio multimodal support.
    # Built by llama.cpp when LLAMA_BUILD_MTMD=ON (see the CMake flag above).
    set(SONNY_MTMD_LIB "${LLAMA_BUILD_DIR}/tools/mtmd/Release/mtmd.lib")
    if (EXISTS "${SONNY_MTMD_LIB}")
        target_link_libraries(personal_assistant "${SONNY_MTMD_LIB}")
        # mtmd-helper depends on llama.cpp's vendor-hash library for hash_sha256_hex().
        set(SONNY_VENDOR_HASH_LIB "${LLAMA_BUILD_DIR}/vendor/hash/Release/vendor-hash.lib")
        if (EXISTS "${SONNY_VENDOR_HASH_LIB}")
            target_link_libraries(personal_assistant "${SONNY_VENDOR_HASH_LIB}")
        endif()
    endif()
    # The GPU backend archives define ggml_backend_cuda_reg / ggml_backend_vk_reg,
    # which ggml-backend-reg.obj references whenever the llama.cpp build had the
    # corresponding backend enabled. Both are static archives built out of tree,
    # so their link-time dependencies have to be added by path as well:
    # ggml-cuda is compiled with the static CUDA runtime (CMAKE defaults to
    # -cudart static) and calls cuBLAS plus the CUDA driver API directly
    # (GGML_CUDA_NO_VMM=OFF); ggml-vulkan links the Vulkan loader.
    if (SONNY_LLAMA_GGML_CUDA_LIB)
        target_link_libraries(personal_assistant "${SONNY_LLAMA_GGML_CUDA_LIB}")
        foreach(sonny_cuda_import cudart_static.lib cublas.lib cublasLt.lib cuda.lib)
            if (EXISTS "${SONNY_CUDA_LIB_DIR}/${sonny_cuda_import}")
                target_link_libraries(personal_assistant "${SONNY_CUDA_LIB_DIR}/${sonny_cuda_import}")
            else()
                message(FATAL_ERROR "llama.cpp CUDA backend needs ${sonny_cuda_import} from ${SONNY_CUDA_LIB_DIR} - install the CUDA toolkit or set CUDA_PATH")
            endif()
        endforeach()
    endif()
    if (SONNY_LLAMA_GGML_VULKAN_LIB)
        target_link_libraries(personal_assistant "${SONNY_LLAMA_GGML_VULKAN_LIB}")
        if (EXISTS "${SONNY_VULKAN_LIB_DIR}/vulkan-1.lib")
            target_link_libraries(personal_assistant "${SONNY_VULKAN_LIB_DIR}/vulkan-1.lib")
        else()
            message(FATAL_ERROR "llama.cpp Vulkan backend needs vulkan-1.lib from ${SONNY_VULKAN_LIB_DIR} - install the Vulkan SDK or set VULKAN_SDK")
        endif()
    endif()
    # Runtime DLLs for the GPU backends, copied next to the exe so Sonny runs
    # standalone on a clean machine:
    #   - cuBLAS ships as import libraries + DLLs even when the CUDA runtime
    #     itself is linked statically.
    foreach(sonny_gpu_dll "${SONNY_CUDA_TOOLKIT_BIN}/cublas64_12.dll"
                          "${SONNY_CUDA_TOOLKIT_BIN}/cublasLt64_12.dll")
        if (EXISTS "${sonny_gpu_dll}")
            add_custom_command(TARGET personal_assistant POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${sonny_gpu_dll}"
                    $<TARGET_FILE_DIR:personal_assistant>)
        endif()
    endforeach()
    #   - Vulkan loader (vulkan-1.dll): deliberately NOT bundled. The whole
    #     ggml-vulkan backend is linked statically into the exe, and the loader
    #     is part of the Vulkan runtime that the graphics driver installs into
    #     C:\Windows\System32 (Intel, AMD and NVIDIA drivers all provide it), so
    #     every machine with a GPU driver already has it. An app-local copy
    #     would shadow that newer system loader - a well known source of
    #     Vulkan/driver breakage - and is explicitly discouraged by Khronos, so
    #     Sonny uses the system loader like any other Windows Vulkan app.
    if (NOT EXISTS "C:/Windows/System32/vulkan-1.dll")
        message(WARNING "C:/Windows/System32/vulkan-1.dll is missing - install a graphics driver that ships the Vulkan runtime, otherwise personal_assistant.exe cannot start because it imports the Vulkan loader")
    endif()
endif()

# ---- Kokoro TTS (in-process) ------------------------------------------------
# Replaces the kokoro-server.exe subprocess + HTTP hop: the kokoro static
# library is linked straight into the exe (see src/KokoroWrapper.cpp).
#
# The static libraries use the static MSVC runtime (/MT) to match
# personal_assistant (the kokoro-server CMake defaults to /MD -> LNK2038).
# When external/kokoro-server/mt is empty they are compiled here from the
# kokoro-server sources in external/ - no helper script required.
sonny_require_dependency("kokoro-server sources" "${KOKORO_BASE_DIR}/src/kokoro.cpp")
sonny_require_dependency("eSpeak NG dev package" "${ESPEAK_DIR}/lib/espeak-ng.lib")
sonny_require_dependency("ONNX Runtime" "${ONNXRUNTIME_LIB}")
sonny_require_dependency("OpenBLAS" "${OPENBLAS_DIR}/lib/openblas.lib")
sonny_require_dependency("CTranslate2 (prebuilt build-cpu)" "${CT2_LIB}")

target_include_directories(personal_assistant PRIVATE
    "${CT2_INCLUDE_DIR}"
    "${KOKORO_BASE_DIR}/src"
    # Generated at configure time: the list of GGUF architectures the linked
    # llama.cpp implements (see sonny_llama_archs.h above).
    "${CMAKE_BINARY_DIR}/generated")

# Mark external (third-party) include directories as SYSTEM so MSVC suppresses
# warnings from their headers.  This avoids spurious noise and reduces the work
# the compiler does when analysing those headers during incremental builds.
target_include_directories(personal_assistant SYSTEM PRIVATE
    "${LLAMA_INCLUDE_DIR}"
    "${LLAMA_SOURCE_DIR}/ggml/include"
    "${LLAMA_SOURCE_DIR}/tools/mtmd"
    "${ONNXRUNTIME_INCLUDE_DIR}"
    "${CT2_INCLUDE_DIR}"
    "${WEBVIEW2_INCLUDE_DIR}")

if (NOT EXISTS "${KOKORO_MT_DIR}/kokoro-mt.lib")
    message(STATUS "external: Kokoro /MT libraries missing - building them from source")
    set(KOKORO_DEPS_DIR "${KOKORO_BASE_DIR}/build/_deps")
    set(MISAKI_DIR "${KOKORO_BASE_DIR}/misaki-cpp")

    add_library(fmt-mt STATIC
        "${KOKORO_DEPS_DIR}/fmt-src/src/format.cc"
        "${KOKORO_DEPS_DIR}/fmt-src/src/os.cc")
    target_include_directories(fmt-mt PUBLIC "${KOKORO_DEPS_DIR}/fmt-src/include")

    add_library(spdlog-mt STATIC
        "${KOKORO_DEPS_DIR}/spdlog-src/src/spdlog.cpp"
        "${KOKORO_DEPS_DIR}/spdlog-src/src/stdout_sinks.cpp"
        "${KOKORO_DEPS_DIR}/spdlog-src/src/color_sinks.cpp"
        "${KOKORO_DEPS_DIR}/spdlog-src/src/file_sinks.cpp"
        "${KOKORO_DEPS_DIR}/spdlog-src/src/async.cpp"
        "${KOKORO_DEPS_DIR}/spdlog-src/src/cfg.cpp"
        "${KOKORO_DEPS_DIR}/spdlog-src/src/bundled_fmtlib_format.cpp")
    target_compile_definitions(spdlog-mt PUBLIC SPDLOG_COMPILED_LIB)
    target_include_directories(spdlog-mt PUBLIC "${KOKORO_DEPS_DIR}/spdlog-src/include")

    add_library(misaki-mt STATIC
        "${MISAKI_DIR}/src/g2p.cpp"
        "${MISAKI_DIR}/src/lexicon.cpp"
        "${MISAKI_DIR}/src/tagger.cpp"
        "${MISAKI_DIR}/src/fallback.cpp"
        "${MISAKI_DIR}/src/num2words_en.cpp")
    target_include_directories(misaki-mt PUBLIC "${MISAKI_DIR}/include" "${ESPEAK_DIR}/include")

    add_library(kokoro-mt STATIC
        "${KOKORO_BASE_DIR}/src/istft.cpp"
        "${KOKORO_BASE_DIR}/src/phonemizer.cpp"
        "${KOKORO_BASE_DIR}/src/g2p.cpp"
        "${KOKORO_BASE_DIR}/src/kokoro.cpp"
        "${KOKORO_BASE_DIR}/src/onnx-decoder.cpp")
    set_target_properties(kokoro-mt PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_include_directories(kokoro-mt PUBLIC
        "${KOKORO_BASE_DIR}/src"
        "${MISAKI_DIR}/include"
        "${ESPEAK_DIR}/include"
        "${ONNXRUNTIME_INCLUDE_DIR}"
        "${OPENBLAS_DIR}/include"
        "${KOKORO_DEPS_DIR}/spdlog-src/include"
        "${KOKORO_DEPS_DIR}/fmt-src/include"
        "${KOKORO_DEPS_DIR}/nlohmann_json-src/include")
    # kokoro.cpp only pulls in the RKNN decoder when USE_RKNN is defined.
    target_link_libraries(kokoro-mt PUBLIC misaki-mt fmt-mt spdlog-mt)
endif()

if (TARGET kokoro-mt)
    target_link_libraries(personal_assistant kokoro-mt)
else()
    target_link_libraries(personal_assistant
        "${KOKORO_MT_DIR}/kokoro-mt.lib"
        "${KOKORO_MT_DIR}/misaki-mt.lib"
        "${KOKORO_MT_DIR}/fmt-mt.lib"
        "${KOKORO_MT_DIR}/spdlog-mt.lib")
endif()

target_link_libraries(personal_assistant "${ESPEAK_DIR}/lib/espeak-ng.lib")
target_link_libraries(personal_assistant "${OPENBLAS_DIR}/lib/openblas.lib")

# Kokoro runtime DLLs (eSpeak NG phonemizer + OpenBLAS for the iSTFT sgemm),
# copied next to the exe so the application directory stays self-contained.
foreach(kokoro_dll "${KOKORO_BASE_DIR}/build/Release/libespeak-ng.dll"
                   "${ESPEAK_DIR}/bin/libespeak-ng.dll"
                   "${OPENBLAS_DIR}/bin/openblas.dll")
    if(EXISTS "${kokoro_dll}")
        add_custom_command(TARGET personal_assistant POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${kokoro_dll}"
                $<TARGET_FILE_DIR:personal_assistant>
        )
    endif()
endforeach()

# Assemble the Kokoro TTS runtime asset directory (kokoro/) next to the exe.
# KokoroWrapper::initialize() expects config.json, onnx/{encoder,har,decoder}.onnx,
# voices_npy/, espeak-ng-data/ and misaki-data/ under the model root.
# Sources live under external/kokoro-server and external/espeak-ng-dev; the
# 3.2 GB Kokoro-82M/kokoro-v1_0.pth PyTorch checkpoint and .pt voice files are
# intentionally excluded (ONNX-inference only).
# These steps carry no COMMENT on purpose: see "Post-build staging report" below.
set(KOKORO_RUNTIME_DIR "$<TARGET_FILE_DIR:personal_assistant>/kokoro")
add_custom_command(TARGET personal_assistant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${KOKORO_RUNTIME_DIR}")
if(EXISTS "${KOKORO_BASE_DIR}/Kokoro-82M/config.json")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${KOKORO_RUNTIME_DIR}/Kokoro-82M"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${KOKORO_BASE_DIR}/Kokoro-82M/config.json"
            "${KOKORO_RUNTIME_DIR}/Kokoro-82M/config.json")
endif()
if(EXISTS "${KOKORO_BASE_DIR}/onnx")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${KOKORO_BASE_DIR}/onnx"
            "${KOKORO_RUNTIME_DIR}/onnx")
endif()
if(EXISTS "${KOKORO_BASE_DIR}/voices_npy")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${KOKORO_BASE_DIR}/voices_npy"
            "${KOKORO_RUNTIME_DIR}/voices_npy")
endif()
if(EXISTS "${ESPEAK_DIR}/share/espeak-ng-data")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ESPEAK_DIR}/share/espeak-ng-data"
            "${KOKORO_RUNTIME_DIR}/espeak-ng-data")
endif()
if(EXISTS "${KOKORO_BASE_DIR}/misaki-cpp/data")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${KOKORO_BASE_DIR}/misaki-cpp/data"
            "${KOKORO_RUNTIME_DIR}/misaki-data")
endif()

set(LLAMA_DLLS
    "${LLAMA_BIN_DIR}/llama.dll"
    "${LLAMA_BIN_DIR}/llama-common.dll"
    "${LLAMA_BIN_DIR}/ggml.dll"
    "${LLAMA_BIN_DIR}/ggml-base.dll"
    "${LLAMA_BIN_DIR}/ggml-cpu.dll"
    "${LLAMA_BIN_DIR}/ggml-vulkan.dll"
    "${LLAMA_BIN_DIR}/ggml-cuda.dll"
    "${LLAMA_BIN_DIR}/mtmd.dll"
)
foreach(dll ${LLAMA_DLLS})
    if(EXISTS "${dll}")
        add_custom_command(TARGET personal_assistant POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll}"
                $<TARGET_FILE_DIR:personal_assistant>
        )
    endif()
endforeach()

# When llama.cpp was built from source above the DLLs are taken from the
# targets instead of the prebuilt tree.
foreach(llama_target llama llama-common ggml ggml-base ggml-cpu ggml-vulkan ggml-cuda mtmd)
    if (TARGET ${llama_target})
        get_target_property(sonny_llama_target_type ${llama_target} TYPE)
        if (sonny_llama_target_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET personal_assistant POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${llama_target}>
                    $<TARGET_FILE_DIR:personal_assistant>)
        endif()
    endif()
endforeach()

# CUDA runtime DLLs required by ggml-cuda.dll. They are read from
# external/cuda/bin so the build never depends on a system-wide CUDA install.
foreach(cuda_dll_name cublas64_12.dll cublasLt64_12.dll cudart64_12.dll)
    if(EXISTS "${SONNY_CUDA_BIN_DIR}/${cuda_dll_name}")
        add_custom_command(TARGET personal_assistant POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SONNY_CUDA_BIN_DIR}/${cuda_dll_name}"
                $<TARGET_FILE_DIR:personal_assistant>)
    endif()
endforeach()

if(EXISTS "${CMAKE_SOURCE_DIR}/resources/avatars/tiny_gltf.h")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/avatars"
            $<TARGET_FILE_DIR:personal_assistant>/avatars
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/resources/images/logo.ico")
    add_custom_command(TARGET personal_assistant PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/images/logo.ico"
            "${CMAKE_BINARY_DIR}/logo.ico"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/resources/web/web_speech.html")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/web"
            $<TARGET_FILE_DIR:personal_assistant>/web
    )
endif()

# Copy other resource directories to output
if(EXISTS "${CMAKE_SOURCE_DIR}/resources/sounds")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/sounds"
            $<TARGET_FILE_DIR:personal_assistant>/sounds
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/resources/videos")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/videos"
            $<TARGET_FILE_DIR:personal_assistant>/videos
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/resources/models")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/models"
            $<TARGET_FILE_DIR:personal_assistant>/models
    )
endif()

# Copy kokoro directory from external or build directory
if(EXISTS "${KOKORO_BASE_DIR}/mt")
    add_custom_command(TARGET personal_assistant POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${KOKORO_BASE_DIR}/mt"
            $<TARGET_FILE_DIR:personal_assistant>/kokoro
    )
endif()

# Remove build-time tool that shouldn't be in the output directory
add_custom_command(TARGET personal_assistant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove
        $<TARGET_FILE_DIR:personal_assistant>/vulkan-shaders-gen.exe
)

# Remove MSVC linker byproducts (.lib import library + .exp export file).
# They are only useful when other binaries link *against* the exe, which
# nothing in this project does. MSVC regenerates them on every link, so this
# only keeps build/Release/ tidy - the exe is self-contained without them.
add_custom_command(TARGET personal_assistant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove
        $<TARGET_FILE_DIR:personal_assistant>/personal_assistant.lib
        $<TARGET_FILE_DIR:personal_assistant>/personal_assistant.exp
)

# ---------------------------------------------------------------------------
# Post-build staging report.
# CMake folds the COMMENT of every POST_BUILD command into ONE tab-separated
# <Message> element of the generated PostBuildEvent, which MSBuild prints as a
# single long, randomly indented line that wraps across the console. That is
# why the staging commands above carry no COMMENT: they stay silent and report
# themselves here instead, one aligned line per group, like the rest of the
# build log.
# ---------------------------------------------------------------------------
add_custom_command(TARGET personal_assistant POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Staged runtime DLLs (CUDA, STT, TTS) next to personal_assistant.exe"
    COMMAND ${CMAKE_COMMAND} -E echo "Staged Kokoro TTS assets and resource directories (avatars, web, sounds, videos, models)"
    COMMAND ${CMAKE_COMMAND} -E echo "Removed build-only artifacts (vulkan-shaders-gen.exe, personal_assistant.lib/.exp)")

if (MSVC)
    target_link_libraries(personal_assistant "winhttp.lib")
    target_link_libraries(personal_assistant "Advapi32.lib")
    target_link_libraries(personal_assistant "Ws2_32.lib")
    target_link_libraries(personal_assistant "Shlwapi.lib")
    target_link_libraries(personal_assistant "userenv.lib")
    target_link_libraries(personal_assistant "Winmm.lib")
    target_link_libraries(personal_assistant "d3d11.lib")
    target_link_libraries(personal_assistant "dxgi.lib")
    target_link_libraries(personal_assistant "d3dcompiler.lib")
    target_link_libraries(personal_assistant "dwmapi.lib")
    target_link_libraries(personal_assistant "shell32.lib")
    target_link_libraries(personal_assistant "comctl32.lib")
    target_link_libraries(personal_assistant "mfplat.lib")
    target_link_libraries(personal_assistant "mf.lib")
    target_link_libraries(personal_assistant "mfreadwrite.lib")
    target_link_libraries(personal_assistant "mfuuid.lib")
    target_link_libraries(personal_assistant "wininet.lib")
    target_link_libraries(personal_assistant "winmm.lib")
    target_link_libraries(personal_assistant "dsound.lib")
    target_link_libraries(personal_assistant "ole32.lib")
    target_link_libraries(personal_assistant "uuid.lib")
    target_link_libraries(personal_assistant "Psapi.lib")
    target_link_libraries(personal_assistant "powrprof.lib")
    # WebView2 runtime - loaded dynamically at runtime via LoadLibrary,
    # so no link dependency is required. The dialog falls back to a
    # static message if the runtime is missing.
    target_link_libraries(personal_assistant "${WEBVIEW2_DLL_LIB}")
    if(EXISTS "${WEBVIEW2_BASE_DIR}/build/native/x64/WebView2Loader.dll")
        add_custom_command(TARGET personal_assistant POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_BASE_DIR}/build/native/x64/WebView2Loader.dll"
                $<TARGET_FILE_DIR:personal_assistant>)
    endif()
    target_link_libraries(personal_assistant "user32.lib")
    target_link_libraries(personal_assistant "gdi32.lib")
endif()

# ---------------------------------------------------------------------------
# Optional: the vectors rendezvous relay (tools/relay/SonnyRelay.cpp).
#
# Only needed for "wan" mode, and only because some networks (CGNAT, corporate
# Wi-Fi) make direct peer-to-peer impossible. It is a plain C++ Winsock server
# with no third-party dependencies, so it builds with the same toolchain and can
# be hosted on any always-on Windows machine you already own - or reached over an
# overlay VPN, in which case you do not need it at all. No paid service involved.
# Disable with -DSONNY_BUILD_RELAY=OFF.
# ---------------------------------------------------------------------------
option(SONNY_BUILD_RELAY "Build the vectors rendezvous relay" ON)

if(SONNY_BUILD_RELAY)
    add_executable(sonny_relay tools/relay/SonnyRelay.cpp)
    target_link_libraries(sonny_relay PRIVATE
        $<$<PLATFORM_ID:Windows>:Ws2_32.lib>)
    if(MSVC)
        set_target_properties(sonny_relay PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
    message(STATUS "relay: sonny_relay target enabled (host it yourself for WAN mode)")
endif()
