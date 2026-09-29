#include "AssistantOrchestrator.h"
#include "Tool.h"
#include "FileExplorerTool.h"
#include "PerfTimer.h"
#include "Logger.h"
#include "CoreUtil.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <regex>
#include <sstream>
#include <filesystem>
#include <system_error>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <random>
#include <iomanip>
#include <algorithm>
#include <cmath>

// Forward declarations for static helpers defined later in this file.
static size_t find_sentence_split(const std::string& buffer, size_t search_from);
static std::string json_unescape(const std::string& input);

#ifndef HINST_THISCOMPONENT
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)
#endif

AssistantOrchestrator::AssistantOrchestrator() 
    : running_(false), processing_(false), audio_processing_thread_active_(false) {
    stt_engine_ = std::make_unique<STTEngineWrapper>();
    web_speech_ = std::make_unique<WebSpeechWrapper>();
    llama_ = std::make_unique<LlamaWrapper>();
    kokoro_ = std::make_unique<KokoroWrapper>();
    audio_capture_ = std::make_unique<AudioCapture>();
    audio_playback_ = std::make_unique<AudioPlayback>();
    video_player_ = std::make_unique<VideoPlayer>();
    rag_engine_ = std::make_unique<Jarvis::RagEngine>();
    cava_visualizer_ = std::make_unique<Jarvis::CavaVisualizer>(32, 16000);
    
    // Generate unique session ID
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::mt19937 rng(static_cast<unsigned int>(now_ms));
    std::uniform_int_distribution<int> dist(1000, 9999);
    std::stringstream ss;
    ss << "session_" << now_ms << "_" << dist(rng);
    session_id_ = ss.str();
    
    system_prompt_ =
        "You are Sonny, a fast and highly capable AI personal assistant. "
        "You speak directly and concisely â€” no filler phrases like 'Certainly!', 'Of course!', "
        "'Great question!', 'Absolutely!', or 'Sure thing!'. Never start a response with those. "
        "Give spoken-style answers: natural, warm, confident, and brief. "
        "Never use bullet points, numbered lists, markdown, or formatting symbols in your responses. "
        "For simple questions answer in one or two sentences. Only elaborate when the user explicitly asks. "
        "Do not repeat the user's question back to them. Do not hedge unnecessarily. "
        "When using tools, respond with only the JSON tool call and nothing else. "
        "After receiving tool results, give a natural spoken summary â€” not raw data. "
        "If you do not know something, say so briefly and move on.";
}

AssistantOrchestrator::~AssistantOrchestrator() {
    stop();
}

