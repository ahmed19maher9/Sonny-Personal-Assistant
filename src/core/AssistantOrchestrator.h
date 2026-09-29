#pragma once

#include "LlamaWrapper.h"
#include "STTEngineWrapper.h"
#include "KokoroWrapper.h"
#include "WebSpeechWrapper.h"
#include "AudioCapture.h"
#include "AudioPlayback.h"
#include "AvatarOverlay.h"
#include "VideoPlayer.h"
#include "CameraCapture.h"
#include "Tool.h"
#include "RagEngine.h"
#include "CavaVisualizer.h"
#include "VectorsConfig.h"
#include "VectorsSync.h"
#include <memory>
#include <string>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

class AssistantOrchestrator {
public:
    using StatusCallback = std::function<void(const std::string&)>;

    AssistantOrchestrator();
    ~AssistantOrchestrator();

    // Initialize all components
    bool initialize(
        const std::string& llama_model_path,
        const std::string& llama_server_path,
        const std::string& tts_model_dir,
        const std::string& stt_script_path = "",
        const std::string& rag_embedding_model_path = "all-MiniLM-L6-v2",
        const std::string& videos_base_path = "",
        const std::string& sounds_base_path = "",
        const std::string& mmproj_path = ""
    );

    // Start the assistant (begins listening)
    bool start(StatusCallback status_callback = nullptr);

    // Stop the assistant
    void stop();

    // Process audio manually (for testing)
    std::string process_audio(const std::vector<float>& audio_data);

    // Set system prompt for LLM
    void set_system_prompt(const std::string& prompt);
    
    // Get current system prompt
    const std::string& get_system_prompt() const { return system_prompt_; }
    void set_voice_name(const std::string& voice) { voice_name_ = voice; }
    void set_language(const std::string& lang) { language_ = lang; }
    void set_tts_speed(float speed) { tts_speed_ = speed; }
    
    // Set browser STT/TTS options
    void set_use_browser_stt(bool use) { use_browser_stt_ = use; }
    void set_use_browser_tts(bool use) { use_browser_tts_ = use; }
    void set_browser_voice(const std::string& voice) { browser_voice_ = voice; }
    
    // Enable/disable wake word detection
    void set_wake_word_enabled(bool enabled) { wake_word_enabled_ = enabled; }
    
    // Set native STT stack paths (CTranslate2 Whisper + Silero VAD).
    void set_stt_paths(const std::string& silero_vad,
                       const std::string& whisper_model_dir) {
        stt_silero_vad_ = silero_vad;
        stt_ct2_model_dir_ = whisper_model_dir;
    }
    void set_model_type(const std::string& type) { stt_model_type_ = type; }

    // openWakeWord ONNX models for the jarvis-style audio wake word.
    void set_wake_word_models(const std::string& wakeword_model,
                              const std::string& melspec_model,
                              const std::string& embedding_model) {
        stt_wakeword_model_ = wakeword_model;
        stt_melspec_model_ = melspec_model;
        stt_embedding_model_ = embedding_model;
    }
    
    // Enable/disable tools
    void enable_tools(bool enable) { tools_enabled_ = enable; }

    // Avatar overlay
    void set_avatar_path(const std::string& path) { avatar_path_ = path; }
    void set_show_avatar(bool show) { show_avatar_ = show; }
    void show_avatar() { if (avatar_) avatar_->show(); }
    void hide_avatar() { if (avatar_) avatar_->hide(); }
    bool is_avatar_visible() const { return avatar_ && avatar_->is_visible(); }
    void set_avatar_rotation_speed(float speed) { if (avatar_) avatar_->set_rotation_speed(speed); }

    // Check if running
    bool is_running() const { return running_; }

    // Inject text as if it were transcribed from speech (debug/listening mode)
    void process_console_input(const std::string& text) {
        handle_processed_text(text);
    }

    // CAVA speech visualizer in terminal
    void set_cava_visualizer_enabled(bool enabled);
    bool is_cava_visualizer_enabled() const;
    void set_cava_visualizer_wake_word_enabled(bool enabled);

    void set_mic_gain(float gain);

    // Vision support
    bool has_vision() const { return llama_ && llama_->has_vision(); }

    // Camera feed configuration: set enabled state and device name from settings
    void set_camera_feed_config(bool enabled, const std::string& device_name);

    // Camera feed toggle: when active, the next vision query captures from the
    // camera instead of a screen grab.
    void toggle_camera_feed(bool on);
    bool is_camera_feed_active() const { return camera_active_; }

    // Capture a screenshot of the primary display as RGB data.
    std::vector<unsigned char> capture_screenshot_rgb(int& out_width, int& out_height);

