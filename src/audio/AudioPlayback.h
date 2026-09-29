#pragma once

#include <vector>
#include <string>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();
    void stop_playback();
    // Initialize audio playback
    bool initialize(int sample_rate = 24000);

    // Play audio data (float PCM)
    bool play(const std::vector<float>& audio_data);

    // Play audio data and wait for completion
    bool play_sync(const std::vector<float>& audio_data);

    // Stop playback
    void stop();

    // Check if playing
    bool is_playing() const { return playing_; }

    // Save audio to WAV file
    bool save_to_wav(const std::vector<float>& audio_data, const std::string& filename, int sample_rate = 24000);

    // Play WAV file directly
    bool play_file(const std::string& filename);

private:
    bool ensure_device(int sample_rate);
    void close_device();

    bool initialized_;
    std::atomic<bool> playing_;
    int sample_rate_;
    std::atomic<bool> m_stop_requested_;
    HWAVEOUT audio_handle_;
    bool device_ready_;
    int device_sample_rate_;
    std::vector<short> active_pcm_buffer_;
};