bool AssistantOrchestrator::initialize(
    const std::string& llama_model_path,
    const std::string& llama_server_path,
    const std::string& tts_model_dir,
    const std::string& stt_script_path,
    const std::string& rag_embedding_model_path,
    const std::string& videos_base_path,
    const std::string& sounds_base_path,
    const std::string& mmproj_path
) {
    // Store base paths for resources
    videos_base_path_ = videos_base_path.empty() ? "..\\..\\videos\\" : videos_base_path;
    sounds_base_path_ = sounds_base_path.empty() ? "..\\..\\sounds\\" : sounds_base_path;
    rag_embedding_model_path_ = rag_embedding_model_path.empty() ? "all-MiniLM-L6-v2"
                                                                : rag_embedding_model_path;
    
    std::cout << "Initializing Personal Assistant..." << std::endl;

    // Initialize VideoPlayer and play loading animation
    if (video_player_) {
        if (video_player_->initialize()) {
            video_player_ready_ = true;

            // Get actual video dimensions
            int video_w = 0, video_h = 0;
            std::string loading_video = videos_base_path_ + "logo-loading-green.wmv";
            if (video_player_->get_video_dimensions(loading_video, video_w, video_h)) {
                LOG_VIDEOPLAYER("Loading video dimensions: " + std::to_string(video_w) + "x" + std::to_string(video_h));
            } else {
                // Fallback to default size if unable to get dimensions
                const int screen_w = GetSystemMetrics(SM_CXSCREEN);
                const int screen_h = GetSystemMetrics(SM_CYSCREEN);
                video_w = std::min(screen_w, screen_h) / 4;
                video_h = video_w;
            }

            // Center the video on screen
            const int screen_w = GetSystemMetrics(SM_CXSCREEN);
            const int screen_h = GetSystemMetrics(SM_CYSCREEN);
            const int x = (screen_w - video_w) / 2;
            const int y = (screen_h - video_h) / 2;

            video_player_->set_position(x, y, video_w, video_h);
            // Play the loading video (loop until initialization is complete) with chroma key enabled
            video_player_->play_file(loading_video, false, true, true);
            LOG_VIDEOPLAYER("Playing loading animation");
        } else {
            LOG_ERROR("VideoPlayer", "Failed to initialize");
            video_player_.reset();
        }
    }
    

    // Initialize Web Speech API server if browser STT or TTS is enabled
    if (use_browser_stt_ || use_browser_tts_) {
        std::cout << "Using Chrome Web Speech API for STT/TTS (browser-based)" << std::endl;
        if (!web_speech_->initialize("resources/web/web_speech.html")) {
            std::cerr << "Failed to initialize Web Speech API" << std::endl;
            if (use_browser_stt_) use_browser_stt_ = false;
            if (use_browser_tts_) use_browser_tts_ = false;
        } else {
            if (!web_speech_->start()) {
                std::cerr << "Failed to start Web Speech API server" << std::endl;
                if (use_browser_stt_) use_browser_stt_ = false;
                if (use_browser_tts_) use_browser_tts_ = false;
            } else {
                std::cout << "Web Speech API server started successfully on port " << web_speech_->get_server_port() << std::endl;
                std::cout << "Open http://localhost:" << web_speech_->get_server_port() << "/?mode=api in Chrome to enable speech recognition" << std::endl;
            }
        }
    }

    // Initialize STT based on config (parallel with Llama if not using browser STT)
    std::thread stt_thread;
    if (!use_browser_stt_) {
        stt_thread = std::thread([this, &stt_script_path]() {
            std::cout << "Using native C++ STT engine" << std::endl;

            if (!stt_engine_->initialize()) {
                std::cerr << "Failed to initialize STTEngine" << std::endl;
                return;
            }

            // Wake sound (jarvis parity). AEC: the confirmation tone
            // (sonny_yes.wav, ~1.3 s) must not leak into the 4 s command
            // recording - otherwise the mic hears the tone and whisper
            // transcribes it into the command text (and the spectrum canvas
            // shows the assistant's own voice as input). Mute the capture
            // while the tone plays and use SND_SYNC so the tone cannot
            // overlap the recording start; the STT processing thread blocks
            // only for the tone duration, which is harmless (it is not
            // recording and the wake model was already reset).
            stt_engine_->set_wake_word_callback([this]() {
                const std::string wake_sound_path = sounds_base_path_ + "sonny_yes.wav";
                LOG_WAKE("Playing wake sound: " + wake_sound_path);
                if (audio_capture_) audio_capture_->set_muted(true);
                PlaySoundA(wake_sound_path.c_str(), nullptr, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
                if (audio_capture_) audio_capture_->set_muted(false);
            });

            if (!stt_silero_vad_.empty() && !stt_ct2_model_dir_.empty()) {
                stt_engine_->set_model_paths(stt_silero_vad_, stt_ct2_model_dir_);
            }
            if (!stt_wakeword_model_.empty()) {
                stt_engine_->set_wake_word_models(stt_wakeword_model_, stt_melspec_model_, stt_embedding_model_);
            }
            stt_engine_->set_model_type(stt_model_type_);
            stt_engine_->set_language(language_);
            stt_engine_->set_wake_word_enabled(wake_word_enabled_);

            auto stt_load_start = std::chrono::high_resolution_clock::now();
            if (!stt_engine_->start()) {
                std::cerr << "Failed to start STTEngine" << std::endl;
                return;
            }
            auto stt_load_end = std::chrono::high_resolution_clock::now();
            auto stt_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stt_load_end - stt_load_start).count();
            LOG_DEBUG("TELEMETRY", "STT_LOAD: " + std::to_string(stt_load_ms) + " ms");
            std::cout << "Native C++ STT started successfully" << std::endl;
        });
    }

    // Initialize Llama (can run in parallel with STT)
    std::cout << "Loading Llama model from: " << llama_model_path << std::endl;
    std::cout << "LLM server path: " << llama_server_path << std::endl;
    auto llm_load_start = std::chrono::high_resolution_clock::now();
    // Push the FINAL system prompt (persona + tool schemas) into LlamaWrapper
    // BEFORE llama init: warmup() then prefills exactly this production
    // prefix and keeps it in the KV cache, so even the first user turn skips
    // the ~2k-token system-prompt prefill. (Tools are registered in main()
    // before AssistantOrchestrator::initialize() is called.)
    refresh_llama_system_prompt(tools_enabled_);
    if (!llama_->initialize(llama_model_path, llama_server_path, mmproj_path)) {
        std::cerr << "Failed to initialize Llama" << std::endl;
        return false;
    }
    auto llm_load_end = std::chrono::high_resolution_clock::now();
    auto llm_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(llm_load_end - llm_load_start).count();
    LOG_DEBUG("TELEMETRY", "LLM_LOAD: " + std::to_string(llm_load_ms) + " ms");
    std::cout << "Llama initialized successfully" << std::endl;
    if (llama_->has_vision()) {
        std::cout << "Vision/multimodal support enabled" << std::endl;
    }
    
    // Initialize camera capture (for vision queries when camera feed is toggled on)
    camera_capture_ = std::make_unique<Jarvis::CameraCapture>();
    camera_capture_->initialize();

    // Stop loading animation since LLM is now ready
    if (video_player_ready_) {
        video_player_->stop();
        LOG_VIDEOPLAYER("Loading animation stopped - LLM initialized");
    }

    // Wait for STT initialization to complete if it was started in parallel
    if (stt_thread.joinable()) {
        stt_thread.join();
    }

    // Initialize WASAPI AudioCapture (feeds the native C++ STT engine).
    if (!use_browser_stt_) {
        if (!audio_capture_->initialize(16000)) {
            std::cerr << "Failed to initialize audio capture" << std::endl;
            return false;
        }
        audio_capture_->set_voice_gate_enabled(false);
        audio_capture_->set_mic_gain(mic_gain_);
        std::cout << "Audio capture initialized successfully (mic_gain=" << mic_gain_ << ")" << std::endl;
    } else {
        std::cout << "Audio capture disabled (using browser STT)" << std::endl;
    }
    if (!use_browser_tts_) {
        std::cout << "Initializing in-process Kokoro TTS with models from: " << tts_model_dir << std::endl;
        auto tts_load_start = std::chrono::high_resolution_clock::now();
        if (!kokoro_->initialize(tts_model_dir)) {
            std::cerr << "Failed to initialize Kokoro TTS - TTS will be disabled" << std::endl;
            // Don't return false - allow the application to continue without TTS
            // User can use browser TTS as fallback
            std::cout << "Note: You can enable browser TTS in settings if Kokoro models are not available" << std::endl;
        } else {
            auto tts_load_end = std::chrono::high_resolution_clock::now();
            auto tts_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tts_load_end - tts_load_start).count();
            LOG_DEBUG("TELEMETRY", "TTS_LOAD: " + std::to_string(tts_load_ms) + " ms");
            kokoro_->set_language(language_);
            std::cout << "Kokoro TTS initialized successfully (in-process)" << std::endl;
        }
    } else {
        std::cout << "Skipping TTS initialization (using browser TTS)" << std::endl;
    }

    // Initialize Audio Playback
    if (!audio_playback_->initialize(24000)) {
        std::cerr << "Failed to initialize audio playback" << std::endl;
        return false;
    }
    std::cout << "Audio playback initialized successfully" << std::endl;

    // Play start logo animation alongside sonny sound to indicate initialization is complete
    if (video_player_ready_) {
        // Get actual video dimensions for start video
        int start_video_w = 0, start_video_h = 0;
        std::string start_video = videos_base_path_ + "logo-start-green.wmv";
        if (video_player_->get_video_dimensions(start_video, start_video_w, start_video_h)) {
            LOG_VIDEOPLAYER("Start video dimensions: " + std::to_string(start_video_w) + "x" + std::to_string(start_video_h));
        } else {
            // Fallback to default size if unable to get dimensions
            const int screen_w = GetSystemMetrics(SM_CXSCREEN);
            const int screen_h = GetSystemMetrics(SM_CYSCREEN);
            start_video_w = std::min(screen_w, screen_h) / 4;
            start_video_h = start_video_w;
        }

        // Center the start video on screen
        const int screen_w = GetSystemMetrics(SM_CXSCREEN);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screen_w - start_video_w) / 2;
        const int y = (screen_h - start_video_h) / 2;

        video_player_->set_position(x, y, start_video_w, start_video_h);
        video_player_->play_file(start_video, false, false, true);
        LOG_VIDEOPLAYER("Playing start animation");
    }

    // Play sonny sound to indicate initialization is complete but model may still be loading
    play_file_with_aec(sounds_base_path_ + "sonny.wav");
    LOG_AUDIO("Sonny sound played");

    // Keep the loading animation playing until LLM model is ready
    // The loading animation will be stopped when the LLM model is actually ready to respond

    // Initialize Avatar Overlay (if enabled)
    if (show_avatar_) {
        avatar_ = std::make_unique<AvatarOverlay>();
        std::string avatar_file = avatar_path_.empty() ? "resources/avatars/sonny.glb" : avatar_path_;
        if (avatar_->initialize(HINST_THISCOMPONENT, avatar_file)) {
            std::cout << "Avatar overlay initialized successfully" << std::endl;
            avatar_->show();
            avatar_->start();
        } else {
            std::cerr << "Failed to initialize avatar overlay (non-fatal)" << std::endl;
            avatar_.reset();
        }
    } else {
        std::cout << "Avatar overlay disabled (show_avatar = false)" << std::endl;
    }

    // Open browser to Web Speech API URL at the end of initialization if Web Speech is enabled
    if (use_browser_stt_ || use_browser_tts_) {
        std::string url = "http://localhost:" + std::to_string(web_speech_->get_server_port()) + "/?mode=api";
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
        std::cout << "Opened browser to: " << url << std::endl;
    }

    // Initialize RAG Engine
    if (rag_engine_) {
        LOG_RAG("Initializing RAG Engine...");

        // The embedding model path used to be dropped on the floor here, which
        // is why the RAG engine never had dense vectors: it was always called
        // with its default (empty) argument list.
        if (rag_engine_->initialize("", "", rag_embedding_model_path_)) {
            LOG_RAG("Native C++ RAG Engine initialized successfully!");
            rag_engine_->set_max_context_chars(rag_max_context_chars_);
            LOG_RAG("Retrieval: " + rag_engine_->embedding_status());
        } else {
            std::cerr << "[RAG] Failed to initialize RAG Engine (non-fatal, continuing without RAG)" << std::endl;
            rag_enabled_ = false;
        }
    }

    // Peer metrics network (opt-in, content-free). Failure is never fatal: the
    // assistant works exactly the same without it.
    vectors_ = std::make_unique<Jarvis::Vectors::VectorsSync>();
    vectors_->initialize(vectors_config_);
    if (vectors_config_.enabled && vectors_config_.mode != "off") {
        if (!vectors_->start()) {
            LOG_WARN("Vectors", "Peer network did not start - continuing standalone");
        }
    }
    if (vectors_ && rag_engine_ && rag_engine_->isReady()) {
        vectors_->set_corpus_scale(rag_engine_->document_chunk_count(),
                                         rag_engine_->user_fact_count(),
                                         rag_engine_->has_semantic_search());

        // Publish the RAG engine's local term frequencies to the vectors
        // so the room gets a lexical topic-trend signal. Content-free: the
        // encoder drops rare terms and noises the counts before publication.
        vectors_->set_term_frequencies(rag_engine_->term_frequencies());

        // Publish local embedding vectors (client-side DP-protected) so
        // peers can aggregate them for cross-node semantic search.
        if (rag_engine_->has_semantic_search()) {
            const std::vector<std::vector<float>> local_vectors =
                rag_engine_->local_embedding_vectors();
            if (!local_vectors.empty()) {
                vectors_->set_embedding_vectors(local_vectors,
                                                      rag_engine_->embedding_dimension());
            }
        }

    // Consume aggregated cross-node embeddings from the vectors
    // to expand the RAG engine's semantic search space.
    const std::vector<std::vector<float>> peer_vectors =
        vectors_->aggregated_embeddings();
    if (!peer_vectors.empty()) {
        rag_engine_->set_cross_node_embeddings(peer_vectors);
        LOG_RAG("Cross-node embeddings received: " +
                std::to_string(peer_vectors.size()) + " vector(s) from peers");
    }

    // Consume aggregated LoRA adapters from the vectors to personalize
    // the local LLM. This is the FL-for-LLMs approach from Chapter 15 of
    // "Federated Learning Foundations and Applications": each node trains
    // a LoRA adapter on its local data, shares only the adapter deltas
    // (~100 KB-2 MB), and the vectors aggregates them by name with a
    // Byzantine-robust mode vote.
    apply_peer_lora_adapters();
    }

    // Log GPU backend
    // (reporting lives in LlamaWrapper, which asks the ggml backend registry -
    //  a GetModuleHandle-based check here would always report CPU because the
    //  CUDA/Vulkan backends are statically linked into the executable)

    // Log memory usage
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        SIZE_T working_set_kb = pmc.WorkingSetSize / 1024;
        SIZE_T peak_working_set_kb = pmc.PeakWorkingSetSize / 1024;
        LOG_DEBUG("TELEMETRY", "MEMORY: WorkingSet=" + std::to_string(working_set_kb) + " KB, PeakWorkingSet=" + std::to_string(peak_working_set_kb) + " KB");
    }

    return true;
}