    // Access to the RAG knowledge base (tools use it for persistent user
    // knowledge such as registered media directories). May be null before
    // initialize() completes.
    Jarvis::RagEngine* get_rag_engine() const { return rag_engine_.get(); }

    // RAG context budget (characters) that may be injected into the LLM prompt.
    // The previous fixed 500-char gate discarded nearly every retrieval hit.
    void set_rag_max_context_chars(int chars) {
        rag_max_context_chars_ = chars;
        if (rag_engine_) rag_engine_->set_max_context_chars(chars);
    }

    // Peer metrics network. Disabled unless config.enabled is true and the mode
    // is "lan" or "wan"; see src/net/VectorsConfig.h for the gates.
    void set_vectors_config(const Jarvis::Vectors::VectorsConfig& config) {
        vectors_config_ = config;
    }
    std::string vectors_status() const {
        return vectors_ ? vectors_->status_json() : std::string("{\"enabled\":false}");
    }
    std::string mesh_json() const {
        if (!vectors_) return std::string("{\"v\":1,\"room\":\"\",\"node_id\":\"\",\"peers\":[]}");
        std::vector<Jarvis::Vectors::MeshPeerView> peers = vectors_->get_mesh_view();
        std::string room = vectors_config_.room;
        std::string nid = vectors_->node_id();
        std::string json = "{\"v\":1,\"room\":\"" + room + "\",\"node_id\":\"" + nid + "\",\"peers\":[";
        for (size_t i = 0; i < peers.size(); ++i) {
            const auto& p = peers[i];
            if (i > 0) json += ",";
            json += "{\"node_id\":\"" + p.node_id + "\",\"host\":\"" + p.host +
                    "\",\"port\":" + std::to_string(p.port) +
                    ",\"brain_model\":\"" + p.brain_model +
                    "\",\"embedding_model\":\"" + p.embedding_model +
                    "\",\"embedding_dim\":" + std::to_string(p.embedding_dim) +
                    ",\"compatible_embeddings\":" + (p.compatible_embeddings ? "true" : "false") +
                    ",\"compatible_lora\":" + (p.compatible_lora ? "true" : "false") +
                    ",\"last_seen_unix\":" + std::to_string(p.last_seen_unix) +
                    ",\"last_sync_unix\":" + std::to_string(p.last_sync_unix) +
                    ",\"sync_count_24h\":" + std::to_string(p.sync_count_24h) +
                    ",\"reputation\":" + std::to_string(p.reputation) +
                    ",\"we_pull_from_them\":" + (p.we_pull_from_them ? "true" : "false") +
                    ",\"they_pull_from_us\":" + (p.they_pull_from_us ? "true" : "false") +
                    "}";
        }
        json += "]}";
        return json;
    }

    // Publish a LoRA adapter to the vectors for cross-node
    // personalization (Chapter 15 of "Federated Learning Foundations and
    // Applications"). The adapter is a small GGUF file of diff tensors;
    // when applied via llama_set_adapters_lora(), it steers the model toward
    // a peer's local domain. Adapters are content-free by construction - they
    // are weights, not data.
    //
    // `name` is a free-form label (e.g. "medical", "legal", "code-review")
    // used for deduplication. `adapter_path` is the path to a GGUF LoRA
    // adapter file. The adapter bytes are read and base64-encoded for
    // transport; the wire format caps adapter size to prevent DoS.
    bool publish_lora_adapter(const std::string& name, const std::string& adapter_path);

    // Apply aggregated LoRA adapters from the vectors to the local
    // model. Called automatically on startup; can be called again to refresh.
    bool apply_peer_lora_adapters();

private:
    std::unique_ptr<STTEngineWrapper> stt_engine_;
    std::unique_ptr<LlamaWrapper> llama_;
    std::unique_ptr<KokoroWrapper> kokoro_;
    std::unique_ptr<WebSpeechWrapper> web_speech_;
    std::unique_ptr<AudioCapture> audio_capture_;
    std::unique_ptr<AudioPlayback> audio_playback_;
    std::unique_ptr<AvatarOverlay> avatar_;
    std::unique_ptr<VideoPlayer> video_player_;
    std::unique_ptr<Jarvis::CavaVisualizer> cava_visualizer_;
    std::unique_ptr<Jarvis::CameraCapture> camera_capture_;
    // Written by the tray toggle thread and read by the vision worker.
    std::atomic<bool> camera_active_{false};
    std::string current_camera_device_;
    bool video_player_ready_ = false;  // True only if VideoPlayer::initialize() succeeded
    bool show_avatar_ = true;
    std::string avatar_path_;

