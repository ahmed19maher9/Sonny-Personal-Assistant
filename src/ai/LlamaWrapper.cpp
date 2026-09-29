#include "LlamaWrapper.h"
#include "Logger.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "sonny_llama_archs.h"
#ifdef SONNY_HAS_MTMD
#include "mtmd.h"
#include "mtmd-helper.h"
#endif
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <chrono>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>
#pragma comment(lib, "dxgi.lib")
#endif

static bool g_backend_initialized = false;
static std::mutex g_backend_mutex;

// Redirect llama.cpp / ggml log output into Sonny's Logger instead of stderr,
// so model-loading chatter ("create_tensor: loading tensor ...", "load_tensors:",
// KV cache stats, etc.) does not leak to the console when running attached to a
// terminal. Called once per process before llama_backend_init().
static void llama_log_callback(enum ggml_log_level level, const char * text, void * /*user_data*/) {
    if (!text) return;
    const std::string msg(text);
    // GGML_LOG_LEVEL_CONT is "continue previous log line" - treat it as INFO so
    // multi-line messages stay attached under the same heading.
    switch (level) {
        case GGML_LOG_LEVEL_DEBUG:  LOG_DEBUG("LLAMA-NATIVE", msg); break;
        case GGML_LOG_LEVEL_INFO:
        case GGML_LOG_LEVEL_CONT:
            LOG_LLAMANATIVE(msg); break;
        case GGML_LOG_LEVEL_WARN:   LOG_WARN("LLAMA-NATIVE", msg); break;
        case GGML_LOG_LEVEL_ERROR:  LOG_ERROR("LLAMA-NATIVE", msg); break;
        default:                    LOG_LLAMANATIVE(msg); break;
    }
}

// Ask the ggml backend registry which accelerator is actually registered.
// Unlike checking for loaded ggml-*.dll modules (which breaks with statically
// linked backends), this works for both static and dynamic backend builds.
static bool ggml_has_gpu_backend() {
    return ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU) != nullptr;
}

static const char* ggml_gpu_backend_name() {
    ggml_backend_dev_t dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
    if (!dev) return "CPU";
    const char* name = ggml_backend_dev_name(dev);
    if (!name) return "GPU";
    return name;  // "CUDA", "Vulkan", ...
}

#ifdef _WIN32
// PCI vendor IDs of the GPU adapter vendors Sonny can steer llama.cpp towards.
static constexpr int GPU_VENDOR_NVIDIA = 0x10DE;  // CUDA backend
static constexpr int GPU_VENDOR_AMD    = 0x1002;  // Vulkan backend
static constexpr int GPU_VENDOR_INTEL  = 0x8086;  // Vulkan backend

// Vendor ID of the primary display adapter (the one with the most dedicated
// video memory, ignoring software adapters), or 0 when none is found.
static int query_primary_gpu_vendor_id() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory) {
        return 0;
    }
    IDXGIAdapter1* best = nullptr;
    DXGI_ADAPTER_DESC1 best_desc = {};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (adapter) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (!best || desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory)) {
                if (best) best->Release();
                best = adapter;  // take ownership
                best_desc = desc;
            } else {
                adapter->Release();
            }
        }
    }
    const int vendor_id = best ? static_cast<int>(best_desc.VendorId) : 0;
    if (best) best->Release();
    factory->Release();
    return vendor_id;
}
#endif

// Expose an environment variable to both the Win32 environment and the CRT
// environment, so every reader in this process sees the value (the ggml backends
// read via getenv, the CUDA runtime via the Win32 environment). Note: an empty
// value would *delete* the variable (SetEnvironmentVariable semantics) rather than
// blank it, so only non-empty values are ever passed here.
static void set_process_env(const char* name, const char* value) {
    SetEnvironmentVariableA(name, value);
    std::string kv = std::string(name) + "=" + value;
    _putenv(kv.c_str());
}

// GPU VRAM telemetry. When the GPU memory budget is exceeded, Windows (WDDM)
// silently pages GPU allocations to system RAM over PCIe - CUDA inference then
// slows down by 10-50x (weights re-streamed over the PCIe bus every token).
// These helpers make that condition visible in the log.
struct GpuVramInfo {
    bool available = false;
    std::string name;
    long long dedicated_bytes = 0;  // physical VRAM on the adapter
    long long budget_bytes = 0;     // memory the OS grants this process
    long long used_bytes = 0;       // this process's current VRAM usage
};

#ifdef _WIN32
static GpuVramInfo query_gpu_vram() {
    GpuVramInfo info;
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory) {
        return info;
    }
    IDXGIAdapter1* best = nullptr;
    DXGI_ADAPTER_DESC1 best_desc = {};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (adapter) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (!best || desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory)) {
                if (best) best->Release();
                best = adapter;  // take ownership
                best_desc = desc;
            } else {
                adapter->Release();
            }
        }
    }
    if (best) {
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(best->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO mem = {};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)) && mem.Budget > 0) {
                info.available = true;
                info.budget_bytes = (long long)mem.Budget;
                info.used_bytes = (long long)mem.CurrentUsage;
                info.dedicated_bytes = (long long)best_desc.DedicatedVideoMemory;
                std::wstring wname(best_desc.Description);
                info.name.assign(wname.begin(), wname.end());
            }
            adapter3->Release();
        }
        best->Release();
    }
    factory->Release();
    return info;
}

static std::string format_gpu_vram(const GpuVramInfo& v) {
    auto mb = [](long long b) { return (long long)(b / (1024 * 1024)); };
    std::ostringstream oss;
    oss << v.name << " | VRAM " << mb(v.used_bytes) << "/" << mb(v.budget_bytes)
        << " MiB used (dedicated " << mb(v.dedicated_bytes) << " MiB)";
    return oss.str();
}

// Spot-check the GPU's actual power/clock/thermal state (nvidia-smi). This
// distinguishes thermal/power throttling (Max-Q laptops: clocks 300-600 MHz,
// temp 85C+, power capped) from VRAM demotion or CPU starvation when
// inference collapses far below its expected speed.
static std::string query_nvidia_smi() {
    FILE* pipe = _popen("nvidia-smi --query-gpu=temperature.gpu,clocks.gr,clocks.mem,power.draw,utilization.gpu,pstate --format=csv,noheader", "rt");
    if (!pipe) return "";
    char buf[512] = {};
    std::string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    _pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}
#endif

// ---------------------------------------------------------------------------
// GGUF pre-flight: model architecture and file kind
// ---------------------------------------------------------------------------

// Architectures that people ask about often enough to deserve a written answer
// instead of "it did not load". A hint is only attached to the failure message
// when arch_is_supported() says the linked llama.cpp really does not implement
// the architecture, so an older pinned revision keeps working unchanged.
struct RemovedArchHint {
    const char* arch;
    const char* family;  // which models carry this architecture
    const char* advice;  // what to use instead
};