bool AssistantOrchestrator::publish_lora_adapter(const std::string& name, const std::string& adapter_path) {
    if (!vectors_ || !llama_ || !llama_->is_initialized()) return false;
    if (name.empty() || name.size() > 128) return false;
    if (adapter_path.empty()) return false;

    // Read the adapter file. Cap at 2 MB: a LoRA adapter for a 3B model is
    // ~100 KB-2 MB. A larger payload is rejected as malformed.
    std::ifstream file(adapter_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_WARN("Vectors", "LoRA: adapter file not found: " + adapter_path);
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size > 2 * 1024 * 1024) {
        LOG_WARN("Vectors", "LoRA: adapter file too large or empty: " + adapter_path);
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(size), '\0');
    file.read(&bytes[0], size);
    file.close();

    // Base64-encode for transport.
    static const char kBase64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned int triple = 0;
        int pad = 0;
        for (int j = 0; j < 3; ++j) {
            triple <<= 8;
            if (i + j < bytes.size()) {
                triple |= static_cast<unsigned char>(bytes[i + j]);
            } else {
                ++pad;
            }
        }
        for (int j = 0; j < 4 - pad; ++j) {
            encoded += kBase64Chars[(triple >> (6 * (3 - j))) & 0x3F];
        }
        for (int j = 0; j < pad; ++j) encoded += '=';
    }

    vectors_->set_lora_adapter(name, encoded);
    LOG_INFO("Vectors", "LoRA: published adapter '" + name +
             "' (" + std::to_string(bytes.size()) + " bytes)");
    return true;
}

bool AssistantOrchestrator::apply_peer_lora_adapters() {
    if (!vectors_ || !llama_ || !llama_->is_initialized()) return false;

    const std::vector<std::pair<std::string, std::string>> peer_adapters =
        vectors_->aggregated_lora_adapters();
    if (peer_adapters.empty()) return false;

    // Resolve the APPDATA path for temp adapter storage.
    std::string appdata_path;
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        appdata_path = std::string(path);
    } else if (const char* ad = std::getenv("APPDATA")) {
        appdata_path = ad;
    }
    if (appdata_path.empty()) appdata_path = ".";

    namespace fs = std::filesystem;
    for (const auto& adapter : peer_adapters) {
        // Write the adapter to a temp file so LlamaWrapper can load it.
        // Adapters are content-free by construction - they are weights,
        // not data. A peer never sees the user's conversations, only the
        // mathematical delta that makes the model more helpful for that
        // peer's domain.
        std::error_code ec;
        fs::path tmp_dir = fs::path(appdata_path) / "Sonny" / "vectors" / "lora";
        fs::create_directories(tmp_dir, ec);
        fs::path tmp_file = tmp_dir / (adapter.first + ".gguf");
        std::ofstream out(tmp_file, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) continue;
        out.write(adapter.second.data(), (std::streamsize)adapter.second.size());
        out.close();

        if (llama_->load_lora_adapter(tmp_file.string())) {
            LOG_LLAMANATIVE("LoRA: applied peer adapter '" + adapter.first +
                            "' (" + std::to_string(adapter.second.size()) + " bytes)");
        }
        // Clean up the temp file after loading (the adapter is kept in
        // memory by llama.cpp; the file is no longer needed).
        fs::remove(tmp_file, ec);
    }
    llama_->apply_lora(1.0f);
    return true;
}

bool AssistantOrchestrator::start(StatusCallback status_callback) {
    if (running_) {
        std::cerr << "Assistant is already running" << std::endl;
        return false;
    }

    status_callback_ = status_callback;
    running_ = true;

    if (!use_browser_stt_) {
        if (!audio_capture_->start([this](const std::vector<float>& audio_data) {
            this->on_audio_captured(audio_data);
        })) {
            std::cerr << "Failed to start audio capture" << std::endl;
            running_ = false;
            return false;
        }
    } else {
        std::cout << "Audio capture disabled (using browser STT)" << std::endl;
        browser_stt_polling_active_ = true;
        browser_stt_polling_thread_ = std::thread([this]() {
            this->poll_browser_stt_transcriptions();
        });
    }

    // Start polling thread for native STT transcriptions.
    python_stt_polling_active_ = true;
    python_stt_polling_thread_ = std::thread([this]() {
        LOG_STT("Result polling thread started");
        while (python_stt_polling_active_ && running_) {
            std::string transcription = stt_engine_->get_transcription();
            if (!transcription.empty()) {
                this->handle_processed_text(transcription);
            }
        }
    });

    if (avatar_) {
        avatar_->show();
    }

    if (cava_visualizer_) {
        cava_visualizer_->start();
    }

    if (status_callback_) {
        status_callback_("Personal Assistant started. Listening for speech...");
    }

    return true;
}

void AssistantOrchestrator::stop() {
    // The peer network is the only component holding sockets and threads of its
    // own, so it is shut down first.
    if (vectors_) {
        vectors_->stop();
    }

    // Release the webcam
    if (camera_capture_) {
        camera_capture_->stop();
    }

    // Save conversation memory before stopping
    if (rag_engine_ && rag_enabled_ && conversation_turn_count_ > 5) {
        std::string summary = rag_engine_->summarizeConversation(conversation_summary_);
        if (!summary.empty()) {
            rag_engine_->saveConversationMemory(session_id_, summary);
            LOG_RAG("Conversation memory saved (session: " + session_id_ + ")");
        }
    }
    
    // Shutdown RAG engine
    if (rag_engine_) {
        rag_engine_->shutdown();
    }
    
    // Shutdown Llama server
    if (llama_) {
        llama_->shutdown();
    }

    if (!running_) {
        return;
    }

    running_ = false;
    
    // Shutdown web speech server if active
    if (web_speech_ && web_speech_->is_running()) {
        web_speech_->stop();
    }
    
    if (!use_browser_stt_) {
        if (audio_capture_->is_capturing()) {
            audio_capture_->stop();
        }
    }
    
    stt_engine_->stop();
    
    if (browser_stt_polling_active_) {
        browser_stt_polling_active_ = false;
        if (browser_stt_polling_thread_.joinable()) {
            browser_stt_polling_thread_.join();
        }
    }

    python_stt_polling_active_ = false;
    if (python_stt_polling_thread_.joinable()) {
        python_stt_polling_thread_.join();
    }

    if (avatar_) {
        avatar_->hide();
    }

    if (cava_visualizer_) {
        cava_visualizer_->stop();
    }

    if (status_callback_) {
        status_callback_("Personal Assistant stopped.");
    }
}

void AssistantOrchestrator::handle_processed_text(const std::string& transcription) {
    LOG_DEBUG_COMPONENT("DEBUG", "handle_processed_text called with: \"" + transcription + "\"");
    if (transcription.size() >= 2 && transcription.front() == '(' && transcription.back() == ')') {
        LOG_STT_DEBUG("Ignoring non-speech annotation: \"" + transcription + "\"");
        return;
    }
    if (this->processing_) return;
    
    Jarvis::CavaVisualizer::clear_terminal_line();
    this->processing_ = true;
    this->conversation_turn_count_++;
    this->conversation_summary_ += "User: " + transcription + "\n";

    try {
        if (this->audio_playback_->is_playing()) {
            this->audio_playback_->stop_playback();
            LOG_DEBUG("AUDIO", "Interrupted playback");
        }

        // Show transcribed text immediately
        LOG_INFO("USER", "You: " + Jarvis::rtl_wrap(transcription));

        // Check if user is asking about the assistant's name
        std::string lower = transcription;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("name") != std::string::npos && 
            (lower.find("your") != std::string::npos || lower.find("who are you") != std::string::npos)) {
            const std::string sound_path = sounds_base_path_ + "sonny_my_name_is_sonny.wav";
            LOG_AI("Assistant: My name is Sonny");
            this->play_file_with_aec(sound_path);
            this->processing_ = false;
            return;
        }

        // Check if user is teaching a fact
        if (lower.find("remember") != std::string::npos && 
            rag_engine_ && rag_enabled_ && rag_engine_->isReady()) {
            std::string fact = transcription;
            for (const auto& suffix : {"remember that", "remember", "okay?", "."}) {
                size_t pos = lower.rfind(suffix);
                if (pos != std::string::npos) {
                    fact = fact.substr(0, pos);
                    break;
                }
            }
            fact.erase(0, fact.find_first_not_of(" \t\n\r,;:"));
            fact.erase(fact.find_last_not_of(" \t\n\r,;:") + 1);
            
            if (!fact.empty()) {
                LOG_INFO("RAG", "Teaching fact: \"" + fact + "\"");
                bool taught = rag_engine_->teach(fact, "user_knowledge");
                if (taught) {
                    LOG_INFO("RAG", "Fact taught successfully");
                } else {
                    LOG_ERROR("RAG", "Failed to teach fact");
                }
            }
        }

        // Tool decisions are left entirely to the LLM via its JSON tool-call format
        // (schemas are always injected when tools are enabled). No keyword/regex
        // pre-routing is used here â€” the LLM alone decides if a tool is needed.

        // Vision requests ("what do you see", "look at my screen", ...) bypass the
        // tool/RAG text path: an image is captured (a live camera frame while the
        // camera feed is on, otherwise a screen grab) and the answer is generated
        // from the image itself. Vision is automatically used when the camera feed
        // is enabled and the question asks for visual information.
        const bool wants_vision = is_vision_request(lower);
        if (wants_vision && llama_ && llama_->has_vision()) {
            int img_width = 0;
            int img_height = 0;
            std::string source;
            std::vector<unsigned char> image = capture_vision_rgb(img_width, img_height, &source);
            if (image.empty() || img_width <= 0 || img_height <= 0) {
                LOG_WARN("VISION", "No image could be captured - answering from text only");
            } else {
                LOG_INFO("VISION", "Sending " + source + " image " + std::to_string(img_width) + "x" +
                    std::to_string(img_height) + " (" + std::to_string(image.size() / 1024) +
                    " KB) to the vision model");
                SONNY_THINKING();
                auto vision_start = std::chrono::high_resolution_clock::now();
                const std::string vision_response =
                    this->generate_response(transcription, {image}, img_width, img_height, false, false);
                const long long vision_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - vision_start).count();
                const std::string clean_vision = clean_response_for_output(vision_response);
                LOG_AI("Vision: " + clean_vision);
                SONNY_SAY(Jarvis::rtl_wrap(clean_vision));
                this->synthesize_and_play(clean_vision);
                SONNY_LISTENING();
                this->log_llm_inference_perf(vision_ms, " [vision]");
                this->processing_ = false;
                return;
            }
        } else if (wants_vision) {
            LOG_WARN("VISION", "Vision was requested but no mmproj (vision projector) is loaded - "
                "answering from text only. Put a matching mmproj-*.gguf next to the model file.");
        }

        SONNY_THINKING();

