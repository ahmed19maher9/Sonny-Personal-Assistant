#pragma once

#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <cmath>
#include <windows.h>
#include <mmsystem.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

class AudioCapture {
public:
    using AudioCallback = std::function<void(const std::vector<float>&)>;

    AudioCapture();
    ~AudioCapture();

    // Initialize audio capture
    bool initialize(int sample_rate = 16000);

    // Start capturing audio
    bool start(AudioCallback callback);

    // Stop capturing audio
    void stop();

    // Check if capturing
    bool is_capturing() const { return capturing_; }

    // Get sample rate
    int get_sample_rate() const { return sample_rate_; }

    // Set microphone input gain (1.0 = unity, 2.0 = +6dB, etc.)
    void set_mic_gain(float gain) { mic_gain_ = std::max(0.1f, gain); }
    float get_mic_gain() const { return mic_gain_; }

    // Mute/unmute audio capture. When muted, captured audio is discarded
    // (not passed to the callback). This prevents the assistant from
    // hearing its own voice through the speakers (feedback loop).
    void set_muted(bool muted) { muted_ = muted; }
    bool is_muted() const { return muted_; }

    // Enable/disable the built-in voice gate. When disabled, all captured
    // audio is forwarded to the callback so an external engine (e.g.
    // The native VAD engine can decide what is speech.
    void set_voice_gate_enabled(bool enabled) { voice_gate_enabled_ = enabled; }
    bool is_voice_gate_enabled() const { return voice_gate_enabled_; }

    // Reset the voice gate state (clears any buffered audio that might
    // have been accumulated during mute).
    void reset_voice_gate_state() { reset_voice_gate(); }

private:
    bool initialized_;
    bool capturing_;
    int sample_rate_;
    std::atomic<bool> should_stop_;
    std::atomic<bool> muted_;

    // WASAPI capture state
    IMMDeviceEnumerator* device_enumerator_ = nullptr;
    IMMDevice* capture_device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    HANDLE capture_event_ = nullptr;
    WAVEFORMATEX* capture_format_ = nullptr;
    bool wasapi_ready_ = false;
    std::thread capture_thread_;

    // Double-buffering: use 4 buffers for continuous capture (more headroom)
    static const int NUM_BUFFERS = 4;
    std::vector<short> raw_buffers_[NUM_BUFFERS];
    WAVEHDR wave_headers_[NUM_BUFFERS];

    // Voice-only gate state
    std::vector<float> pending_speech_buffer_;
    int consecutive_voice_frames_ = 0;
    int consecutive_nonvoice_frames_ = 0;
    float noise_floor_rms_ = 0.001f;
    bool speech_segment_active_ = false;
    std::vector<float> enrolled_voice_features_;
    bool has_enrolled_voice_ = false;
    int enrolled_frame_count_ = 0;
    std::vector<float> pending_segment_features_;
    int pending_segment_frames_ = 0;
    bool voice_gate_enabled_ = false;
    float mic_gain_ = 1.0f;

    void capture_loop(AudioCallback callback);
    bool start_wavein();
    bool open_capture_device(IMMDevice* device);   // full WASAPI init for a device
    void try_switch_capture_device();              // fallback when default mic is silent
    static std::wstring get_device_friendly_name(IMMDevice* device);
    void stop_wavein();
    std::vector<float> compute_voice_features(const std::vector<float>& frame) const;
    std::vector<float> resample_audio(const std::vector<float>& input, int source_rate, int target_rate) const;
    float compare_voice_features(const std::vector<float>& a, const std::vector<float>& b) const;
    void update_enrollment(const std::vector<float>& features);
    void accumulate_segment_features(const std::vector<float>& features);
    bool is_voice_like_frame(const std::vector<float>& frame) const;
    void reset_voice_gate();
    void flush_speech_segment(AudioCallback callback);
};