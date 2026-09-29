#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>

struct VoiceInfo {
    std::string name;
    std::string lang;
    bool default_voice;
};

class WebSpeechWrapper {
public:
    WebSpeechWrapper();
    ~WebSpeechWrapper();

    // Initialize the web speech API wrapper
    bool initialize(const std::string& html_path = "resources/web/web_speech.html");

    // Start the web server and speech recognition
    bool start();

    // Stop the web server and speech recognition
    void stop();

    // Speak text using TTS
    bool speak(const std::string& text, const std::string& voice_name = "");

    // Get the latest transcription (non-blocking)
    std::string get_transcription();

    // Check if initialized
    bool is_initialized() const { return initialized_; }

    // Check if running
    bool is_running() const { return running_; }

    // Get available voices
    static std::vector<VoiceInfo> get_available_voices();
    
    // Get available voices from the web page (non-static, requires server running)
    std::vector<VoiceInfo> get_web_voices();

    // Get the server port
    int get_server_port() const { return server_port_; }

private:
    bool initialized_;
    std::atomic<bool> running_;
    int server_port_;
    std::string html_path_;
    
    // Server thread
    std::thread server_thread_;
    std::atomic<bool> server_thread_running_;
    
    // Transcription queue
    std::queue<std::string> transcription_queue_;
    std::mutex queue_mutex_;
    
    // Voices queue (received from web page)
    std::queue<VoiceInfo> voices_queue_;
    std::mutex voices_mutex_;
    
    // TTS queue
    std::queue<std::string> tts_queue_;
    std::queue<std::string> tts_voice_queue_;
    std::mutex tts_mutex_;
    
    // HTTP server implementation
    void run_http_server();
    bool handle_request(const std::string& request, std::string& response);
    std::string get_http_response(const std::string& content, const std::string& content_type = "text/html", int status_code = 200) const;
    std::string get_cors_response(const std::string& content) const;
};
