#include "AssistantOrchestrator.h"
#include "Logger.h"
#include "ConfigManager.h"
#include "SystemTray.h"
#include "SettingsDialog.h"
#include "FirstRunWizard.h"
#include "MeshDialog.h"
#include "PerfTimer.h"
#include "Tool.h"
#include "BrowserTool.h"
#include "GoogleSearchTool.h"
#include "WebSearchTool.h"
#include "YouTubeSearchTool.h"
#include "YouTubeVideoTool.h"
#include "SystemCommandTool.h"
#include "CalculatorTool.h"
#include "TimeTool.h"
#include "ClipboardTool.h"
#include "FileTool.h"
#include "FileExplorerTool.h"
#include "MusicPlayerTool.h"
#include "AddMediaDirectoryTool.h"
#include "RemoveMediaDirectoryTool.h"
#include "ListMediaLibraryTool.h"
#include "CameraCaptureTool.h"
#include <iostream>
#include <string>
#include <sstream>
#include <csignal>
#include <thread>
#include <chrono>
#include <filesystem>
#include <windows.h>
#include <fstream>

// Global flag for graceful shutdown
static volatile bool g_running = true;
static volatile bool g_restart = false;
static SystemTray* g_tray = nullptr;

// Output buffer for replay
static std::ostringstream g_output_buffer;

// Whether we're attached to a parent console (i.e. launched from cmd/powershell)
static bool g_attached_to_console = false;

// Tracks our own visibility. Hiding a console window with SW_HIDE only clears
// WS_VISIBLE: the conhost window survives, so the shell keeps its taskbar
// button and the console looks minimized instead of gone. The console is
// therefore detached with FreeConsole() to destroy the host, and re-created
// with AllocConsole() when it is toggled back on.
static bool g_console_visible = true;

// Custom stream buffer that writes to both original stream AND string buffer
class TeeStreamBuf : public std::streambuf {
public:
    TeeStreamBuf(std::streambuf* original, std::ostringstream& buffer)
        : original_(original), buffer_(buffer) {}

protected:
    virtual int overflow(int c) override {
        if (c != EOF) {
            buffer_.put(static_cast<char>(c));
            if (original_) original_->sputc(static_cast<char>(c));
        }
        return c;
    }
    virtual std::streamsize xsputn(const char* s, std::streamsize n) override {
        buffer_.write(s, n);
        if (original_) original_->sputn(s, n);
        return n;
    }
private:
    std::streambuf* original_;
    std::ostringstream& buffer_;
};

static TeeStreamBuf* g_tee_cout = nullptr;
static TeeStreamBuf* g_tee_cerr = nullptr;

static void init_output_buffering() {
    auto* orig_cout = std::cout.rdbuf();
    auto* orig_cerr = std::cerr.rdbuf();
    g_tee_cout = new TeeStreamBuf(orig_cout, g_output_buffer);
    g_tee_cerr = new TeeStreamBuf(orig_cerr, g_output_buffer);
    std::cout.rdbuf(g_tee_cout);
    std::cerr.rdbuf(g_tee_cerr);

    // Flush every write. When stdout is a terminal this is what makes the log
    // appear line by line instead of in 4 KB bursts, and when it is redirected
    // to a file (support logs, scripted runs) it is the difference between a
    // live log and one that only shows the first few kilobytes.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
}

// Console management
static void apply_console_codepage() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