static const RemovedArchHint kRemovedArchHints[] = {
    { "mllama",
      "Mllama - Llama 3.2 Vision 11B/90B (Hugging Face Llama-3.2-*-Vision-Instruct "
      "GGUFs, Ollama's llama3.2-vision and the ~1.9 GB projector blob pulled next to it).",
      "Upstream llama.cpp removed the Mllama implementation, so these files cannot be "
      "loaded by Sonny any more. Use a vision model this build implements together with "
      "its matching mmproj-*.gguf projector (Qwen2.5-VL, Qwen3-VL, Gemma 3/4, Llama 4, "
      "Mistral Small 3.1, InternVL, Hunyuan-VL), or a plain text model such as "
      "Llama 3.2 3B, Gemma 3 or Qwen2.5." },
};

static const RemovedArchHint* find_removed_arch_hint(const std::string& arch) {
    for (const RemovedArchHint& hint : kRemovedArchHints) {
        if (arch == hint.arch) return &hint;
    }
    return nullptr;
}

// Whether the linked llama.cpp implements `general.architecture == arch`. The
// list is generated by CMake from the pinned src/llama-arch.cpp; when that
// extraction failed the array holds only its terminator and no judgement is
// made, so the pre-flight never blocks a load it cannot be sure about.
static bool arch_is_supported(const std::string& arch) {
    if (SONNY_LLAMA_SUPPORTED_ARCHS[0] == nullptr) return true;
    for (const char* const* it = SONNY_LLAMA_SUPPORTED_ARCHS; *it != nullptr; ++it) {
        if (arch == *it) return true;
    }
    return false;
}

// The supported architectures as a wrapped, log-friendly block.
static std::string supported_architectures_text() {
    size_t count = 0;
    std::string out;
    size_t column = 0;  // length of the current line
    for (const char* const* it = SONNY_LLAMA_SUPPORTED_ARCHS; *it != nullptr; ++it) {
        const std::string entry = *it;
        if (column == 0) {
            out += "  ";
            column = 2;
        } else if (column + entry.size() + 2 > 92) {
            out += ",\n  ";
            column = 2;
        } else {
            out += ", ";
            column += 2;
        }
        out += entry;
        column += entry.size();
        ++count;
    }
    if (count == 0) {
        return "  (list unavailable: it is generated from the llama.cpp sources at configure time)";
    }
    return out + "\n  (" + std::to_string(count) + " architectures)";
}

// Turns the metadata read from a GGUF file into the user-facing explanation in
// info.problem, and marks the file as unloadable when llama.cpp is certain to
// reject it (see inspect_model_file()).
static void describe_model_file_problem(const std::string& model_path, const std::string& general_type,
                                       LlamaWrapper::ModelFileInfo& info) {
    const std::string& arch = info.architecture;

    if (general_type == "projector" || arch == "clip") {
        // Vision/audio encoders (mmproj) identify themselves with general.type
        // ("projector", as in the ggml-org and Ollama projector GGUFs) or with the
        // "clip" architecture; neither describes a language model.
        info.is_projector = true;
        info.problem = "Model file is a multimodal projector (mmproj), not a language model:\n  " + model_path +
            "\n  Point Sonny's model setting at the matching language model GGUF and keep this file\n"
            "  next to it, named mmproj-<model>.gguf - Sonny then detects it on its own, enables\n"
            "  vision and routes camera / screenshot images through it.";
    } else if (arch.empty()) {
        info.problem =
            "Model file carries no 'general.architecture' metadata, so llama.cpp cannot identify it:\n  " + model_path;
    } else if (!arch_is_supported(arch)) {
        info.problem = "Model architecture '" + arch + "' is not implemented by the llama.cpp build Sonny links against.";
        info.problem += "\n  Model file: " + model_path;
        if (!info.name.empty()) info.problem += "\n  Model name: " + info.name;
        if (const RemovedArchHint* hint = find_removed_arch_hint(arch)) {
            info.problem += "\n  " + std::string(hint->family);
            info.problem += "\n  " + std::string(hint->advice);
        } else {
            info.problem += "\n  Choose a model whose architecture appears in the list below; a vision\n"
                            "  model additionally needs its mmproj-*.gguf projector beside it.";
        }
        info.problem += "\n  Architectures this build loads:\n" + supported_architectures_text();
    }

    // Everything reported above makes llama_model_load_from_file() fail for sure,
    // so the caller can stop before any GPU memory is touched.
    info.unusable = !info.problem.empty();
}

LlamaWrapper::ModelFileInfo LlamaWrapper::inspect_model_file(const std::string& model_path) {
    ModelFileInfo info;

    // Magic check first: ggml's GGUF parser expects GGUF and otherwise reports
    // through the ggml logger, while a wrong path or a stray JSON manifest file is
    // a configuration mistake worth naming.
    {
        std::ifstream file(model_path, std::ios::binary);
        if (!file) {
            info.problem = "Model file cannot be opened:\n  " + model_path;
            info.unusable = true;
            return info;
        }
        char magic[4] = {};
        file.read(magic, sizeof(magic));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
            std::memcmp(magic, "GGUF", sizeof(magic)) != 0) {
            info.problem = "Model file is not a GGUF file (the 'GGUF' magic bytes are missing):\n  " + model_path;
            info.unusable = true;
            return info;
        }
    }

    // Metadata only: no_alloc keeps the tensor data out of the read (milliseconds
    // even for an 8 GB model) and a null ctx skips the ggml context entirely.
    gguf_init_params params = {};
    params.no_alloc = true;
    params.ctx = nullptr;
    gguf_context* ctx = gguf_init_from_file(model_path.c_str(), params);
    if (!ctx) {
        // Not fatal - llama.cpp still gets its own attempt at the file.
        info.problem = "GGUF metadata could not be read (truncated or corrupt file?):\n  " + model_path;
        return info;
    }

    auto read_string = [ctx](const char* key) -> std::string {
        const int64_t key_id = gguf_find_key(ctx, key);
        if (key_id < 0 || gguf_get_kv_type(ctx, key_id) != GGUF_TYPE_STRING) return std::string();
        const char* value = gguf_get_val_str(ctx, key_id);
        return value ? std::string(value) : std::string();
    };

    info.readable = true;
    info.architecture = read_string("general.architecture");
    info.name = read_string("general.name");
    const std::string general_type = read_string("general.type");
    gguf_free(ctx);

    describe_model_file_problem(model_path, general_type, info);
    return info;
}

LlamaWrapper::LlamaWrapper()
    : initialized_(false), has_vision_(false), port_(8081), model_(nullptr), context_(nullptr),
       sampler_(nullptr), vocab_(nullptr), n_ctx_(4096), n_batch_(512), cache_pos_(0) {
}

LlamaWrapper::~LlamaWrapper() {
    shutdown();
}

