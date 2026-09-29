#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

struct AppConfig {
    std::string llama_model_path;
    std::string kokoro_voice;
    std::string language; // TTS/STT language code (e.g., "en", "de", "fr")
    float tts_speed; // TTS speech speed multiplier (1.0 = normal, 1.15 = balanced speed/accuracy)
    std::string avatar_type; // "sonny" or "iron-man"
    std::string system_prompt;
    bool use_browser_stt; // Use browser/Windows SAPI for STT instead of Python server
    bool use_browser_tts; // Use browser/Windows SAPI for TTS instead of Kokoro
    std::string browser_voice; // Selected browser voice name
    bool show_avatar; // Show the 3D avatar overlay
    std::string rag_embedding_model; // Embedding model for RAG (matches models\ dir name)
    bool wake_word_enabled; // Enable wake word detection from STT pipeline
    bool debug_mode; // Enable debug mode with performance timing logging
    bool enable_cava_visualizer; // Enable CAVA terminal visualizer for user speech
    bool console_listening_mode; // Inject console stdin text as transcribed input
    float mic_gain; // Microphone input gain multiplier (1.0 = no change, 2.0 = +6dB, 5.0 = +14dB)
    int rag_max_context_chars; // RAG context budget injected into the LLM prompt
    bool camera_feed_enabled; // Enable camera feed for vision (vision always enabled when camera is available)
    std::string camera_device_name; // Camera device name to use (empty = default/first camera)

    // ---- Peer metrics network (opt-in, content-free; see src/net/) ----
    bool vectors_enabled;         // master switch (default off)
    std::string vectors_mode;     // "off" | "lan" | "wan"
    std::string vectors_room;     // cohort name; only same-room peers exchange data
    std::string vectors_room_secret;   // optional shared secret -> HMAC-signed payloads
    std::string vectors_rendezvous_url; // optional relay/overlay address for "wan"
    bool vectors_share_tool_metrics;   // share tool success/latency counters
    int vectors_k_anonymity;      // peers required before a consensus value is shown
    float vectors_dp_sigma;       // Gaussian noise added to published aggregates

    // Default values
    AppConfig()
        : llama_model_path("")
        , kokoro_voice("af_heart")
        , language("en")
        , tts_speed(1.15f)
        , avatar_type("sonny")
        , system_prompt("You are Sonny, a professional butler AI. Respond concisely and directly. Be formal, respectful, and efficient. Provide accurate information without unnecessary elaboration. Execute tasks promptly. Avoid conversational filler. Speak as a distinguished butler serving a master. When you need current information or recent facts, use the web_search tool to find the answer.")
        , use_browser_stt(false)
        , use_browser_tts(false)
        , browser_voice("")
        , show_avatar(true)
        , rag_embedding_model("bge-large-en-v1.5")
        , wake_word_enabled(false)
        , debug_mode(false)
        , enable_cava_visualizer(true)
        , console_listening_mode(false)
        , mic_gain(1.0f)
        , rag_max_context_chars(1500)
        , camera_feed_enabled(true)
        , camera_device_name("")
        , vectors_enabled(true)
        , vectors_mode("wan")
        , vectors_room("global")
        , vectors_room_secret("")
         , vectors_rendezvous_url("http://vectorsync.ddns.net:47830/sonny/vectors/v1/publish")
        , vectors_share_tool_metrics(true)
        , vectors_k_anonymity(3)
        , vectors_dp_sigma(1.0f)
    {}
};

class ConfigManager {
public:
    static bool save_config(const AppConfig& config, const std::string& filepath = "config.ini");
    static bool load_config(AppConfig& config, const std::string& filepath = "config.ini");
    static std::string get_config_path();
    static bool is_first_run();
};
