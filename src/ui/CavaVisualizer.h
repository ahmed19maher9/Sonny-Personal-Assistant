#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>

namespace Jarvis {

class CavaVisualizer {
public:
    CavaVisualizer(size_t bar_count = 32, int sample_rate = 16000);
    ~CavaVisualizer();

    // Start / stop the rendering thread
    void start();
    void stop();

    // Feed real-time microphone samples (float [-1.0, 1.0])
    void feed_audio(const std::vector<float>& samples);

    // Controls
    void set_enabled(bool enabled);
    bool is_enabled() const { return enabled_.load(); }
    void set_speaking(bool speaking);
    void set_wake_word_enabled(bool enabled) { wake_word_enabled_ = enabled; }

    // Clear the active visualizer line in the console
    static void clear_terminal_line();

    // Global console mutex for coordinating with Logger
    static std::mutex& get_console_mutex();

private:
    void render_loop();
    void compute_fft(const float* in, float* out_mag, size_t n);
    void update_bands(const float* mag, size_t n, float dt);
    std::string format_cava_bar();

    size_t bar_count_;
    int sample_rate_;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> running_{false};
    std::atomic<bool> is_speaking_{false};
    std::atomic<bool> wake_word_enabled_{false};

    // Audio input ring buffer
    std::mutex audio_mutex_;
    std::vector<float> audio_buffer_;
    size_t buffer_write_pos_ = 0;
    size_t buffer_available_ = 0;
    std::atomic<size_t> new_samples_arrived_{0};
    static constexpr size_t FFT_SIZE = 512;

    // Precomputed FFT tables
    std::vector<float> hann_window_;
    std::vector<size_t> bit_reverse_;
    std::vector<float> twiddle_cos_;
    std::vector<float> twiddle_sin_;

    // Band frequency mapping
    struct BandInfo {
        size_t bin_start;
        size_t bin_end;
    };
    std::vector<BandInfo> bands_;

    // CAVA physics state per bar
    std::vector<float> bar_heights_;      // [0.0, 1.0]
    std::vector<float> falloff_speeds_;
    std::vector<float> peak_bars_;

    // Listening/SPEAKING label state (render thread only). Hysteresis +
    // minimum hold: bar heights jitter frame-to-frame near the decision
    // threshold, so a bare per-frame comparison makes the label flip at
    // ~45 FPS (flickering).
    bool label_speaking_ = false;
    std::chrono::steady_clock::time_point label_state_since_ =
        std::chrono::steady_clock::now();

    // Console repaint state (render thread only). The line is repainted only
    // when its content changed and at most ~20 FPS; repainting identical
    // frames at 45 FPS makes the terminal caret flash like a key is being
    // pressed continuously. The caret is hidden while rendering (ANSI
    // DECSCUSR-style \033[?25l) and restored on stop/disable.
    std::string last_rendered_line_;
    std::chrono::steady_clock::time_point last_console_repaint_ =
        std::chrono::steady_clock::now();
    bool caret_hidden_ = false;

    void hide_caret();
    void show_caret();

    // Dynamic auto-gain
    float max_peak_energy_ = 0.05f;
    float noise_floor_ = 0.002f;

    // Render thread
    std::thread render_thread_;
    std::condition_variable cv_;
};

} // namespace Jarvis