bool LlamaWrapper::initialize(const std::string& model_path, const std::string& llama_server_path, const std::string& mmproj_path, int port) {
    (void)llama_server_path;
    (void)port;
    model_path_ = model_path;
    port_ = port;
    mmproj_path_ = mmproj_path;

    // GPU backend auto-detection (no user setting). Both the CUDA and the Vulkan
    // backend are compiled into the exe; if both stayed active, ggml would expose
    // the same physical GPU twice (CUDA0 + Vulkan0) and the model would be split
    // across two backends. The vendor of the primary display adapter (DXGI; the
    // adapter with the most dedicated VRAM) therefore selects the backend, by
    // hiding the other backend's devices in the process environment. This must
    // happen before llama_backend_init(), because the ggml backend registry it
    // builds is constructed once and never re-reads these variables:
    //  - NVIDIA (0x10DE)               -> CUDA,   Vulkan devices hidden
    //  - AMD (0x1002) / Intel (0x8086) -> Vulkan, CUDA devices hidden
    //  - no GPU adapter                -> CPU only (both backends hidden)
    // Value semantics (verified against this build's backends):
    //  - CUDA: only "-1" makes the CUDA runtime report cudaErrorNoDevice; an unset
    //    OR empty CUDA_VISIBLE_DEVICES keeps every device visible, so that variable
    //    must never be blanked out on the CUDA path.
    //  - Vulkan: GGML_VK_VISIBLE_DEVICES is parsed as a list of indices, so "none"
    //    parses to an empty list -> the Vulkan backend registers 0 devices.
    {
#ifdef _WIN32
        const int vendor_id = query_primary_gpu_vendor_id();
        if (vendor_id == GPU_VENDOR_NVIDIA) {
            // A user-provided CUDA_VISIBLE_DEVICES (GPU pick) is left untouched.
            set_process_env("GGML_VK_VISIBLE_DEVICES", "none");
            LOG_LLAMANATIVE("GPU backend: NVIDIA adapter detected - using CUDA (Vulkan devices hidden)");
        } else if (vendor_id == GPU_VENDOR_AMD || vendor_id == GPU_VENDOR_INTEL) {
            set_process_env("CUDA_VISIBLE_DEVICES", "-1");
            LOG_LLAMANATIVE(vendor_id == GPU_VENDOR_AMD
                ? "GPU backend: AMD adapter detected - using Vulkan (CUDA devices hidden)"
                : "GPU backend: Intel adapter detected - using Vulkan (CUDA devices hidden)");
        } else {
            set_process_env("CUDA_VISIBLE_DEVICES", "-1");
            set_process_env("GGML_VK_VISIBLE_DEVICES", "none");
            LOG_LLAMANATIVE("GPU backend: no GPU adapter detected - running on CPU only");
        }
#else
        LOG_LLAMANATIVE("GPU backend: auto (all backends visible)");
#endif
    }

    {
        std::lock_guard<std::mutex> lock(g_backend_mutex);
        if (!g_backend_initialized) {
            llama_log_set(llama_log_callback, nullptr);
            llama_backend_init();
            g_backend_initialized = true;
            LOG_LLAMANATIVE("Backend initialized");
        }
    }

    LOG_LLAMANATIVE("Loading model: " + model_path_);

    // Pre-flight the GGUF header before any weights are touched. An architecture
    // the linked llama.cpp does not implement (llama.cpp's own failure is the bare
    // "unknown model architecture: '<name>'") or an mmproj projector file supplied
    // by mistake is reported here with the reason, the alternatives and the
    // architectures that do load, instead of a failure that says nothing.
    {
        ModelFileInfo info = inspect_model_file(model_path_);
        if (info.unusable) {
            std::istringstream problem_lines(info.problem);
            std::string line;
            while (std::getline(problem_lines, line)) {
                std::cerr << "[LLAMA-NATIVE] " << line << std::endl;
            }
            std::cerr << "[LLAMA-NATIVE] Model load skipped: the bundled llama.cpp cannot load this file." << std::endl;
            return false;
        }
        if (!info.problem.empty()) {
            std::cerr << "[LLAMA-NATIVE] " << info.problem << std::endl;
        }
    }

#ifdef _WIN32
    // Snapshot VRAM before the weights go in. If the post-load snapshot below
    // shows usage near the budget, GPU memory is about to be paged over PCIe.
    {
        GpuVramInfo vram_before = query_gpu_vram();
        if (vram_before.available) {
            LOG_LLAMANATIVE("GPU before load: " + format_gpu_vram(vram_before));
        }
    }
#endif

    auto llama_load_start = std::chrono::high_resolution_clock::now();

    llama_model_params model_params = llama_model_default_params();
    // Offload all layers to GPU (CUDA or Vulkan). If the GPU load fails the context
    // creation below will trigger a CPU-only retry automatically.
    model_params.n_gpu_layers = 99;
    model_params.progress_callback = nullptr;

    model_.reset(llama_model_load_from_file(model_path_.c_str(), model_params));
    if (!model_) {
        // The GPU (CUDA/Vulkan) model load itself failed — e.g. Vulkan
        // ErrorOutOfDeviceMemory when the GPU does not have enough free VRAM
        // for the weights ("unable to allocate Vulkan0 buffer"). The previous
        // code only retried on context-creation failure, which never fired
        // here, so Sonny aborted entirely. Reload CPU-only so the assistant
        // still starts (inference is slower, but fully functional).
        std::cerr << "[LLAMA-NATIVE] GPU model load failed, retrying CPU-only..." << std::endl;
        llama_model_params cpu_load_params = llama_model_default_params();
        cpu_load_params.n_gpu_layers = 0;
        model_.reset(llama_model_load_from_file(model_path_.c_str(), cpu_load_params));
        if (!model_) {
            std::cerr << "[LLAMA-NATIVE] Failed to load model from: " << model_path_ << std::endl;
            return false;
        }
    }
    LOG_LLAMANATIVE("Model loaded successfully");

    vocab_ = llama_model_get_vocab(model_.get());
    if (!vocab_) {
        std::cerr << "[LLAMA-NATIVE] Failed to get vocab from model" << std::endl;
        return false;
    }
    LOG_LLAMANATIVE("Vocab loaded: " + std::to_string(llama_vocab_n_tokens(vocab_)) + " tokens");

    unsigned int hw_threads = std::thread::hardware_concurrency();
    // Use more threads for CPU inference; on GPU these are used for the CPU parts only.
    int n_threads = hw_threads > 0 ? (int)std::clamp(hw_threads, 4u, 32u) : 4;
    int n_threads_batch = hw_threads > 0 ? (int)std::clamp(hw_threads, 8u, 32u) : 8;

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = n_ctx_;
    ctx_params.n_batch = n_batch_;    // 512: same GPU prefill speed as larger batches,
                                      // ~4x smaller activation-memory peak (less VRAM)
    ctx_params.n_seq_max = 1;
    ctx_params.n_threads = n_threads;
    ctx_params.n_threads_batch = n_threads_batch;
    // Context fallback ladder (each rung only if the previous one fails to create):
    //   1) Flash attention + Q8_0 KV cache - fastest, ~half the KV VRAM
    //      (quantized KV requires flash attention, hence the ladder order).
    //   2) Flash attention + F16 KV cache - if quantized KV is not supported.
    //   3) No flash attention + F16 KV cache - if FA is not supported.
    //   4) CPU-only reload - if no GPU context can be created at all.
    // Keeping VRAM low matters on 6 GB GPUs: exceeding the WDDM budget makes
    // Windows page GPU memory over PCIe, slowing inference down by 10-50x.
    ctx_params.type_k = GGML_TYPE_Q8_0;
    ctx_params.type_v = GGML_TYPE_Q8_0;
    ctx_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;

    context_.reset(llama_init_from_model(model_.get(), ctx_params));
    if (!context_) {
        ctx_params.type_k = GGML_TYPE_F16;
        ctx_params.type_v = GGML_TYPE_F16;
        context_.reset(llama_init_from_model(model_.get(), ctx_params));
    }
    if (!context_) {
        // Flash attention failed - retry with flash attention disabled.
        std::cerr << "[LLAMA-NATIVE] Flash-attn context failed, retrying without flash attention..." << std::endl;
        ctx_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
        context_.reset(llama_init_from_model(model_.get(), ctx_params));
    }
    if (!context_) {
        // GPU entirely unavailable — reload model CPU-only.
        std::cerr << "[LLAMA-NATIVE] GPU context failed, falling back to CPU-only..." << std::endl;
        llama_model_params cpu_model_params = llama_model_default_params();
        cpu_model_params.n_gpu_layers = 0;
        model_.reset(llama_model_load_from_file(model_path_.c_str(), cpu_model_params));
        if (!model_) {
            std::cerr << "[LLAMA-NATIVE] Failed to load model on CPU" << std::endl;
            return false;
        }
        vocab_ = llama_model_get_vocab(model_.get());
        if (!vocab_) {
            std::cerr << "[LLAMA-NATIVE] Failed to get vocab from model" << std::endl;
            return false;
        }
        ctx_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
        context_.reset(llama_init_from_model(model_.get(), ctx_params));
    }
    if (!context_) {
        std::cerr << "[LLAMA-NATIVE] Failed to create context" << std::endl;
        return false;
    }

    n_ctx_ = llama_n_ctx(context_.get());
    n_batch_ = llama_n_batch(context_.get());

    uint64_t model_size = llama_model_size(model_.get());
    uint64_t model_n_params = llama_model_n_params(model_.get());
    double model_size_mb = model_size / (1024.0 * 1024.0);
    double model_params_b = model_n_params / 1e9;
    LOG_DEBUG("TELEMETRY", "LLM_MODEL_SIZE: " + std::to_string(model_size_mb) + " MB, PARAMS: " + std::to_string(model_params_b) + "B");

#ifdef _WIN32
    // Registry-based detection: with statically linked backends no ggml-*.dll
    // is ever loaded, so module checks would always report "CPU".
    std::string gpu_backend = ggml_gpu_backend_name();  // "CUDA", "Vulkan", or "CPU"
    LOG_DEBUG("TELEMETRY", "GPU_BACKEND: " + gpu_backend);
    if (gpu_backend == "CPU" && model_params_b > 2.0) {
        std::cerr << "[WARNING] Large model (" << model_params_b << "B) running on CPU. "
                  << "Response will be slow. Consider using a smaller model (<=3B) or enabling GPU acceleration." << std::endl;
    }
#endif

    LOG_LLAMANATIVE(std::string("Context ready: n_ctx=") + std::to_string(n_ctx_)
        + ", n_batch=" + std::to_string(n_batch_)
        + ", flash_attn=" + llama_flash_attn_type_name(ctx_params.flash_attn_type)
        + ", kv_cache=" + (ctx_params.type_k == GGML_TYPE_Q8_0 ? "Q8_0" : "F16"));

#ifdef _WIN32
    {
        GpuVramInfo vram_after = query_gpu_vram();
        if (vram_after.available) {
            LOG_LLAMANATIVE("GPU after load: " + format_gpu_vram(vram_after));
            long long used_mb = vram_after.used_bytes / (1024 * 1024);
            long long budget_mb = vram_after.budget_bytes / (1024 * 1024);
            if (budget_mb > 0 && used_mb * 100 / budget_mb >= 90) {
                LOG_WARN("LLAMA-NATIVE", "VRAM nearly saturated (" + std::to_string(used_mb) + "/"
                    + std::to_string(budget_mb) + " MiB). GPU memory oversubscription makes Windows "
                    "page it over PCIe, slowing inference by 10-50x. Close GPU-heavy apps or use a "
                    "smaller model / context.");
            }
        }
     }
#endif

#ifdef SONNY_HAS_MTMD
    // Initialize the multimodal (mtmd) context if a projector file was supplied.
    // The mmproj GGUF is a small vision encoder that sits alongside the main
    // language model; mtmd_init_from_file() binds it to the already-loaded model.
    if (!mmproj_path_.empty()) {
        if (std::filesystem::exists(mmproj_path_)) {
            mtmd_context_params mparams = mtmd_context_params_default();
            mparams.n_threads = n_threads;
            mparams.use_gpu = ggml_has_gpu_backend();
            mparams.media_marker = mtmd_default_marker();

            mtmd_context* raw_mtmd = mtmd_init_from_file(
                mmproj_path_.c_str(), model_.get(), mparams);
            if (raw_mtmd) {
                mtmd_ctx_.reset(raw_mtmd);
                if (mtmd_support_vision(mtmd_ctx_.get())) {
                    has_vision_ = true;
                    LOG_LLAMANATIVE("Multimodal context initialized (vision enabled)");
                } else {
                    LOG_LLAMANATIVE("Multimodal context initialized (vision not supported by this model)");
                }
            } else {
                LOG_WARN("LLAMA-NATIVE", "Failed to initialize mtmd context from: " + mmproj_path_);
            }
        } else {
            LOG_WARN("LLAMA-NATIVE", "mmproj file not found: " + mmproj_path_);
        }
    }
#endif

    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    sampler_.reset(llama_sampler_chain_init(sparams));
    if (!sampler_) {
        std::cerr << "[LLAMA-NATIVE] Failed to create sampler chain" << std::endl;
        return false;
    }
    llama_sampler_chain_add(sampler_.get(), llama_sampler_init_temp(current_temperature_));
    // Anti-repetition: without it the 3B model frequently enters degenerate
    // loops that burn the entire max_tokens budget - catastrophic on a
    // throttled GPU where every wasted token costs 40-500 ms.
    llama_sampler_chain_add(sampler_.get(), llama_sampler_init_penalties(
        llama_vocab_n_tokens(vocab_), /*penalty_last_n=*/64, /*penalty_repeat=*/1.1f,
        /*penalty_freq=*/0.0f, /*penalty_present=*/0.0f));
    // Greedy must stay last: it terminates the chain with the sampled token.
    llama_sampler_chain_add(sampler_.get(), llama_sampler_init_greedy());

#ifdef _WIN32
    // Absorb CUDA module load / JIT and cuBLAS init costs before the first real
    // query so the user's first question is not the slowest one. GPU only: a
    // full ~2k-token CPU prefill would stall startup for minutes.
    if (ggml_has_gpu_backend()) {
        warmup();
    }
#endif

    auto llama_load_end = std::chrono::high_resolution_clock::now();
    auto llama_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(llama_load_end - llama_load_start).count();
    LOG_DEBUG("TELEMETRY", "LLM_MODEL_LOAD: " + std::to_string(llama_load_ms) + " ms");

    initialized_ = true;
    LOG_LLAMANATIVE("Initialized successfully");
    return true;
}