std::string enhanced_prompt = transcription;
const bool fast_conversational_turn =
    lower.find("hello") != std::string::npos ||
    lower.find("hi ") != std::string::npos ||
    lower.find("hey") != std::string::npos ||
    lower.find("how are you") != std::string::npos ||
    lower.find("favorite") != std::string::npos ||
    lower.find("favourite") != std::string::npos ||
    lower.find("thanks") != std::string::npos ||
    lower.find("thank you") != std::string::npos ||
    lower.find("who are you") != std::string::npos ||
    lower.find("your name") != std::string::npos ||
    lower.find("short story") != std::string::npos ||
    lower.find("tell me a story") != std::string::npos ||
    lower.find("tell me a joke") != std::string::npos ||
    lower.find("make me laugh") != std::string::npos ||
    lower.find("poem") != std::string::npos ||
    lower.find("would you choose") != std::string::npos;

if (!fast_conversational_turn && this->rag_engine_ && this->rag_enabled_ && this->rag_engine_->isReady()) {
    std::string rag_context = this->rag_engine_->getContextForLLM(transcription);
    
    if (!rag_context.empty() && rag_context.find("\"error\"") == std::string::npos) {
        // The budget is configurable now (default 1500 chars). The old fixed
        // 500-char gate threw away the retrieved passage in most cases, which
        // made retrieval look useless even when it had found the right text.
        const size_t context_budget = static_cast<size_t>(rag_engine_->get_max_context_chars());
        if (rag_context.length() <= context_budget) {
            enhanced_prompt = "User question: " + transcription + "\n\n"
                "Reference info:\n" + rag_context + "\n"
                "Answer concisely based on the above.";
            LOG_DEBUG("RAG", "Context provided (" + std::to_string(rag_context.length()) + " chars)");
        } else {
            LOG_DEBUG("RAG", "Context too long (" + std::to_string(rag_context.length()) +
                                 " chars > " + std::to_string(context_budget) + "), skipping");
        }
    } else if (rag_context.find("Timeout") != std::string::npos || 
               rag_context.find("not ready") != std::string::npos) {
        rag_enabled_ = false;
        LOG_WARN("RAG", "Disabling RAG due to persistent errors");
    }
}

LOG_DEBUG("LLM", "Input: " + enhanced_prompt.substr(0, 200) + "...");

        const std::string response = this->generate_response(enhanced_prompt, true, false);
std::string clean_response = clean_response_for_output(response);
SONNY_SAY(Jarvis::rtl_wrap(clean_response));
this->synthesize_and_play(clean_response);
SONNY_LISTENING();
    } catch (const std::exception& e) {
        LOG_ERROR("ORCHESTRATOR", std::string("Error processing audio: ") + e.what());
    }
    this->processing_ = false;
}

// ============================================================================
// Vision: image capture and the "look at ..." request phrases
// ============================================================================

// Spoken (and typed) requests that mean "use your eyes". A table, so the wording
// is easy to extend: the LLM is never asked to guess whether an image is
// expected, because a wrong guess means confidently describing a screen it never
// saw.
bool AssistantOrchestrator::is_vision_request(const std::string& lower_text) const {
    static const char* const kPhrases[] = {
        "what do you see",
        "what can you see",
        "what are you seeing",
        "what do you see from camera",
        "what do you see from the camera",
        "what does the camera show",
        "what can you see from camera",
        "what can you see from the camera",
        "how many fingers",
        "how many fingers am i",
        "how many fingers are you",
        "count my fingers",

        "what does my screen show",
        "what does the screen show",
        "describe my screen",
        "describe the screen",
        "describe what shows on my screen",
        "describe what is on my screen",
        "what's on my screen",
        "what is on my screen",
        "whats on my screen",
        "what's on the screen",
        "what is on the screen",
        "whats on the screen",
        "can you describe my screen",
        "can you see my screen",
        "can you see my camera",
        "can you see my camera feed",
        "can you see me",
        "do you see me",
        "see me through the camera",
        "see me through camera",
        "see me on the camera",
        "see me with the camera",
        "look through the camera",
        "see my camera",
        "see the camera",
        "see my camera feed",
        "look at my camera",
        "look at the camera",
        "camera feed please",
        "show me what you see",
        "show me what you can see",
        "tell me what you see",
        "tell me what you can see",
        "see my screen",
        "read my screen",
        "read the screen",
        "look at my screen",
        "look at the screen",
        "look at my display",
        "look at my monitor",
        "look at the display",
        "look at this image",
        "look at the image",
        "take a look at my screen",
    };
    for (const char* phrase : kPhrases) {
        if (lower_text.find(phrase) != std::string::npos) return true;
    }
    return false;
}

// Screenshot of the primary display as top-down RGB24 (width * height * 3), the
// layout mtmd_bitmap_init() expects. The screen is scaled straight into a 32-bit
// DIB so the (per-pixel) colour conversion runs on the small image only, and the
// long edge is capped because image tokens cost prefill time: a 1920x1080 grab is
// roughly 2.25x the tokens of a 1280x720 one on a 6 GB laptop GPU.
std::vector<unsigned char> AssistantOrchestrator::capture_screenshot_rgb(int& out_width, int& out_height) {
    out_width = 0;
    out_height = 0;

    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0 || screen_h <= 0) return {};

    constexpr int kMaxVisionImageDim = 1280;
    const double scale = std::min(1.0, static_cast<double>(kMaxVisionImageDim) /
                                        static_cast<double>(std::max(screen_w, screen_h)));
    const int dst_w = std::max(1, static_cast<int>(std::lround(screen_w * scale)));
    const int dst_h = std::max(1, static_cast<int>(std::lround(screen_h * scale)));

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) return {};

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dst_w;
    bmi.bmiHeader.biHeight = -dst_h;  // negative height = top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;    // BGRA
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    std::vector<unsigned char> rgb;
    if (dib && pixels) {
        HDC mem_dc = CreateCompatibleDC(screen_dc);
        HGDIOBJ old_bitmap = SelectObject(mem_dc, dib);
        SetStretchBltMode(mem_dc, HALFTONE);  // smooth downscale instead of nearest
        SetBrushOrgEx(mem_dc, 0, 0, nullptr);
        if (StretchBlt(mem_dc, 0, 0, dst_w, dst_h, screen_dc, 0, 0, screen_w, screen_h, SRCCOPY)) {
            GdiFlush();  // the DIB bits are only guaranteed valid after a flush
            const size_t pixel_count = static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);
            rgb.resize(pixel_count * 3);
            const unsigned char* src = static_cast<const unsigned char*>(pixels);
            for (size_t i = 0; i < pixel_count; ++i) {
                rgb[i * 3 + 0] = src[i * 4 + 2];  // R (the DIB stores BGRX)
                rgb[i * 3 + 1] = src[i * 4 + 1];  // G
                rgb[i * 3 + 2] = src[i * 4 + 0];  // B
            }
            out_width = dst_w;
            out_height = dst_h;
            LOG_DEBUG("VISION", "Screen grab " + std::to_string(screen_w) + "x" + std::to_string(screen_h) +
                " scaled to " + std::to_string(dst_w) + "x" + std::to_string(dst_h));
        } else {
            LOG_WARN("VISION", "StretchBlt failed - the screen could not be grabbed");
        }
        SelectObject(mem_dc, old_bitmap);
        DeleteDC(mem_dc);
    } else {
        LOG_WARN("VISION", "CreateDIBSection failed - the screen could not be grabbed");
    }
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen_dc);
    return rgb;
}

