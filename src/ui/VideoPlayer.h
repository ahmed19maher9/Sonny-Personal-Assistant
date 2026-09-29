#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <windows.h>

// Forward declarations for Media Foundation
struct IMFSourceReader;

class AudioPlayback; // Forward declaration

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    bool initialize();
    bool play_file(const std::string& video_path, bool wait_for_completion = false, bool loop = false, bool enable_chroma_key = true);
    void stop();
    bool is_playing() const { return playing_; }
    void set_completion_callback(std::function<void()> callback);
    void shutdown();
    void set_position(int x, int y, int width, int height);
    void set_chroma_key_color(uint8_t r, uint8_t g, uint8_t b, float tolerance = 0.3f);
    int get_video_width() const { return video_width_; }
    int get_video_height() const { return video_height_; }
    bool get_video_dimensions(const std::string& video_path, int& width, int& height);

    // Set audio playback device for playing video audio
    void set_audio_playback(AudioPlayback* audio) { audio_playback_ = audio; }

private:
    bool open_media_file(const std::string& path);
    void close_media_file();
    bool read_video_frame(std::vector<uint8_t>& out, int& width, int& height, long long& duration);
    bool read_audio_frame(std::vector<float>& audio_out);
    bool open_audio_stream();
    void close_audio_stream();
    void playback_thread_func(const std::string& path, bool loop);

    // NV12 -> BGRA conversion
    void nv12_to_bgra(const uint8_t* y_data, const uint8_t* uv_data, int w, int h, int y_stride, int uv_stride, uint8_t* bgra);

    // Chroma key applied in software
    void extract_foreground(const uint8_t* bgra, uint8_t* out_bgra, int w, int h);

    HWND hwnd_ = nullptr;
    HINSTANCE hinstance_ = nullptr;
    IMFSourceReader* source_reader_ = nullptr;
    bool window_created_ = false;
    bool mf_initialized_ = false;

    // Audio stream support
    bool audio_stream_available_ = false;
    int audio_sample_rate_ = 48000;
    int audio_channels_ = 2;
    AudioPlayback* audio_playback_ = nullptr;

    int window_x_ = 0, window_y_ = 0, window_width_ = 800, window_height_ = 450;
    int video_width_ = 0, video_height_ = 0;

    std::atomic<bool> initialized_{ false };
    std::atomic<bool> playing_{ false };
    std::atomic<bool> stop_requested_{ false };
    std::atomic<bool> enable_chroma_key_{ true };
    std::function<void()> completion_callback_;
    std::unique_ptr<std::thread> playback_thread_;
    std::mutex playback_mutex_;
    std::condition_variable playback_cv_;
    std::atomic<bool> playback_finished_{ false };

    struct ChromaKeyParams {
        float key_r = 0.0f, key_g = 1.0f, key_b = 0.0f;
        float tolerance = 1.0f; // Aggressive tolerance for complete WMV green screen removal
    };
    ChromaKeyParams chroma_key_;
};