void LlamaWrapper::warmup() {
    if (!context_ || !vocab_) return;

    std::string warmup_prompt = build_prompt("Hello");
    std::vector<llama_token> warmup_tokens = tokenize(warmup_prompt, true);
    if (warmup_tokens.empty()) {
        std::cerr << "[LLAMA-NATIVE] Warmup: no tokens to process" << std::endl;
        return;
    }

    std::cerr << "[LLAMA-NATIVE] Warmup: processing " << warmup_tokens.size() << " tokens..." << std::endl;

    for (int iter = 0; iter < 1; iter++) {
        llama_memory_clear(llama_get_memory(context_.get()), true);

        int pos = 0;
        auto prefill_start = std::chrono::high_resolution_clock::now();
        while (pos < (int)warmup_tokens.size()) {
            int chunk = std::min((int)llama_n_batch(context_.get()), (int)warmup_tokens.size() - pos);
            llama_batch batch = llama_batch_get_one(warmup_tokens.data() + pos, chunk);
            if (llama_decode(context_.get(), batch) != 0) {
                std::cerr << "[LLAMA-NATIVE] Warmup: batch decode failed at iter " << iter << std::endl;
                break;
            }
            pos += chunk;
        }
        auto prefill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - prefill_start).count();
        if (prefill_ms > 0) {
            // Startup self-test: the app's ACTUAL prefill speed on a production-size
            // prompt (full system prompt + tools). Healthy GPU: several hundred tok/s.
            // Single digits: GPU memory is being paged over PCIe or the GPU throttles.
            std::ostringstream perf;
            perf << "Warmup prefill self-test: " << warmup_tokens.size() << " tokens in "
                 << prefill_ms << " ms (" << std::fixed << std::setprecision(1)
                 << (double)warmup_tokens.size() * 1000.0 / (double)prefill_ms << " tok/s)";
            LOG_PERF(perf.str());
        }

        llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
        llama_sampler_ptr warmup_sampler(llama_sampler_chain_init(sparams));
        if (!warmup_sampler) {
            std::cerr << "[LLAMA-NATIVE] Warmup: failed to create sampler" << std::endl;
            continue;
        }
        llama_sampler_chain_add(warmup_sampler.get(), llama_sampler_init_temp(0.7f));
        llama_sampler_chain_add(warmup_sampler.get(), llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), 64, 1.0f, 0.0f, 0.0f));
        llama_sampler_chain_add(warmup_sampler.get(), llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

        llama_token token;
        std::string piece;
        auto gen_start = std::chrono::high_resolution_clock::now();
        int gen_count = 0;
        for (int i = 0; i < 8; i++) {
            if (!sample_and_decode(context_.get(), warmup_sampler.get(), token, piece)) {
                break;
            }
            if (token == llama_vocab_eos(vocab_)) {
                break;
            }
        llama_batch next = llama_batch_get_one(&token, 1);
        if (llama_decode(context_.get(), next) != 0) {
            break;
        }
            gen_count++;
        }
        auto gen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - gen_start).count();
        if (gen_ms > 0 && gen_count > 0) {
            std::ostringstream perf;
            perf << "Warmup generation self-test: " << gen_count << " tokens in "
                 << gen_ms << " ms (" << std::fixed << std::setprecision(1)
                 << (double)gen_count * 1000.0 / (double)gen_ms << " tok/s)";
            LOG_PERF(perf.str());
        }

        // Keep the warmup's KV cache and register it as the prompt prefix
        // cache: the prefill above already computed the production system
        // prompt (pushed by the orchestrator before initialize()), so the
        // first real user turn only prefills its own tokens (a few dozen)
        // instead of re-prefilling the full ~2k-token prompt. The generated
        // warmup tokens sit at positions past the prompt and are truncated
        // by the prefix match on the next generate() call.
        if (pos == (int)warmup_tokens.size()) {
            cached_tokens_ = warmup_tokens;
            cache_pos_ = (int)warmup_tokens.size();
            cached_system_prompt_ = system_prompt_;
        }
    }

    std::cerr << "[LLAMA-NATIVE] Warmup complete" << std::endl;
}