// Picks the image source for a vision request: a live camera frame while the
// camera feed is on, otherwise a screen grab. A camera feed that has no frame yet
// (no webcam, or the frame loop has not produced one) falls back to the screen -
// the same behaviour CameraCapture documents for the orchestrator.
std::vector<unsigned char> AssistantOrchestrator::capture_vision_rgb(int& out_width, int& out_height,
                                                                    std::string* source_label) {
    if (camera_active_ && camera_capture_) {
        int cam_w = 0;
        int cam_h = 0;
        std::vector<unsigned char> frame = camera_capture_->capture_frame(cam_w, cam_h);
        if (!frame.empty() && cam_w > 0 && cam_h > 0) {
            out_width = cam_w;
            out_height = cam_h;
            if (source_label) *source_label = "camera";
            return frame;
        }
        LOG_DEBUG("VISION", "Camera feed has no frame available - falling back to a screen grab");
    }

    std::vector<unsigned char> shot = capture_screenshot_rgb(out_width, out_height);
    if (source_label) *source_label = "screen";
    return shot;
}

void AssistantOrchestrator::set_camera_feed_config(bool enabled, const std::string& device_name) {
    if (enabled && !camera_capture_) {
        camera_capture_ = std::make_unique<Jarvis::CameraCapture>();
        camera_capture_->initialize();
    }
    if (!camera_capture_) return;

    // Check if device changed while camera is active
    bool device_changed = (current_camera_device_ != device_name) && camera_active_;
    current_camera_device_ = device_name;

    // Update camera state based on enabled setting
    bool was_active = camera_active_;
    camera_active_ = enabled;

    if (enabled && !was_active) {
        // Set device before starting
        camera_capture_->set_camera_device(device_name);
        camera_capture_->start();
        LOG_INFO("VISION", "Camera feed ON - vision requests now capture from the webcam");
    } else if (!enabled && was_active) {
        camera_capture_->stop();
        LOG_INFO("VISION", "Camera feed OFF - vision requests capture the screen");
    } else if (enabled && was_active && device_changed) {
        // Camera is already active and device changed - restart
        camera_capture_->stop();
        camera_capture_->set_camera_device(device_name);
        camera_capture_->start();
        LOG_INFO("VISION", "Camera feed restarted with new device: " + device_name);
    }
}

void AssistantOrchestrator::toggle_camera_feed(bool on) {
    if (on && !camera_capture_) {
        // initialize() creates the camera; tolerate a tray click before it ran.
        camera_capture_ = std::make_unique<Jarvis::CameraCapture>();
        camera_capture_->initialize();
    }
    if (!camera_capture_) return;

    camera_active_ = on;
    if (on) {
        camera_capture_->start();
        LOG_INFO("VISION", "Camera feed ON - vision requests now capture from the webcam");
    } else {
        camera_capture_->stop();
        LOG_INFO("VISION", "Camera feed OFF - vision requests capture the screen");
    }
}

void AssistantOrchestrator::on_audio_captured(const std::vector<float>& audio_data) {
    // Removed verbose audio callback logging
    // static int callback_count = 0;
    // if (callback_count < 3) {
    //     std::cout << "[ORCHESTRATOR] Audio callback: " << audio_data.size() << " samples" << std::endl;
    // }
    // callback_count++;

    if (!running_) {
        return;
    }

    if (stt_engine_ && stt_engine_->is_running()) {
        stt_engine_->send_audio(audio_data, 16000);
    }

    if (cava_visualizer_ && !processing_) {
        cava_visualizer_->feed_audio(audio_data);
    }
}

std::string AssistantOrchestrator::transcribe_audio(const std::vector<float>& audio_data) {
    std::string transcription;
    if (use_browser_stt_) {
        transcription = web_speech_->get_transcription();
    } else {
        stt_engine_->send_audio(audio_data, 16000);
        transcription = stt_engine_->get_transcription();
    }
    return transcription;
}

// Find split position in buffer for streaming sentence extraction.
// search_from is the position to start scanning from (avoids rescanning old text).
static size_t find_sentence_split(const std::string& buffer, size_t search_from = 0) {
    if (buffer.length() < 3) return std::string::npos;
    if (search_from >= buffer.length()) return std::string::npos;
    
    for (size_t i = search_from + 1; i < buffer.length(); ++i) {
        char c = buffer[i - 1];
        char next = buffer[i];
        
        if ((c == '.' || c == '!' || c == '?') && (next == ' ' || next == '\n' || next == '\t' || next == '\r')) {
            if (i >= 2 && isdigit(static_cast<unsigned char>(buffer[i - 2])) && (i < buffer.length() && isdigit(static_cast<unsigned char>(buffer[i])))) {
                continue;
            }
            return i - 1;
        }
        
        if (c == '\n' && next == '\n') {
            return i - 1;
        }
    }
    
    if (buffer.length() > 80 && search_from < 40) {
        for (size_t i = 40; i < buffer.length(); ++i) {
            char c = buffer[i - 1];
            char next = buffer[i];
            if ((c == ',' || c == ';' || c == ':') && (next == ' ' || next == '\n')) {
                return i - 1;
            }
        }
    }
    
    return std::string::npos;
}

// Build (once per tool-schemas flag change) the full system prompt that is
// actually sent to the LLM - persona + tool schemas + file-explorer context -
// and push it into LlamaWrapper. Keeping this string byte-identical across
// calls is what lets the KV-cache prefix reuse skip the ~2k-token system
// prompt prefill on every turn (only the new user message gets prefilled).
void AssistantOrchestrator::refresh_llama_system_prompt(bool want_tool_schemas) {
    if (want_tool_schemas != cached_tool_schemas_enabled_ || cached_full_system_prompt_.empty()) {
        if (want_tool_schemas) {
            std::string tool_schemas = Jarvis::ToolRegistry::getInstance().getToolSchemas();

            std::string current_dir = "";
            Jarvis::Tool* file_explorer = Jarvis::ToolRegistry::getInstance().getTool("file_explorer");
            if (file_explorer) {
                Jarvis::FileExplorerTool* fe_tool = static_cast<Jarvis::FileExplorerTool*>(file_explorer);
                current_dir = fe_tool->getCurrentDirectory();
            }

            std::string context_info = "";
            if (!current_dir.empty()) {
                context_info = "Current directory: " + current_dir + "\n";
            }

            cached_full_system_prompt_ = system_prompt_ + "\n\n## Tool Usage\n"
                "You have access to these tools. When the user's request requires a tool, respond with ONLY this JSON format (no other text).\nNOTE: the user's text is voice-transcribed - a spoken 'slash' or 'backslash' between names is a path separator ('x slash y' means 'X:\\y'), and path parameters must include every folder name the user said.\n"
                "{\"tool\": \"tool_name\", \"params\": {\"param_name\": \"value\"}}\n\n"
                + context_info +
                "Available tools:\n" + tool_schemas + "\n\n"
                "## Browser Commands\n"
                "Spoken browser commands use one action verb each. All browser actions share a single interactive Chromium session, so pages stay open after each call.\n"
                "  Open YouTube              -> browser, action=open url=youtube\n"
                "  Go to example.com         -> browser, action=open url=https://example.com\n"
                "  Search for cats           -> browser, action=search site=google query=cats\n"
                "  Search YouTube for X      -> browser, action=search site=youtube query=X\n"
                "  Open the second result    -> browser, action=open_result index=2\n"
                "  Open the video named X    -> browser, action=open_result site=youtube title=X\n"
                "  Scroll down               -> browser, action=scroll direction=down\n"
                "  Go back / forward         -> browser, action=back / action=forward\n"
                "  Reload                    -> browser, action=reload\n\n"
                "## Camera Commands\n"
                "  Take a photo / snapshot   -> camera, action=photo\n"
                "  Record a video / clip     -> camera, action=start_recording\n"
                "  Stop / finish recording   -> camera, action=stop_recording\n"
                "  Show last video / photo   -> camera, action=open_last kind=videos/photos\n"
                "  Open the capture folder   -> camera, action=open_folder\n"
                "'record'/'video'/'clip' means action=start_recording, never photo. Recording takes two calls: start_recording, then stop_recording when the user asks to stop.\n\n"
                "IMPORTANT: If no tool is needed, respond in normal conversational English. Do NOT output JSON unless calling a tool.";
        } else {
            cached_full_system_prompt_ = system_prompt_;
        }
        cached_tool_schemas_enabled_ = want_tool_schemas;
    }
    if (llama_ && llama_->get_system_prompt() != cached_full_system_prompt_) {
        llama_->set_system_prompt(cached_full_system_prompt_);
    }
}