static void show_console() {
    HWND consoleWnd = GetConsoleWindow();
    if (!consoleWnd) {
        return;
    }
    ShowWindow(consoleWnd, IsIconic(consoleWnd) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(consoleWnd);
    SetWindowPos(consoleWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    g_console_visible = true;
}

static void open_console() {
    bool allocated = false;
    if (!GetConsoleWindow() && AllocConsole()) {
        allocated = true;
        FILE* fDummy;
        freopen_s(&fDummy, "CONOUT$", "w", stdout);
        freopen_s(&fDummy, "CONOUT$", "w", stderr);
        freopen_s(&fDummy, "CONIN$", "r", stdin);
        std::cout.clear();
        std::cerr.clear();
        apply_console_codepage();

        // Disable the close button (X) so user can't accidentally close it
        HWND newConsoleWnd = GetConsoleWindow();
        HMENU hMenu = newConsoleWnd ? GetSystemMenu(newConsoleWnd, FALSE) : nullptr;
        if (hMenu) {
            EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
    }

    show_console();

    // A freshly allocated console starts empty, so replay what was buffered
    // while it was detached. The buffer is dropped afterwards, otherwise every
    // toggle would reprint the whole session.
    if (allocated) {
        std::string buffered = g_output_buffer.str();
        g_output_buffer.str(std::string());
        g_output_buffer.clear();
        if (!buffered.empty()) {
            std::cout << buffered;
            std::cout.flush();
        }
    }
}

static void hide_console() {
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        // Detaching tears down the conhost window, so the console leaves the
        // taskbar completely. Output keeps accumulating in g_output_buffer and
        // is replayed by open_console() when the console comes back. When
        // launched from a terminal this also leaves the launching shell alone
        // instead of hiding the user's window.
        if (!FreeConsole()) {
            ShowWindow(consoleWnd, SW_HIDE);
        }
    }
    g_console_visible = false;
}

static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nShutting down..." << std::endl;
        g_running = false;
    }
}

void on_exit_request() {
    std::cout << "Exit requested from tray menu" << std::endl;
    g_running = false;
}

void on_settings_restart() {
    std::cout << "Restart requested from settings" << std::endl;
    g_running = false;
    g_restart = true;
}