void LlamaWrapper::shutdown() {
    if (!initialized_) return;

    LOG_LLAMANATIVE("Shutting down...");

    // Clear memory before releasing resources
    if (context_) {
        llama_memory_clear(llama_get_memory(context_.get()), true);
    }

    sampler_.reset();
    context_.reset();
    model_.reset();
    vocab_ = nullptr;
#ifdef SONNY_HAS_MTMD
    mtmd_ctx_.reset();
#endif
    has_vision_ = false;

    initialized_ = false;
    cache_pos_ = 0;
    cached_tokens_.clear();
    cached_tokens_.shrink_to_fit();
    cached_system_prompt_.clear();
    conversation_history_.clear();
    history_.clear();
    
    LOG_LLAMANATIVE("Shutdown complete");
}

std::vector<llama_token> LlamaWrapper::tokenize(const std::string& text, bool add_special) const {
    std::vector<llama_token> tokens;
    if (text.empty() || !vocab_) return tokens;

    const char* data = text.c_str();
    int32_t len = (int32_t)text.length();

    int32_t n_tokens = llama_tokenize(vocab_, data, len, nullptr, 0, add_special, false);
    if (n_tokens < 0) {
        n_tokens = -n_tokens;
    }

    if (n_tokens <= 0) {
        std::cerr << "[LLAMA-NATIVE] Tokenize failed" << std::endl;
        return tokens;
    }

    tokens.resize(n_tokens);
    int32_t actual = llama_tokenize(vocab_, data, len, tokens.data(), n_tokens, add_special, false);
    if (actual <= 0) {
        tokens.clear();
    }
    return tokens;
}

std::string LlamaWrapper::detokenize(const llama_token* tokens, int n_tokens) const {
    if (!tokens || n_tokens <= 0 || !vocab_) return "";

    std::string result;
    result.reserve(n_tokens * 4);

    char buf[256];
    for (int i = 0; i < n_tokens; ++i) {
        int n = llama_token_to_piece(vocab_, tokens[i], buf, sizeof(buf), 0, true);
        if (n > 0) {
            result.append(buf, n);
        }
    }
    return result;
}

std::string LlamaWrapper::build_prompt(const std::string& user_message) const {
    if (!vocab_ || !context_) return user_message;

    const char* chat_template = llama_model_chat_template(model_.get(), nullptr);
    if (!chat_template) {
        std::string fallback;
        if (!system_prompt_.empty()) {
            fallback += "System: " + system_prompt_ + "\n\n";
        }
        fallback += "User: " + user_message + "\nAssistant: ";
        return fallback;
    }

    static const char* kStartHeader = "<|start_header_id|>";
    static const char* kEndHeader = "<|end_header_id|>";
    static const char* kEotId = "<|eot_id|>";

    std::vector<llama_chat_message> messages;
    if (!system_prompt_.empty()) {
        messages.push_back({"system", system_prompt_.c_str()});
    }
    messages.push_back({"user", user_message.c_str()});

    std::vector<char> buf(32768);
    int ret = llama_chat_apply_template(chat_template, messages.data(), (int)messages.size(), true, buf.data(), (int)buf.size());
    if (ret > 0 && ret < (int)buf.size()) {
        return std::string(buf.data(), ret);
    }

    std::string fallback;
    fallback.reserve(8192);
    if (!system_prompt_.empty()) {
        fallback += kStartHeader;
        fallback += "system";
        fallback += kEndHeader;
        fallback += "\n\n";
        fallback += system_prompt_;
        fallback += kEotId;
    }
    fallback += kStartHeader;
    fallback += "user";
    fallback += kEndHeader;
    fallback += "\n\n";
    fallback += user_message;
    fallback += kEotId;
    fallback += kStartHeader;
    fallback += "assistant";
    fallback += kEndHeader;
    fallback += "\n\n";

    return fallback;
}