std::string AssistantOrchestrator::generate_response(const std::string& text, bool include_tool_schemas, bool stream_tts, bool is_first_call) {
    return generate_response(text, {}, 0, 0, include_tool_schemas, stream_tts, is_first_call);
}

std::string AssistantOrchestrator::generate_response(
        const std::string& text,
        const std::vector<std::vector<unsigned char>>& images,
        int img_width, int img_height,
        bool include_tool_schemas, bool stream_tts, bool is_first_call) {
    auto llm_start = std::chrono::high_resolution_clock::now();
    if (is_first_call) {
        tool_followup_depth_ = 0;  // a fresh user utterance starts a new tool chain
    }

    bool want_tool_schemas = tools_enabled_ && include_tool_schemas;
    // Centralized in refresh_llama_system_prompt() so initialize() can push
    // the same string BEFORE llama init, letting warmup() seed the KV prefix
    // cache with the production system prompt (see AssistantOrchestrator::
    // initialize). The block below is now a no-op (the helper owns this).
    refresh_llama_system_prompt(want_tool_schemas);
    std::string enhanced_prompt = cached_full_system_prompt_;
    
    // (System prompt already pushed to the LLM by refresh_llama_system_prompt.)

    std::string raw_response;
    raw_response.reserve(4096);
    std::string stream_sentence_buf;
    stream_sentence_buf.reserve(512);
    bool is_tool_call_detected = false;
    bool in_thinking_tag = false;
    size_t sentence_search_from = 0;

    auto stream_cb = [&](const std::string& token) {
        raw_response.append(token);
        if (!stream_tts) return;
        if (!is_tool_call_detected && tools_enabled_) {
            if (raw_response.find("{\"tool\":") != std::string::npos ||
                raw_response.find("{\"tool\" :") != std::string::npos ||
                raw_response.find("{\"browser\":") != std::string::npos ||
                raw_response.find("{\"google_search\":") != std::string::npos ||
                raw_response.find("{\"web_search\":") != std::string::npos ||
                raw_response.find("{\"youtube_search\":") != std::string::npos ||
                raw_response.find("{\"youtube_video\":") != std::string::npos ||
                raw_response.find("{\"music_player\":") != std::string::npos ||
                raw_response.find("{\"file_operation\":") != std::string::npos ||
                raw_response.find("{\"file_explorer\":") != std::string::npos ||
                raw_response.find("{\"system_command\":") != std::string::npos ||
                raw_response.find("{\"calculator\":") != std::string::npos ||
                raw_response.find("{\"get_time\":") != std::string::npos ||
                raw_response.find("{\"clipboard\":") != std::string::npos ||
                raw_response.find("{\"add_media_directory\":") != std::string::npos ||
                raw_response.find("{\"remove_media_directory\":") != std::string::npos ||
                raw_response.find("{\"list_media_library\":") != std::string::npos ||
                raw_response.find("{\"camera\":") != std::string::npos) {
                is_tool_call_detected = true;
            }
        }
        if (is_tool_call_detected) return;
        stream_sentence_buf.append(token);
        size_t tool_json_pos = stream_sentence_buf.find("{\"tool\":");
        if (tool_json_pos == std::string::npos) {
            tool_json_pos = stream_sentence_buf.find("{\"tool\" :");
        }
        if (tool_json_pos == std::string::npos) {
            static const char* tool_patterns[] = {
                "{\"browser\":", "{\"google_search\":", "{\"web_search\":", "{\"youtube_search\":",
                "{\"youtube_video\":", "{\"music_player\":", "{\"file_operation\":", "{\"file_explorer\":",
                "{\"system_command\":", "{\"calculator\":", "{\"get_time\":", "{\"clipboard\":",
                "{\"add_media_directory\":", "{\"remove_media_directory\":", "{\"list_media_library\":",
                "{\"camera\":"
            };
            for (const char* pattern : tool_patterns) {
                tool_json_pos = stream_sentence_buf.find(pattern);
                if (tool_json_pos != std::string::npos) break;
            }
        }
        if (tool_json_pos != std::string::npos) {
            stream_sentence_buf = stream_sentence_buf.substr(0, tool_json_pos);
            is_tool_call_detected = true;
            return;
        }
        if (stream_sentence_buf.find("<think", sentence_search_from) != std::string::npos ||
            stream_sentence_buf.find("<|channel|>", sentence_search_from) != std::string::npos) {
            in_thinking_tag = true;
        }
        if (in_thinking_tag) {
            if (stream_sentence_buf.find("</think", sentence_search_from) != std::string::npos ||
                stream_sentence_buf.find("<|end|>", sentence_search_from) != std::string::npos) {
                in_thinking_tag = false;
                stream_sentence_buf = clean_response_for_output(stream_sentence_buf);
                sentence_search_from = 0;
            }
            return;
        }
        size_t split_pos = find_sentence_split(stream_sentence_buf, sentence_search_from);
        if (split_pos != std::string::npos) {
            std::string sentence_chunk = stream_sentence_buf.substr(0, split_pos + 1);
            stream_sentence_buf = stream_sentence_buf.substr(split_pos + 1);
            sentence_search_from = 0;
            std::string clean_chunk = clean_response_for_output(sentence_chunk);
            if (!clean_chunk.empty()) {
                this->synthesize_and_play(clean_chunk);
            }
        } else {
            sentence_search_from = stream_sentence_buf.length();
        }
    };
    if (!images.empty() && llama_->has_vision()) {
        llama_->generate_stream_with_images(text, images, img_width, img_height, stream_cb, 256, 0.1f);
    } else {
        llama_->generate_stream(text, stream_cb, 256, 0.1f);
    }

    if (tools_enabled_) {
        std::string tool_result = parse_llm_tool_call(raw_response);
        if (!tool_result.empty()) {
            // Log the PERF line here: the tool path returns before the end of
            // the function, and without this the token/tok/s stats would never
            // be logged for tool interactions.
            auto llm_end = std::chrono::high_resolution_clock::now();
            auto llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(llm_end - llm_start).count();
            log_llm_inference_perf(llm_ms, is_first_call ? ""
                : " [follow-up " + std::to_string(tool_followup_depth_.load()) + "]");
            // The follow-up call must carry the user's ORIGINAL request:
            // without it the LLM only sees "Tool result: ..." with no
            // question/action in context and answers with a generic greeting
            // instead of confirming the action it just performed.
            std::string followup_prompt =
                "The user's request was: " + text + "\n\n"
                "A tool was called to handle it and returned: " + tool_result + "\n\n"
                "Confirm the outcome to the user in one short spoken sentence. "
                "State that it was done ONLY if the tool result confirms success; "
                "if the tool result reports an error or failure, say that it failed - "
                "never claim success. "
                "Do NOT call any more tools. Do not greet or ask what the user needs - the request is above.";
            // Keep the caller's include_tool_schemas: flipping it to false here
            // changes the system prompt, which invalidates llama.cpp's KV-cache
            // prefix and forces a full ~2k-token re-prefill on the NEXT user
            // query. Keeping it identical preserves the cache across the whole
            // session (and lets the follow-up self-correct tool calls).
            if (tool_followup_depth_ < kMaxToolFollowups) {
                tool_followup_depth_++;
                return this->generate_response(followup_prompt, include_tool_schemas, stream_tts, false);
            }
            // Retry cap reached: the model keeps re-calling the same failing
            // tool (each round trip costs seconds of generation), which reads
            // as a long stall. Stop the spiral and tell the user it failed.
            LOG_WARN("TOOL", "Tool follow-up cap (" + std::to_string(kMaxToolFollowups)
                + ") reached - stopping retries");
            std::string fallback = "I tried that a few times, but it did not go through.";
            if (stream_tts) {
                this->synthesize_and_play(fallback);
            }
            return fallback;
        }
    }

    if (stream_tts && !is_tool_call_detected && !stream_sentence_buf.empty()) {
        std::string clean_remainder = clean_response_for_output(stream_sentence_buf);
        if (!clean_remainder.empty()) {
            this->synthesize_and_play(clean_remainder);
        }
    }

    if (!llm_ready_ && !raw_response.empty()) {
        llm_ready_ = true;
        if (video_player_ready_) {
            video_player_->stop();
            LOG_VIDEOPLAYER("Loading animation stopped - LLM is ready");
        }
    }

    std::string clean_response = clean_response_for_output(raw_response);

    auto llm_end = std::chrono::high_resolution_clock::now();
    auto llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(llm_end - llm_start).count();
    log_llm_inference_perf(llm_ms, is_first_call ? ""
        : " [follow-up " + std::to_string(tool_followup_depth_.load()) + "]");

    return clean_response;
}