    std::atomic<bool> running_;
    std::atomic<bool> processing_;
    StatusCallback status_callback_;
    std::atomic<bool> audio_processing_thread_active_;
    std::atomic<bool> tools_enabled_ = false;
    std::atomic<bool> llm_ready_ = false;  // Track when LLM is ready to respond
    std::thread browser_stt_polling_thread_;
    std::atomic<bool> browser_stt_polling_active_ = false;
    std::thread python_stt_polling_thread_;
    std::atomic<bool> python_stt_polling_active_ = false;

    std::vector<float> audio_buffer_;
    std::mutex audio_mutex_;

    std::string system_prompt_;
    std::string cached_full_system_prompt_;  // Cached system prompt including tool schemas
    bool cached_tool_schemas_enabled_ = false;  // Whether the cached prompt includes tool schemas
    std::string voice_name_ = "af_nicole";
    std::string language_ = "en";
    float tts_speed_ = 1.15f;
    
    bool use_browser_stt_ = false;
    bool use_browser_tts_ = false;
    std::string browser_voice_ = "en-US";
    
    bool wake_word_enabled_ = false;
    
    float mic_gain_ = 1.0f;
    
    std::string videos_base_path_;
    std::string sounds_base_path_;
    
    // Native STT model paths
    std::string stt_silero_vad_;
    std::string stt_ct2_model_dir_;
    std::string stt_wakeword_model_;
    std::string stt_melspec_model_;
    std::string stt_embedding_model_;
    std::string stt_model_type_ = "nemo";
    
    void on_audio_captured(const std::vector<float>& audio_data);
    std::string transcribe_audio(const std::vector<float>& audio_data);
    // Image routing: when images are provided and vision is available,
    // the LLM call routes through generate_stream_with_images() instead of
    // the text-only generate_stream().
    std::string generate_response(const std::string& text, bool include_tool_schemas = true, bool stream_tts = true, bool is_first_call = true);
    std::string generate_response(const std::string& text,
        const std::vector<std::vector<unsigned char>>& images,
        int img_width, int img_height,
        bool include_tool_schemas = true, bool stream_tts = true, bool is_first_call = true);
    // Builds (once per tool-schemas flag change) the full LLM system prompt
    // (persona + tool schemas + file-explorer context) and pushes it into
    // LlamaWrapper. Keeping the string byte-identical across calls is what
    // enables the KV-cache prefix reuse that skips the ~2k-token system
    // prompt re-prefill on every turn.
    void refresh_llama_system_prompt(bool want_tool_schemas);
    void log_llm_inference_perf(long long llm_ms, const std::string& tag = "");
    // Tool-call follow-up recursion cap: the model is invited to retry failed
    // tool calls (e.g. dictation fixes), but an uncapped chain of retries
    // reads as a long stall (each round trip costs seconds of generation).
    static constexpr int kMaxToolFollowups = 2;
    std::atomic<int> tool_followup_depth_{0};
    void synthesize_and_play(const std::string& text);
    // Play a file while flagging the assistant as speaking (jarvis_ears.py
    // AEC: the STT wake-word handler ignores the input during playback).
    void play_file_with_aec(const std::string& path);
    std::string parse_llm_tool_call(const std::string& llm_output);
    std::string clean_response_for_output(const std::string& response);
    void poll_browser_stt_transcriptions();
    void handle_processed_text(const std::string& transcription);

    // Vision plumbing. capture_vision_rgb() prefers a live camera frame while the
    // camera feed is on and falls back to a screen grab when the camera has no
    // frame yet (or no webcam is present). is_vision_request() recognises the
    // spoken phrases that mean "use your eyes".
    std::vector<unsigned char> capture_vision_rgb(int& out_width, int& out_height, std::string* source_label);
    bool is_vision_request(const std::string& lower_text) const;

    std::unique_ptr<Jarvis::RagEngine> rag_engine_;
    bool rag_enabled_ = true;
    int rag_max_context_chars_ = Jarvis::RagEngine::kDefaultMaxContextChars;
    std::string rag_embedding_model_path_ = "all-MiniLM-L6-v2";

    // Peer metrics network (content-free tool statistics; see src/net/).
    std::unique_ptr<Jarvis::Vectors::VectorsSync> vectors_;
    Jarvis::Vectors::VectorsConfig vectors_config_;

    // Records one tool invocation for the vectors: tool name, outcome and
    // duration only - no query text, parameters or results.
    void record_tool_outcome(const std::string& tool,
                             bool success,
                             std::chrono::steady_clock::time_point started_at);
    
    std::string session_id_;
    std::string conversation_summary_;
    int conversation_turn_count_ = 0;
};