#include "CoreUtil.h"

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    SetErrorMode(SEM_FAILCRITICALERRORS);

    apply_console_codepage();

    // Check if we were launched from a parent console (cmd/powershell/bash)
    // by trying to attach to it. If successful, we keep the console.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Successfully attached to parent console - launched from terminal
        g_attached_to_console = true;
        std::cout.sync_with_stdio(true);
        std::cerr.sync_with_stdio(true);
    }
    // Standalone launch: the /SUBSYSTEM:CONSOLE linker flag ensures a console window
    // is provided. Keep it visible so users can see telemetry and log output.

    // Set up output buffering (captures ALL output for later replay)
    init_output_buffering();

    // Set console control handler for safety
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    std::cout << "========================================" << std::endl;
    std::cout << "  Personal Assistant" << std::endl;
    std::cout << "  Powered by STTEngine + Llama + Kokoro" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Load configuration from file
    AppConfig config;
    bool first_run = ConfigManager::is_first_run();
    if (first_run) {
        if (!FirstRunWizard::show_dialog(nullptr, config)) {
            // User skipped, use defaults
        }
        ConfigManager::save_config(config, ConfigManager::get_config_path());
    }
    ConfigManager::load_config(config, ConfigManager::get_config_path());

    PerfTimer::set_debug_mode(config.debug_mode);

    char module_dir[MAX_PATH];
    GetModuleFileNameA(nullptr, module_dir, MAX_PATH);
    std::string module_path_str(module_dir);
    size_t last_slash = module_path_str.find_last_of("\\/");
    std::string module_directory = module_path_str.substr(0, last_slash);
    
    std::cout << "Module directory: " << module_directory << std::endl;
    
    SetCurrentDirectoryA(module_directory.c_str());
    
    std::string llama_model_path = config.llama_model_path;
    
    std::string llama_server_path;
    
    std::string tts_model_dir, avatar_model_path, videos_path, sounds_path;
    
    // ---------------------------------------------------------------------------
    // Resource base resolution.
    //   Development: running the built app from a build output directory (e.g.
    //                build\Release) - reference assets in the project root, which
    //                sits two levels above the module (executable) directory.
    //   Production : installed app - the installer bundles assets next to the exe.
    // ---------------------------------------------------------------------------
    bool is_production = false;
    std::filesystem::path module_dir_path(module_directory);
    std::filesystem::path project_root = (module_dir_path / ".." / "..").lexically_normal();

    if (std::filesystem::exists(module_dir_path / "models" / "silero_vad.onnx")) {
        // Assets are bundled alongside the executable (installed application or build output).
        is_production = true;
        std::cout << "Resource mode: production (assets bundled next to executable)" << std::endl;
    } else if (std::filesystem::exists(project_root / "CMakeLists.txt") &&
               (std::filesystem::exists(project_root / "resources" / "models" / "silero_vad.onnx"))) {
        // Running the built application from a build output dir (development).
        is_production = false;
        std::cout << "Resource mode: development (assets referenced from project root: "
                  << project_root.string() << ")" << std::endl;
    } else {
        // Unknown layout - fall back to the executable directory.
        project_root = module_dir_path;
        std::cout << "Resource mode: fallback (using executable directory)" << std::endl;
    }

    std::filesystem::path resource_base = is_production ? module_dir_path : project_root;

    // In production/build mode, assets are copied without the resources/ prefix
    // In development mode, assets are under resources/
    std::filesystem::path models_dir;
    std::filesystem::path avatars_dir;
    std::filesystem::path videos_dir;
    std::filesystem::path sounds_dir;

    if (is_production) {
        // Production/build mode: assets are directly in module directory
        models_dir = resource_base / "models";
        avatars_dir = resource_base / "avatars";
        videos_dir = resource_base / "videos";
        sounds_dir = resource_base / "sounds";
    } else {
        // Development mode: assets are under resources/
        models_dir = resource_base / "resources" / "models";
        avatars_dir = resource_base / "resources" / "avatars";
        videos_dir = resource_base / "resources" / "videos";
        sounds_dir = resource_base / "resources" / "sounds";
    }

    // Model paths - STT stack (CTranslate2 Whisper base + openWakeWord + Silero VAD).
    std::string models_base = models_dir.string() + "\\";
    std::string stt_silero_vad = (models_dir / "silero_vad.onnx").string();
    std::filesystem::path ct2_model_dir = models_dir / "whisper-base-ct2";

    LOG_STT("Active backend: CTranslate2 Whisper base + openWakeWord (jarvis_ears parity)");
    LOG_STT("CT2 model directory: " + ct2_model_dir.string());

    // TTS / avatar / video / sound assets.
    // Kokoro models location
    std::filesystem::path kokoro_dir;
    if (std::filesystem::exists(resource_base / "kokoro")) {
        kokoro_dir = resource_base / "kokoro";
    } else if (std::filesystem::exists(models_dir / "kokoro")) {
        kokoro_dir = models_dir / "kokoro";
    } else {
        std::cerr << "ERROR: Kokoro models directory not found" << std::endl;
        kokoro_dir = models_dir / "kokoro"; // fallback
    }
    tts_model_dir = kokoro_dir.string();
    avatar_model_path = config.avatar_type == "iron-man"
        ? (avatars_dir / "iron-man.glb").string()
        : (avatars_dir / "sonny.glb").string();
    videos_path = videos_dir.string() + "\\";
    sounds_path = sounds_dir.string() + "\\";
    
    if (llama_model_path.empty() || GetFileAttributesA(llama_model_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        llama_model_path.clear();
        MessageBox(nullptr, 
            "Llama model is not configured or the file was not found.\n\nPlease click OK and browse to select your Llama model file (.gguf or .ggml).", 
            "Model Required", MB_OK | MB_ICONERROR);
        SettingsDialog::set_restart_callback(nullptr);
        if (SettingsDialog::show_dialog(nullptr, config)) {
            ConfigManager::save_config(config, ConfigManager::get_config_path());
            llama_model_path = config.llama_model_path;
        }
        if (llama_model_path.empty() || GetFileAttributesA(llama_model_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBox(nullptr, "Llama model is still not configured or the file does not exist.", "Configuration Error", MB_OK | MB_ICONERROR);
            return 1;
        }
    }
    
    std::cout << "Using Native STTEngine & RAG Engine in C++" << std::endl;

    if (argc > 1) llama_model_path = argv[1];
    if (argc > 2) llama_server_path = argv[2];
    if (argc > 3) tts_model_dir = argv[3];
    if (argc > 4) avatar_model_path = argv[4];

    // Pre-flight the model GGUF before the (slow) GPU/model init. An
    // architecture the bundled llama.cpp does not implement - Ollama's
    // llama3.2-vision (mllama) is the common case - is explained here, with the
    // chance to pick another file, instead of ending in a bare "Failed to
    // initialize Llama" on a console a tray launch never shows.
    {
        LlamaWrapper::ModelFileInfo model_info = LlamaWrapper::inspect_model_file(llama_model_path);
        if (model_info.unusable) {
            std::cerr << "Model cannot be loaded by this build:" << std::endl;
            std::cerr << model_info.problem << std::endl;
            std::string dialog_text = "This model cannot be loaded by the bundled llama.cpp:\n\n"
                + model_info.problem + "\n\nClick OK to select a different model file.";
            MessageBoxA(nullptr, dialog_text.c_str(), "Unsupported Model", MB_OK | MB_ICONERROR);
            SettingsDialog::set_restart_callback(nullptr);
            if (SettingsDialog::show_dialog(nullptr, config)) {
                ConfigManager::save_config(config, ConfigManager::get_config_path());
                llama_model_path = config.llama_model_path;
            }
            LlamaWrapper::ModelFileInfo retry_info = llama_model_path.empty()
                ? model_info
                : LlamaWrapper::inspect_model_file(llama_model_path);
            if (llama_model_path.empty() ||
                GetFileAttributesA(llama_model_path.c_str()) == INVALID_FILE_ATTRIBUTES ||
                retry_info.unusable) {
                MessageBoxA(nullptr, "No loadable model was selected - Sonny cannot start.\n\n"
                    "Pick a GGUF whose architecture this build implements (a vision model also "
                    "needs its mmproj-*.gguf projector next to it).",
                    "Configuration Error", MB_OK | MB_ICONERROR);
                return 1;
            }
        } else if (!model_info.problem.empty()) {
            std::cout << model_info.problem << std::endl;
        }
    }

    // Auto-detect the mmproj (multimodal projector / vision encoder) file.
    // Vision-capable models (e.g. Qwen2.5-VL, Qwen3-VL, Gemma 3) ship with a
    // companion mmproj GGUF that sits next to the main model. Search the model's
    // directory first, then the global models dir, matching common naming
    // patterns. If found, it is passed to LlamaWrapper::initialize() which
    // turns on image input routing automatically — no config toggle needed.
    std::string mmproj_path;
    std::filesystem::path model_fs(llama_model_path);
    std::filesystem::path model_dir = model_fs.parent_path();

    // Look for mmproj files: common names are mmproj-*.gguf, mmproj_model_f*.gguf,
    // and various model-specific naming. Check alongside the model first.
    if (std::filesystem::exists(model_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(model_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            std::string fn_lower = fn;
            std::transform(fn_lower.begin(), fn_lower.end(), fn_lower.begin(), ::tolower);
            if ((fn_lower.find("mmproj") != std::string::npos || fn_lower.find("vision") != std::string::npos)
                && fn_lower.find(".gguf") != std::string::npos) {
                mmproj_path = entry.path().string();
                break;
            }
        }
    }
    // Fall back to the models directory.
    if (mmproj_path.empty() && std::filesystem::exists(models_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(models_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            std::string fn_lower = fn;
            std::transform(fn_lower.begin(), fn_lower.end(), fn_lower.begin(), ::tolower);
            if ((fn_lower.find("mmproj") != std::string::npos || fn_lower.find("vision") != std::string::npos)
                && fn_lower.find(".gguf") != std::string::npos) {
                mmproj_path = entry.path().string();
                break;
            }
        }
    }
    if (!mmproj_path.empty()) {
        std::cout << "Vision mmproj detected: " << mmproj_path << std::endl;
    } else {
        std::cout << "No mmproj (vision) file detected - vision features disabled" << std::endl;
    }

    AssistantOrchestrator assistant;
    assistant.set_avatar_path(avatar_model_path);
    assistant.set_show_avatar(config.show_avatar);
    assistant.set_voice_name(config.kokoro_voice);
    assistant.set_language(config.language);
    assistant.set_tts_speed(config.tts_speed);
    // Kokoro TTS runs in-process now (KokoroWrapper links the kokoro static
    // library directly) - no separate kokoro-server.exe process is needed.
    // The TTS model directory is passed to initialize() below.
    // Set native STT model paths on the assistant.
    assistant.set_stt_paths(stt_silero_vad, ct2_model_dir.string());
    std::filesystem::path wake_models_dir = models_dir / "openwakeword";
    assistant.set_wake_word_models(
        (wake_models_dir / "sonny.onnx").string(),
        (wake_models_dir / "melspectrogram.onnx").string(),
        (wake_models_dir / "embedding_model.onnx").string());
    assistant.set_language(config.language);
    
    // Use the CTranslate2 Whisper path directly with Silero VAD.
    assistant.set_model_type("whisper-ct2");

    SystemTray tray;
    g_tray = &tray;
    tray.initialize(GetModuleHandle(nullptr), "Sonny Assistant");
    
    tray.set_exit_callback(on_exit_request);
    tray.set_console_visible_callback([]() { return g_console_visible; });
    tray.set_console_callback([]() {
        HWND consoleWnd = GetConsoleWindow();
        if (!consoleWnd) {
            // No console exists - create one
            open_console();
        } else if (g_console_visible) {
            // Console is visible - hide it completely (no taskbar entry left)
            hide_console();
        } else {
            // Console exists but hidden - show it again
            show_console();
        }
        if (!g_attached_to_console) {
            std::cout << "\n========================================" << std::endl;
            std::cout << "  Console window toggled from tray menu" << std::endl;
            std::cout << "  Close button is disabled - use tray menu to toggle" << std::endl;
            std::cout << "========================================" << std::endl;
        }
    });
    tray.set_mesh_callback([&]() {
        MeshDialog::show_dialog(nullptr, [&]() -> std::string {
            return assistant.mesh_json();
        });
    });
    tray.set_settings_callback([&](AppConfig& new_config) {
        config = new_config;
        assistant.set_voice_name(config.kokoro_voice);
        assistant.set_system_prompt(config.system_prompt);
        assistant.set_use_browser_stt(config.use_browser_stt);
        assistant.set_use_browser_tts(config.use_browser_tts);
        assistant.set_browser_voice(config.browser_voice);
        assistant.set_wake_word_enabled(config.wake_word_enabled);
        assistant.set_cava_visualizer_wake_word_enabled(config.wake_word_enabled);
        assistant.set_mic_gain(config.mic_gain);
        assistant.set_camera_feed_config(config.camera_feed_enabled, config.camera_device_name);
        std::cout << "Settings updated from tray menu" << std::endl;
    });
    
    SettingsDialog::set_restart_callback(on_settings_restart);
    
    Jarvis::ToolRegistry& registry = Jarvis::ToolRegistry::getInstance();
    registry.registerTool("browser", std::make_unique<Jarvis::BrowserTool>());

    registry.registerTool("google_search", std::make_unique<Jarvis::GoogleSearchTool>());
    registry.registerTool("web_search", std::make_unique<Jarvis::WebSearchTool>());
    registry.registerTool("youtube_search", std::make_unique<Jarvis::YouTubeSearchTool>());
    registry.registerTool("youtube_video", std::make_unique<Jarvis::YouTubeVideoTool>());
    registry.registerTool("system_command", std::make_unique<Jarvis::SystemCommandTool>());
    registry.registerTool("calculator", std::make_unique<Jarvis::CalculatorTool>());
    registry.registerTool("get_time", std::make_unique<Jarvis::TimeTool>());
    registry.registerTool("clipboard", std::make_unique<Jarvis::ClipboardTool>());
    registry.registerTool("file_operation", std::make_unique<Jarvis::FileTool>());
    registry.registerTool("file_explorer", std::make_unique<Jarvis::FileExplorerTool>());
    registry.registerTool("music_player", std::make_unique<Jarvis::MusicPlayerTool>());
    registry.registerTool("add_media_directory", std::make_unique<Jarvis::AddMediaDirectoryTool>());
    registry.registerTool("remove_media_directory", std::make_unique<Jarvis::RemoveMediaDirectoryTool>());
    registry.registerTool("list_media_library", std::make_unique<Jarvis::ListMediaLibraryTool>());
    registry.registerTool("camera", std::make_unique<Jarvis::CameraCaptureTool>());
    
    std::cout << "Registered " << registry.getAllToolNames().size() << " tools" << std::endl;

    assistant.set_use_browser_stt(config.use_browser_stt);
    assistant.set_use_browser_tts(config.use_browser_tts);
    assistant.set_browser_voice(config.browser_voice);
    assistant.set_system_prompt(config.system_prompt);
    assistant.set_voice_name(config.kokoro_voice);
    assistant.set_wake_word_enabled(config.wake_word_enabled);
    assistant.set_cava_visualizer_enabled(config.enable_cava_visualizer);
    assistant.set_cava_visualizer_wake_word_enabled(config.wake_word_enabled);
    // Enable tools BEFORE initialize(): the orchestrator pushes the final
    // (tool-schemas) system prompt to the LLM pre-init, so warmup() seeds the
    // KV prefix cache with the exact production prompt.
    assistant.enable_tools(true);

    // Retrieval budget + the opt-in peer metrics network. Both are plain config
    // values; the vectors does nothing unless explicitly enabled.
    assistant.set_rag_max_context_chars(config.rag_max_context_chars);
    {
        Jarvis::Vectors::VectorsConfig vectors;
        vectors.enabled = config.vectors_enabled;
        vectors.mode = config.vectors_mode;
        vectors.room = config.vectors_room;
        vectors.room_secret = config.vectors_room_secret;
        vectors.rendezvous_url = config.vectors_rendezvous_url;
        vectors.share_tool_metrics = config.vectors_share_tool_metrics;
        vectors.k_anonymity = config.vectors_k_anonymity;
        vectors.dp_sigma = config.vectors_dp_sigma;
        assistant.set_vectors_config(vectors);
    }

    if (!assistant.initialize(llama_model_path, llama_server_path, tts_model_dir, "", config.rag_embedding_model, videos_path, sounds_path, mmproj_path)) {
        std::cerr << "Failed to initialize assistant" << std::endl;
        return 1;
    }

    // Initialize camera feed from settings (vision is always enabled when camera is available)
    assistant.set_camera_feed_config(config.camera_feed_enabled, config.camera_device_name);

    // Tools can now reach the RAG knowledge base (registered media
    // libraries, learned facts, ...).
    registry.setRagEngine(assistant.get_rag_engine());

    std::cout << std::endl;
    std::cout << "Assistant initialized successfully!" << std::endl;
    std::cout << std::endl;
    
    // (enable_tools moved before assistant.initialize above so the warmup
    // prefill seeds the tool-schemas system prompt.)

    if (!assistant.start([](const std::string& status) {
        LOG_STATUS(status);
    })) {
       std::cerr << "Failed to start assistant" << std::endl;
        return 1;
    }

    std::thread console_input_thread;
    console_input_thread = std::thread([&assistant]() {
        std::string line;
        while (assistant.is_running()) {
            if (std::getline(std::cin, line)) {
                if (!line.empty()) {
                    assistant.process_console_input(line);
                }
            } else {
                // Hiding the console detaches it, which ends the pending read
                // on CONIN$. Retry so typing works again after it is restored.
                std::cin.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    });

    // Start without a console window. Only for a standalone launch: when the
    // app was started from a terminal, that terminal is the console the user
    // is watching and must not be detached underneath them. Everything printed
    // so far sits in g_output_buffer and is replayed by open_console().
    if (!g_attached_to_console) {
        hide_console();
    }

    while (g_running) {
        tray.process_messages();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assistant.stop();
    tray.shutdown();

    if (console_input_thread.joinable()) {
        console_input_thread.join();
    }
    
    // Clean up output buffering
    delete g_tee_cout;
    delete g_tee_cerr;
    g_tee_cout = nullptr;
    g_tee_cerr = nullptr;
    
    // Close freopen handles if we allocated a console
    if (!g_attached_to_console) {
        fclose(stdout);
        fclose(stderr);
        fclose(stdin);
    }
    
    std::cout << "Assistant shutdown complete." << std::endl;
    
    if (g_restart) {
        char module_path[MAX_PATH];
        GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        if (CreateProcessA(module_path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    
    return 0;
}