// Log the [PERF] LLM Inference line (tokens, cache reuse, prefill/generation
// tok/s) for the most recent llama_ generation. tag is appended for
// tool-call follow-ups so the retry chain is visible in the log.
void AssistantOrchestrator::log_llm_inference_perf(long long llm_ms, const std::string& tag) {
    if (!llama_) return;
    int prompt_tokens = llama_->get_last_prompt_tokens();
    int cached_tokens = llama_->get_last_cached_tokens();
    int generated_tokens = llama_->get_last_generated_tokens();
    long long prefill_ms = llama_->get_last_prefill_ms();
    long long generate_ms = llama_->get_last_generate_ms();
    int sent_tokens = prompt_tokens - cached_tokens;
    std::ostringstream perf_msg;
    perf_msg << "LLM Inference: " << llm_ms << " ms | Prompt: " << prompt_tokens
             << " tokens (" << sent_tokens << " sent, " << cached_tokens << " cached";
    if (prefill_ms > 0 && sent_tokens > 0) {
        perf_msg << ", " << std::fixed << std::setprecision(1)
                 << sent_tokens * 1000.0 / prefill_ms << " tok/s";
    }
    perf_msg << ") | Generated: " << generated_tokens << " tokens";
    if (generate_ms > 0 && generated_tokens > 0) {
        perf_msg << " (" << std::fixed << std::setprecision(1)
                 << generated_tokens * 1000.0 / generate_ms << " tok/s)";
    }
    LOG_PERF(perf_msg.str() + tag);
}

// jarvis_ears.py AEC: play a file while flagging the assistant as speaking so
// the STT wake-word handler ignores the input (is_mouth_speaking()).
void AssistantOrchestrator::play_file_with_aec(const std::string& path) {
    if (stt_engine_) stt_engine_->set_assistant_speaking(true);
    audio_playback_->play_file(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    if (stt_engine_) stt_engine_->set_assistant_speaking(false);
}
void AssistantOrchestrator::synthesize_and_play(const std::string& text) {
    std::string clean = clean_response_for_output(text);
    if (clean.empty()) return;

    auto tts_start = std::chrono::high_resolution_clock::now();

    if (use_browser_tts_) {
        size_t text_len = clean.length();
        int speak_ms = static_cast<int>(text_len * 1000 / 15) + 600;
        // jarvis_ears.py AEC: keep listening but ignore wake words while the
        // assistant's mouth is moving.
        if (stt_engine_) stt_engine_->set_assistant_speaking(true);
        try {
            if (!web_speech_->speak(clean, browser_voice_)) {
                std::cerr << "Failed to speak using Web Speech API" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Web Speech API exception: " << e.what() << std::endl;
        }
        Sleep(speak_ms);
        if (stt_engine_) stt_engine_->set_assistant_speaking(false);
    } else {
        std::vector<float> audio_data;
        try {
            audio_data = kokoro_->synthesize(clean, voice_name_, tts_speed_);
        } catch (const std::exception& e) {
            std::cerr << "TTS synthesis exception: " << e.what() << std::endl;
            return;
        } catch (...) {
            std::cerr << "TTS synthesis unknown exception" << std::endl;
            return;
        }

        if (!audio_data.empty()) {
            try {
                // jarvis_ears.py AEC: keep listening but ignore wake words
                // while the assistant's mouth is moving. No capture muting:
                // audio keeps flowing to the wake word detector, exactly like
                // jarvis_ears.py.
                if (stt_engine_) stt_engine_->set_assistant_speaking(true);
                audio_playback_->play(audio_data);
            } catch (const std::exception& e) {
                std::cerr << "Audio playback exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Audio playback unknown exception" << std::endl;
            }
            if (stt_engine_) stt_engine_->set_assistant_speaking(false);
        } else {
            std::cerr << "Failed to synthesize audio" << std::endl;
        }
    }

    auto tts_end = std::chrono::high_resolution_clock::now();
    auto tts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tts_end - tts_start).count();
    LOG_PERF("TTS Synthesis: " + std::to_string(tts_ms) + " ms");
}
void AssistantOrchestrator::set_system_prompt(const std::string& prompt) {
    system_prompt_ = prompt;
    // Invalidate the cached full system prompt so it gets rebuilt on next use.
    cached_full_system_prompt_.clear();
    cached_tool_schemas_enabled_ = false;
}

// Unescape JSON string escapes (\\, \", \/, \n, \r, \t, \b, \f, \uXXXX)
static std::string json_unescape(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[i + 1];
            switch (next) {
                case '\\': result += '\\'; i++; break;
                case '"':  result += '"';  i++; break;
                case '/':  result += '/';  i++; break;
                case 'n':  result += '\n'; i++; break;
                case 'r':  result += '\r'; i++; break;
                case 't':  result += '\t'; i++; break;
                case 'b':  result += '\b'; i++; break;
                case 'f':  result += '\f'; i++; break;
                case 'u': {
                    // Unicode escape - handle basic case by preserving the code point as UTF-8
                    if (i + 5 < input.size()) {
                        std::string hex = input.substr(i + 2, 4);
                        try {
                            unsigned int code = (unsigned int)std::stoul(hex, nullptr, 16);
                            // Convert code point to UTF-8
                            if (code < 0x80) {
                                result += (char)code;
                            } else if (code < 0x800) {
                                result += (char)(0xC0 | (code >> 6));
                                result += (char)(0x80 | (code & 0x3F));
                            } else {
                                result += (char)(0xE0 | (code >> 12));
                                result += (char)(0x80 | ((code >> 6) & 0x3F));
                                result += (char)(0x80 | (code & 0x3F));
                            }
                        } catch (...) {
                            result += input[i]; // keep original on parse error
                        }
                        i += 5;
                    } else {
                        result += input[i];
                    }
                    break;
                }
                default: result += input[i]; break;
            }
        } else {
            result += input[i];
        }
    }
    return result;
}

std::string AssistantOrchestrator::clean_response_for_output(const std::string& response) {
    std::string result;
    result.reserve(response.size());
    
    bool in_tag = false;
    bool in_think = false;
    size_t i = 0;
    
    while (i < response.size()) {
        if (!in_tag && response[i] == '<') {
            in_tag = true;
            size_t tag_end = response.find('>', i);
            if (tag_end == std::string::npos) {
                break;
            }
            std::string tag = response.substr(i + 1, tag_end - i - 1);
            
            bool should_skip = false;
            if (tag.find("think") != std::string::npos ||
                tag.find("reason") != std::string::npos ||
                tag.find("env") != std::string::npos ||
                tag.find("tool") != std::string::npos ||
                tag.find("assistant") != std::string::npos ||
                tag.find("user") != std::string::npos ||
                tag.find("system") != std::string::npos ||
                tag.find("channel") != std::string::npos ||
                tag.find("start") != std::string::npos ||
                tag.find("end") != std::string::npos ||
                tag.find("|") != std::string::npos) {
                should_skip = true;
            }
            
            if (should_skip) {
                if (tag.find("think") != std::string::npos) {
                    in_think = true;
                }
                i = tag_end + 1;
                continue;
            }
            
            in_tag = false;
            i = tag_end + 1;
            continue;
        }
        
        if (in_tag) {
            if (response[i] == '>') {
                in_tag = false;
            }
            i++;
            continue;
        }
        
        if (in_think) {
            if (response[i] == '<' && response.size() - i >= 4 &&
                response[i + 1] == '/' && response[i + 2] == 't' && response[i + 3] == 'h') {
                size_t tag_end = response.find('>', i);
                if (tag_end != std::string::npos && response.substr(i, tag_end - i + 1).find("/think") != std::string::npos) {
                    in_think = false;
                    i = tag_end + 1;
                    continue;
                }
            }
            i++;
            continue;
        }
        
        result += response[i];
        i++;
    }
    
    size_t start = result.find_first_not_of(" \t\n\r");
    size_t end_pos = result.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end_pos != std::string::npos) {
        result = result.substr(start, end_pos - start + 1);
    } else {
        result.clear();
    }
    
    // Strip JSON tool calls from the response
    auto erase_matching_json = [&](const std::string& pattern) {
        size_t json_pos = 0;
        while ((json_pos = result.find(pattern, json_pos)) != std::string::npos) {
            size_t brace_start = result.find('{', json_pos);
            if (brace_start == std::string::npos) {
                json_pos += pattern.length();
                continue;
            }
            int depth = 0;
            size_t brace_end = brace_start;
            bool closed = false;
            while (brace_end < result.length()) {
                if (result[brace_end] == '{') {
                    depth++;
                } else if (result[brace_end] == '}') {
                    depth--;
                    if (depth == 0) {
                        brace_end++;
                        closed = true;
                        break;
                    }
                }
                brace_end++;
            }
            if (closed) {
                result.erase(json_pos, brace_end - json_pos);
            } else {
                json_pos += pattern.length();
            }
        }
    };

    erase_matching_json("{\"tool\":");
    erase_matching_json("{\"tool\" :");
    for (const auto& tn : {"browser", "google_search", "web_search", "youtube_search", "youtube_video", 
                           "system_command", "calculator", "get_time", "clipboard", "file_operation", 
                           "file_explorer", "music_player", "add_media_directory", "remove_media_directory", 
                           "list_media_library"}) {
        erase_matching_json("{\"" + std::string(tn) + "\":");
    }
    
    size_t pos = 0;
    while ((pos = result.find("  ", pos)) != std::string::npos) {
        result.replace(pos, 2, " ");
    }
    
    return result;
}