bool LlamaWrapper::sample_and_decode(llama_context* ctx, llama_sampler* smpl, llama_token& out_token, std::string& piece) const {
    if (!ctx || !smpl || !vocab_) return false;

    llama_token token = llama_sampler_sample(smpl, ctx, -1);
    llama_sampler_accept(smpl, token);
    out_token = token;

    char buf[256];
    int n = llama_token_to_piece(vocab_, token, buf, sizeof(buf), 0, true);
    if (n > 0) {
        piece.assign(buf, n);
    } else {
        piece.clear();
    }

    return true;
}

std::string LlamaWrapper::generate(const std::string& prompt, int max_tokens, float temperature) {
    return generate_impl(prompt, max_tokens, temperature, nullptr);
}

void LlamaWrapper::generate_stream(const std::string& prompt, TokenCallback callback, int max_tokens, float temperature) {
    if (!initialized_ || !callback) return;
    generate_impl(prompt, max_tokens, temperature, callback);
}

std::string LlamaWrapper::generate_with_images(const std::string& prompt,
        const std::vector<std::vector<unsigned char>>& images,
        int width, int height, int max_tokens, float temperature) {
    return generate_impl_with_images(prompt, images, width, height, max_tokens, temperature, nullptr);
}

void LlamaWrapper::generate_stream_with_images(const std::string& prompt,
        const std::vector<std::vector<unsigned char>>& images,
        int width, int height, TokenCallback callback, int max_tokens, float temperature) {
    if (!initialized_ || !callback) return;
    generate_impl_with_images(prompt, images, width, height, max_tokens, temperature, callback);
}

std::string LlamaWrapper::generate_impl(const std::string& prompt, int max_tokens, float temperature, TokenCallback callback) {
    if (!initialized_) return "Error: Llama not initialized";

    std::lock_guard<std::mutex> lock(generate_mutex_);
    last_prompt_tokens_ = 0;
    last_cached_tokens_ = 0;
    last_generated_tokens_ = 0;
    last_prefill_ms_ = 0;
    last_generate_ms_ = 0;

#ifdef _WIN32
    {
        GpuVramInfo vram_now = query_gpu_vram();
        if (vram_now.available) {
            LOG_DEBUG("LLAMA-NATIVE", "GPU VRAM at inference start: " + format_gpu_vram(vram_now));
        }
    }
#endif

    auto t0 = std::chrono::high_resolution_clock::now();

    std::string full_prompt = build_prompt(prompt);

    auto t1 = std::chrono::high_resolution_clock::now();

    std::vector<llama_token> tokens = tokenize(full_prompt, true);
    if (tokens.empty()) {
        return "Error: Failed to tokenize prompt";
    }

    auto t2 = std::chrono::high_resolution_clock::now();

    size_t common = 0;

    if (cached_system_prompt_ != system_prompt_ || cache_pos_ == 0 || cached_tokens_.empty()) {
        llama_memory_clear(llama_get_memory(context_.get()), true);
        cache_pos_ = 0;
        cached_tokens_.clear();
    } else {
        size_t max_common = std::min(cached_tokens_.size(), tokens.size());
        while (common < max_common && cached_tokens_[common] == tokens[common]) {
            common++;
        }
        if (common == 0) {
            llama_memory_clear(llama_get_memory(context_.get()), true);
            cache_pos_ = 0;
            cached_tokens_.clear();
        } else {
            // Truncate KV cache past the common prefix
            llama_memory_seq_rm(llama_get_memory(context_.get()), 0, (llama_pos)common, -1);
        }
    }
    cached_system_prompt_ = system_prompt_;

    if (temperature != current_temperature_) {
        // Drain the chain and rebuild it, preserving the order
        // [temp, penalties, greedy]. Greedy must remain last (it terminates
        // the chain); an earlier temp-swap bug left [greedy, temp], where
        // greedy always won and the temperature was silently ignored.
        const int chain_n = llama_sampler_chain_n(sampler_.get());
        for (int i = chain_n - 1; i >= 0; --i) {
            if (llama_sampler* s = llama_sampler_chain_remove(sampler_.get(), (size_t)i)) {
                llama_sampler_free(s);
            }
        }
        llama_sampler_chain_add(sampler_.get(), llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(sampler_.get(), llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), 64, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(sampler_.get(), llama_sampler_init_greedy());
        current_temperature_ = temperature;
    }
    llama_sampler_reset(sampler_.get());

    int actual_batch = llama_n_batch(context_.get());
    int pos = (int)common;
    bool prompt_retry_used = false;
    auto t2a = std::chrono::high_resolution_clock::now();
    while (pos < (int)tokens.size()) {
        int chunk = std::min(actual_batch, (int)tokens.size() - pos);
        llama_batch batch = llama_batch_get_one(tokens.data() + pos, chunk);
        if (llama_decode(context_.get(), batch) != 0) {
            std::cerr << "[LLAMA-NATIVE] Failed to decode prompt batch at pos=" << pos << std::endl;
            if (prompt_retry_used) {
                std::cerr << "[LLAMA-NATIVE] Prompt decode retry failed" << std::endl;
                last_prompt_tokens_ = (int)tokens.size();
                last_cached_tokens_ = (int)common;
                last_prefill_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - t2a).count();
                return "Error: Failed to decode prompt";
            }
            prompt_retry_used = true;
            llama_memory_clear(llama_get_memory(context_.get()), true);
            cache_pos_ = 0;
            cached_tokens_.clear();
            pos = 0;
            continue;
        }
        pos += chunk;
    }
    auto t2b = std::chrono::high_resolution_clock::now();

    std::string result;
    result.reserve(max_tokens * 4);
    llama_token token;
    std::string piece;
    int n_generated = 0;

    while (n_generated < max_tokens) {
        auto token_start = std::chrono::high_resolution_clock::now();
        
        if (!sample_and_decode(context_.get(), sampler_.get(), token, piece)) {
            std::cerr << "[LLAMA-NATIVE] sample_and_decode failed at n_generated=" << n_generated << std::endl;
            break;
        }

        if (token == llama_vocab_eos(vocab_) || llama_vocab_is_eog(vocab_, token)) {
            break;
        }

        if (piece.find("<|eot_id|>") != std::string::npos ||
            piece.find("<|im_end|>") != std::string::npos ||
            piece.find("<|end|>") != std::string::npos ||
            piece.find("<|end_of_text|>") != std::string::npos ||
            piece.find("<|endoftext|>") != std::string::npos ||
            piece.find("<|start_header_id|>") != std::string::npos) {
            break;
        }

        if (!piece.empty()) {
            result += piece;
            if (callback) {
                callback(piece);
            }
        }

        llama_batch next = llama_batch_get_one(&token, 1);
        if (llama_decode(context_.get(), next) != 0) {
            // The KV cache is full: the prompt plus everything generated so far
            // has reached n_ctx, so llama.cpp cannot allocate another slot.
            // Returning the text so far silently made a cut-off tool call look
            // like a model that simply stopped, so report what actually happened.
            LOG_WARN("LLAMA-NATIVE",
                     "Generation truncated: KV cache full after " + std::to_string(n_generated) +
                     " tokens (prompt " + std::to_string(tokens.size()) + " of n_ctx " +
                     std::to_string(n_ctx_) + "). Raise the context size or shorten the prompt.");
            break;
        }

        n_generated++;
    }

    cached_tokens_ = tokens;
    cache_pos_ = (int)tokens.size();

    auto t3 = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();
    last_prompt_tokens_ = (int)tokens.size();
    last_cached_tokens_ = (int)common;
    last_generated_tokens_ = n_generated;
    last_prefill_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2b - t2a).count();
    last_generate_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2b).count();
    LOG_DEBUG("LLAMA-NATIVE", "Inference: " + std::to_string(total_ms) + " ms (" + std::to_string(tokens.size()) + " prompt + " + std::to_string(n_generated) + " generated tokens)");

    // One-time slow-prefill diagnostic: on a healthy GPU the prefill runs at
    // several hundred tok/s. Single-digit rates are the signature of GPU memory
    // being paged over PCIe (WDDM shared-memory spill) or thermal throttling.
    if (!warned_slow_prefill_ && last_prefill_ms_ > 0 &&
        (int)tokens.size() - (int)common >= 256 &&
        ((double)(tokens.size() - common) * 1000.0 / (double)last_prefill_ms_) < 100.0) {
        warned_slow_prefill_ = true;
        std::ostringstream warn;
        warn << "Slow prompt processing (" << std::fixed << std::setprecision(1)
             << (double)(tokens.size() - common) * 1000.0 / (double)last_prefill_ms_
             << " tok/s). Causes: thermal/power throttling, VRAM demotion to "
             << "shared memory, or CPU starvation.";
#ifdef _WIN32
        GpuVramInfo v = query_gpu_vram();
        if (v.available) {
            warn << " Sonny process: " << format_gpu_vram(v);
        }
        std::string smi = query_nvidia_smi();
        if (!smi.empty()) {
            warn << " GPU [tempC,coreClkMHz,memClkMHz,powerW,util%,pstate]: " << smi;
        }
#endif
        LOG_WARN("LLAMA-NATIVE", warn.str());
    }

    // Generation-collapse diagnostic: healthy decode on this GPU runs ~25-30
    // tok/s. A per-turn collapse (18 -> 4 -> 2 -> 0.5 tok/s) is the signature
    // of GPU memory being demoted to shared memory (model weights streaming
    // over PCIe) as VRAM fills up. Log the VRAM state when it happens.
    if (last_generate_ms_ > 0 && n_generated >= 16 &&
        ((double)n_generated * 1000.0 / (double)last_generate_ms_) < 10.0) {
        static std::chrono::steady_clock::time_point last_gen_warn{};
        auto now_steady = std::chrono::steady_clock::now();
        if (now_steady - last_gen_warn > std::chrono::seconds(30)) {
            last_gen_warn = now_steady;
            std::ostringstream gwarn;
            gwarn << "Slow generation (" << std::fixed << std::setprecision(1)
                  << (double)n_generated * 1000.0 / (double)last_generate_ms_
                  << " tok/s) - causes: thermal/power throttling (Max-Q), VRAM "
                  << "demotion to shared memory, or CPU starvation.";
#ifdef _WIN32
            GpuVramInfo v = query_gpu_vram();
            if (v.available) {
                gwarn << " Sonny process: " << format_gpu_vram(v);
            }
            std::string smi = query_nvidia_smi();
            if (!smi.empty()) {
                gwarn << " GPU [tempC,coreClkMHz,memClkMHz,powerW,util%,pstate]: " << smi;
            }
#endif
            LOG_WARN("LLAMA-NATIVE", gwarn.str());
        }
    }

    // No conversation history tracking - RAG provides context
    return result;
}