std::string AssistantOrchestrator::parse_llm_tool_call(const std::string& llm_output) {
    // Try to parse JSON tool call from LLM output
    auto& registry = Jarvis::ToolRegistry::getInstance();
    std::vector<std::string> tool_names = registry.getAllToolNames();
    
    for (const auto& tn : tool_names) {
        std::string search_pattern = "{\"" + tn + "\":{";
        size_t start = llm_output.find(search_pattern);
        if (start == std::string::npos) {
            search_pattern = "{\"" + tn + "\": {";
            start = llm_output.find(search_pattern);
        }
        if (start != std::string::npos) {
            std::string tool_name = tn;
            
            size_t brace_start = llm_output.find("{", start + search_pattern.length() - 1);
            if (brace_start == std::string::npos) continue;
            
            int depth = 1;
            size_t brace_end = brace_start + 1;
            while (depth > 0 && brace_end < llm_output.length()) {
                if (llm_output[brace_end] == '{') depth++;
                else if (llm_output[brace_end] == '}') depth--;
                brace_end++;
            }
            if (depth != 0) continue;
            
            std::string inner_json = llm_output.substr(brace_start, brace_end - brace_start);

            std::map<std::string, std::string> params;
            size_t pos = 0;
            while (pos < inner_json.length()) {
                size_t key_start = inner_json.find("\"", pos);
                if (key_start == std::string::npos) break;
                key_start++;
                size_t key_end = inner_json.find("\"", key_start);
                if (key_end == std::string::npos) break;
                std::string key = inner_json.substr(key_start, key_end - key_start);

                size_t colon_pos = inner_json.find(":", key_end);
                if (colon_pos == std::string::npos) break;
                colon_pos++;

                while (colon_pos < inner_json.length() && (inner_json[colon_pos] == ' ' || inner_json[colon_pos] == '\t')) {
                    colon_pos++;
                }

                std::string value;
                if (colon_pos < inner_json.length() && inner_json[colon_pos] == '"') {
                    size_t value_start = colon_pos + 1;
                    size_t value_end = inner_json.find("\"", value_start);
                    if (value_end != std::string::npos) {
                        value = inner_json.substr(value_start, value_end - value_start);
                        pos = value_end + 1;
                    } else {
                        break;
                    }
                } else {
                    size_t value_end = inner_json.find_first_of(",}", colon_pos);
                    if (value_end != std::string::npos) {
                        value = inner_json.substr(colon_pos, value_end - colon_pos);
                        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                            value.pop_back();
                        }
                        pos = value_end + 1;
                    } else {
                        break;
                    }
                }

                if (!key.empty()) {
                    params[key] = json_unescape(value);
                }
            }
            
            const auto tool_started_at = std::chrono::steady_clock::now();
            Jarvis::ToolResult result = registry.executeTool(tool_name, params);
            this->record_tool_outcome(tool_name, result.success, tool_started_at);

            if (result.success) {
                return result.output;
            } else {
                return "Error: " + result.error;
            }
        }
    }
    
    // Handle Format 1: {"tool": "...", "params": {...}}
    size_t start = llm_output.find("{\"tool\":");
    if (start == std::string::npos) {
        start = llm_output.find("{\"tool\" :");
    }
    if (start == std::string::npos) {
        start = llm_output.find("{\n  \"tool\"");
    }
    if (start == std::string::npos) {
        return "";
    }

    int depth = 1;
    size_t end = start + 1;
    while (depth > 0 && end < llm_output.length()) {
        if (llm_output[end] == '{') depth++;
        else if (llm_output[end] == '}') depth--;
        end++;
    }
    if (depth != 0) {
        return "";
    }

    std::string json_str = llm_output.substr(start, end - start);

    size_t tool_start = json_str.find("\"tool\":");
    if (tool_start == std::string::npos) {
        tool_start = json_str.find("\"tool\" :");
    }
    if (tool_start == std::string::npos) return "";

    tool_start = json_str.find("\"", tool_start + 7);
    if (tool_start == std::string::npos) return "";
    tool_start++;

    size_t tool_end = json_str.find("\"", tool_start);
    if (tool_end == std::string::npos) return "";

    std::string tool_name = json_str.substr(tool_start, tool_end - tool_start);

    size_t params_start = json_str.find("\"params\":");
    if (params_start == std::string::npos) {
        params_start = json_str.find("\"params\" :");
    }
    if (params_start == std::string::npos) return "";

    params_start = json_str.find("{", params_start + 8);
    if (params_start == std::string::npos) return "";

    depth = 1;
    size_t params_end = params_start + 1;
    while (depth > 0 && params_end < json_str.length()) {
        if (json_str[params_end] == '{') depth++;
        else if (json_str[params_end] == '}') depth--;
        params_end++;
    }
    if (depth != 0) return "";

    std::string params_json = json_str.substr(params_start, params_end - params_start);

    std::map<std::string, std::string> params;
    size_t pos = 0;
    while (pos < params_json.length()) {
        size_t key_start = params_json.find("\"", pos);
        if (key_start == std::string::npos) break;
        key_start++;
        size_t key_end = params_json.find("\"", key_start);
        if (key_end == std::string::npos) break;
        std::string key = params_json.substr(key_start, key_end - key_start);

        size_t colon_pos = params_json.find(":", key_end);
        if (colon_pos == std::string::npos) break;
        colon_pos++;

        while (colon_pos < params_json.length() && (params_json[colon_pos] == ' ' || params_json[colon_pos] == '\t')) {
            colon_pos++;
        }

        std::string value;
        if (colon_pos < params_json.length() && params_json[colon_pos] == '"') {
            size_t value_start = colon_pos + 1;
            size_t value_end = params_json.find("\"", value_start);
            if (value_end != std::string::npos) {
                value = params_json.substr(value_start, value_end - value_start);
                pos = value_end + 1;
            } else {
                break;
            }
        } else {
            size_t value_end = params_json.find_first_of(",}", colon_pos);
            if (value_end != std::string::npos) {
                value = params_json.substr(colon_pos, value_end - colon_pos);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.pop_back();
                }
                pos = value_end + 1;
            } else {
                break;
            }
        }

        if (!key.empty()) {
            params[key] = json_unescape(value);
        }
    }

    const auto tool_started_at = std::chrono::steady_clock::now();
    Jarvis::ToolResult result = registry.executeTool(tool_name, params);
    this->record_tool_outcome(tool_name, result.success, tool_started_at);

    if (result.success) {
        return result.output;
    } else {
        return "Error: " + result.error;
    }
}


// Records one tool invocation for the vectors. Deliberately content-free:
// a tool name, whether it worked and how long it took - the parameters, the
// transcribed query and the result are never touched.
void AssistantOrchestrator::record_tool_outcome(const std::string& tool,
                                                bool success,
                                                std::chrono::steady_clock::time_point started_at) {
    if (!vectors_ || !vectors_->is_enabled()) return;

    const double latency_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at)
            .count();
    vectors_->record_tool_outcome(tool, success, latency_ms);
}

void AssistantOrchestrator::poll_browser_stt_transcriptions() {
    LOG_BROWSER_STT("Polling thread started");
    
    while (browser_stt_polling_active_ && running_) {
        std::string transcription = web_speech_->get_transcription();
        
        if (!transcription.empty()) {
            LOG_BROWSER_STT("Received transcription: " + transcription);
            this->handle_processed_text(transcription);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    LOG_BROWSER_STT("Polling thread stopped");
}

void AssistantOrchestrator::set_cava_visualizer_enabled(bool enabled) {
    if (cava_visualizer_) cava_visualizer_->set_enabled(enabled);
}

bool AssistantOrchestrator::is_cava_visualizer_enabled() const {
    return cava_visualizer_ ? cava_visualizer_->is_enabled() : false;
}

void AssistantOrchestrator::set_cava_visualizer_wake_word_enabled(bool enabled) {
    if (cava_visualizer_) cava_visualizer_->set_wake_word_enabled(enabled);
}

void AssistantOrchestrator::set_mic_gain(float gain) {
    mic_gain_ = std::max(0.1f, gain);
    if (audio_capture_) audio_capture_->set_mic_gain(gain);
}