// ============================================================================
// LoRA adapter support (vectors personalization)
//
// A LoRA adapter is a small GGUF file of diff tensors (~100 KB-2 MB). The
// vectors shares adapter bytes between peers; each node applies the
// received adapters to its local model via llama_set_adapters_lora().
// Multiple adapters can be active simultaneously, each with its own scale.
//
// Privacy: adapters are content-free by construction - they are weights, not
// data. A peer never sees the user's conversations, only the mathematical
// delta that makes the model more helpful for that peer's domain.
// ============================================================================

bool LlamaWrapper::load_lora_adapter(const std::string& path) {
    if (!initialized_ || !model_) {
        LOG_LLAMANATIVE("LoRA: cannot load adapter - model not initialized");
        return false;
    }
    if (path.empty()) return false;

    std::ifstream test(path, std::ios::binary);
    if (!test.is_open()) {
        LOG_LLAMANATIVE("LoRA: adapter file not found: " + path);
        return false;
    }
    test.close();

    llama_adapter_lora* adapter = llama_adapter_lora_init(model_.get(), path.c_str());
    if (!adapter) {
        LOG_LLAMANATIVE("LoRA: failed to load adapter from: " + path);
        return false;
    }

    std::lock_guard<std::mutex> lock(lora_mutex_);
    lora_adapters_.emplace_back(adapter);
    lora_scales_.push_back(1.0f);
    LOG_LLAMANATIVE("LoRA: loaded adapter from " + path +
                    " (" + std::to_string(lora_adapters_.size()) + " active)");
    return true;
}

bool LlamaWrapper::apply_lora(float scale) {
    if (!initialized_ || !context_) return false;

    std::lock_guard<std::mutex> lock(lora_mutex_);
    if (lora_adapters_.empty()) return false;

    // Clamp scale to a reasonable range to prevent numerical instability.
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 2.0f) scale = 2.0f;

    // Apply the same scale to all active adapters. A caller that wants
    // per-adapter scaling can clear and re-add with explicit scales.
    for (float& s : lora_scales_) s = scale;

    std::vector<llama_adapter_lora*> ptrs;
    ptrs.reserve(lora_adapters_.size());
    for (const auto& a : lora_adapters_) ptrs.push_back(a.get());

    const int32_t result = llama_set_adapters_lora(
        context_.get(), ptrs.data(), ptrs.size(), lora_scales_.data());

    if (result != 0) {
        LOG_LLAMANATIVE("LoRA: llama_set_adapters_lora failed (code " +
                        std::to_string(result) + ")");
        return false;
    }

    LOG_LLAMANATIVE("LoRA: applied " + std::to_string(ptrs.size()) +
                    " adapter(s) at scale " + std::to_string(scale));
    return true;
}

void LlamaWrapper::clear_lora() {
    std::lock_guard<std::mutex> lock(lora_mutex_);
    if (lora_adapters_.empty()) return;

    // Detach from context before freeing.
    if (context_) {
        llama_set_adapters_lora(context_.get(), nullptr, 0, nullptr);
    }

    const int count = static_cast<int>(lora_adapters_.size());
    lora_adapters_.clear();
    lora_scales_.clear();
    LOG_LLAMANATIVE("LoRA: cleared " + std::to_string(count) + " adapter(s)");
}

// ============================================================================
// Vision / multimodal support
//
// When an mmproj (multimodal projector) file is supplied at initialize() time,
// LlamaWrapper uses mtmd to encode images into embeddings that are injected
// into the language model's token stream. Vision queries automatically use
// the image path; non-vision queries ignore it.
//
// The mmproj file is a standalone GGUF (e.g. mmproj-model-f16.gguf) that sits
// next to the main model — it is auto-detected in main.cpp, so no config needed.
// ============================================================================

std::string LlamaWrapper::generate_impl_with_images(
        const std::string& prompt,
        const std::vector<std::vector<unsigned char>>& images,
        int width, int height,
        int max_tokens, float temperature,
        TokenCallback callback) {

    if (!initialized_) return "Error: Llama not initialized";
    if (!has_vision_) return "Error: Vision not available (no mmproj loaded)";
#ifdef SONNY_HAS_MTMD
    if (!mtmd_ctx_) return "Error: mtmd context not initialized";
#endif
    if (images.empty()) return generate_impl(prompt, max_tokens, temperature, callback);

    std::lock_guard<std::mutex> lock(generate_mutex_);
    last_prompt_tokens_ = 0;
    last_cached_tokens_ = 0;
    last_generated_tokens_ = 0;
    last_prefill_ms_ = 0;
    last_generate_ms_ = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    // Clear KV cache — image embeddings are non-deterministic and cannot reuse
    // the text-only prefix cache.
    llama_memory_clear(llama_get_memory(context_.get()), true);
    cache_pos_ = 0;
    cached_tokens_.clear();
    cached_system_prompt_.clear();

    // Build a prompt with the media marker embedded in the user message so
    // mtmd_tokenize() knows where to splice in image tokens.
    const char* marker = mtmd_default_marker();
    std::string marked_prompt = build_prompt(std::string(marker) + prompt);

    mtmd_input_text input_text;
    input_text.text = marked_prompt.c_str();
    input_text.text_len = marked_prompt.size();
    input_text.add_special = true;
    input_text.parse_special = true;

    // Wrap each image as an mtmd_bitmap (RGB, width*height*3 bytes).
    std::vector<mtmd::bitmap> bitmaps;
    std::vector<const mtmd_bitmap*> bitmap_ptrs;
    bitmaps.reserve(images.size());
    bitmap_ptrs.reserve(images.size());
    for (const auto& img : images) {
        mtmd_bitmap* bmp = mtmd_bitmap_init(width, height, img.data());
        if (!bmp) {
            LOG_WARN("LLAMA-NATIVE", "Failed to create mtmd bitmap (" + std::to_string(width) + "x" + std::to_string(height) + ")");
            return "Error: Failed to create image bitmap";
        }
        bitmaps.emplace_back(bmp);
        bitmap_ptrs.push_back(bmp);
    }

    // Tokenize the prompt together with the images into input chunks.
    mtmd::input_chunks chunks(mtmd_input_chunks_init());
    int32_t tok_res = mtmd_tokenize(mtmd_ctx_.get(), chunks.ptr.get(),
                                    &input_text, bitmap_ptrs.data(), bitmap_ptrs.size());
    if (tok_res != 0) {
        LOG_WARN("LLAMA-NATIVE", "mtmd_tokenize failed (code " + std::to_string(tok_res) + ")");
        return "Error: Failed to tokenize image input";
    }

    last_prompt_tokens_ = (int)mtmd_helper_get_n_tokens(chunks.ptr.get());

    // Evaluate all chunks (text + image embeddings) through the llama context.
    // mtmd_helper_eval_chunks handles the text llama_decode() calls and the
    // image mtmd_encode_chunk() + embedding decode internally.
    llama_pos new_n_past = cache_pos_;
    int32_t eval_res = mtmd_helper_eval_chunks(
        mtmd_ctx_.get(),
        context_.get(),
        chunks.ptr.get(),
        cache_pos_,
        0,  // seq_id
        llama_n_batch(context_.get()),
        true,   // logits_last - compute logits so the sampler can draw the first token
        &new_n_past);
    if (eval_res != 0) {
        LOG_WARN("LLAMA-NATIVE", "mtmd_helper_eval_chunks failed (code " + std::to_string(eval_res) + ")");
        return "Error: Failed to evaluate image input";
    }
    cache_pos_ = new_n_past;

    auto t2 = std::chrono::high_resolution_clock::now();
    last_prefill_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();

    // Temperature sync with the normal generation path.
    if (temperature != current_temperature_) {
        const int chain_n = llama_sampler_chain_n(sampler_.get());
        for (int i = chain_n - 1; i >= 0; --i) {
            if (llama_sampler* s = llama_sampler_chain_remove(sampler_.get(), (size_t)i)) {
                llama_sampler_free(s);
            }
        }
        llama_sampler_chain_add(sampler_.get(), llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(sampler_.get(), llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), 64, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(sampler_.get(), llama_sampler_init_greedy());
        current_temperature_ = temperature;
    }
    llama_sampler_reset(sampler_.get());

    // Generation loop — same pattern as generate_impl().
    std::string result;
    result.reserve(max_tokens * 4);
    llama_token token;
    std::string piece;
    int n_generated = 0;

    while (n_generated < max_tokens) {
        if (!sample_and_decode(context_.get(), sampler_.get(), token, piece)) {
            break;
        }

        if (token == llama_vocab_eos(vocab_) || llama_vocab_is_eog(vocab_, token)) {
            break;
        }

        if (piece.find("<|eot_id|>") != std::string::npos ||
            piece.find("<|im_end|>") != std::string::npos ||
            piece.find("<|end|>") != std::string::npos ||
            piece.find("<|end_of_text|>") != std::string::npos ||
            piece.find("<|endoftext|>") != std::string::npos ||
            piece.find("<|start_header_id|>") != std::string::npos) {
            break;
        }

        if (!piece.empty()) {
            result += piece;
            if (callback) {
                callback(piece);
            }
        }

        llama_batch next = llama_batch_get_one(&token, 1);
        if (llama_decode(context_.get(), next) != 0) {
            break;
        }
        cache_pos_++;
        n_generated++;
    }

    last_generated_tokens_ = n_generated;
    last_generate_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - t2).count();
    LOG_DEBUG("LLAMA-NATIVE", "Vision inference: " + std::to_string(cache_pos_) + " prompt + "
        + std::to_string(n_generated) + " generated tokens");

    cached_tokens_.clear();
    cached_system_prompt_.clear();

    return result;
}