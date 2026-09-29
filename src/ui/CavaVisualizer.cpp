#include "CavaVisualizer.h"
#include "Logger.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Jarvis {

namespace {

// Eighth-block characters in UTF-8
const char* kBlocks[9] = {
    " ",          // 0: space
    "\xE2\x96\x81", // 1: 
    "\xE2\x96\x82", // 2: ▂
    "\xE2\x96\x83", // 3: ▃
    "\xE2\x96\x84", // 4: ▄
    "\xE2\x96\x85", // 5: ▅
    "\xE2\x96\x86", // 6: ▆
    "\xE2\x96\x87", // 7: ▇
    "\xE2\x96\x88"  // 8: █
};

// Compute ANSI 24-bit color code for bar position i out of total N
std::string get_bar_color(size_t i, size_t total) {
    float t = (total <= 1) ? 0.0f : static_cast<float>(i) / static_cast<float>(total - 1);
    // Gradient: Cyan (0, 230, 255) -> Purple (130, 100, 255) -> Magenta/Pink (255, 60, 190)
    int r, g, b;
    if (t < 0.5f) {
        float sub = t / 0.5f;
        r = static_cast<int>(0 + 130 * sub);
        g = static_cast<int>(230 - 130 * sub);
        b = 255;
    } else {
        float sub = (t - 0.5f) / 0.5f;
        r = static_cast<int>(130 + 125 * sub);
        g = static_cast<int>(100 - 40 * sub);
        b = static_cast<int>(255 - 65 * sub);
    }
    return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

} // anonymous namespace

std::mutex& CavaVisualizer::get_console_mutex() {
    static std::mutex s_console_mutex;
    return s_console_mutex;
}

void CavaVisualizer::clear_terminal_line() {
    std::lock_guard<std::mutex> lock(get_console_mutex());
    std::cout << "\r\033[K" << std::flush;
}

// The terminal caret sits at the line start and visibly flashes with every
// erase/rewrite of the canvas line - like someone pressing a key
// continuously. Hide it while the canvas renders, restore it afterwards.
void CavaVisualizer::hide_caret() {
    std::lock_guard<std::mutex> lock(get_console_mutex());
    if (caret_hidden_) return;
    std::cout << "\033[?25l" << std::flush;
    caret_hidden_ = true;
}

void CavaVisualizer::show_caret() {
    std::lock_guard<std::mutex> lock(get_console_mutex());
    if (!caret_hidden_) return;
    std::cout << "\033[?25h" << std::flush;
    caret_hidden_ = false;
}

CavaVisualizer::CavaVisualizer(size_t bar_count, int sample_rate)
    : bar_count_(bar_count), sample_rate_(sample_rate) {
    if (bar_count_ < 8) bar_count_ = 8;
    if (bar_count_ > 64) bar_count_ = 64;

    audio_buffer_.resize(FFT_SIZE * 4, 0.0f);
    bar_heights_.assign(bar_count_, 0.0f);
    falloff_speeds_.assign(bar_count_, 0.0f);
    peak_bars_.assign(bar_count_, 0.0f);

    // Precompute Hann window
    hann_window_.resize(FFT_SIZE);
    for (size_t n = 0; n < FFT_SIZE; ++n) {
        hann_window_[n] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * n / (FFT_SIZE - 1)));
    }

    // Precompute Bit-Reversal indices for Radix-2 FFT
    bit_reverse_.resize(FFT_SIZE);
    size_t levels = 0;
    while ((1ULL << levels) < FFT_SIZE) levels++;
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        size_t rev = 0;
        for (size_t j = 0; j < levels; ++j) {
            if ((i >> j) & 1) {
                rev |= (1ULL << (levels - 1 - j));
            }
        }
        bit_reverse_[i] = rev;
    }

    // Precompute Twiddle factors
    twiddle_cos_.resize(FFT_SIZE / 2);
    twiddle_sin_.resize(FFT_SIZE / 2);
    for (size_t k = 0; k < FFT_SIZE / 2; ++k) {
        float angle = -2.0f * static_cast<float>(M_PI) * k / FFT_SIZE;
        twiddle_cos_[k] = std::cos(angle);
        twiddle_sin_[k] = std::sin(angle);
    }

    // Precompute Logarithmic Frequency Bands (tuned for human speech 80 Hz - 4000 Hz)
    const float min_freq = 80.0f;
    const float max_freq = 4000.0f;
    const float bin_res = static_cast<float>(sample_rate_) / FFT_SIZE; // e.g. 16000 / 512 = 31.25 Hz

    bands_.resize(bar_count_);
    for (size_t i = 0; i < bar_count_; ++i) {
        float f_start = min_freq * std::pow(max_freq / min_freq, static_cast<float>(i) / bar_count_);
        float f_end = min_freq * std::pow(max_freq / min_freq, static_cast<float>(i + 1) / bar_count_);
        
        size_t b_start = static_cast<size_t>(f_start / bin_res);
        size_t b_end = static_cast<size_t>(std::ceil(f_end / bin_res));
        if (b_end <= b_start) b_end = b_start + 1;
        if (b_start >= FFT_SIZE / 2) b_start = FFT_SIZE / 2 - 1;
        if (b_end > FFT_SIZE / 2) b_end = FFT_SIZE / 2;

        bands_[i] = { b_start, b_end };
    }
}

CavaVisualizer::~CavaVisualizer() {
    stop();
}

void CavaVisualizer::start() {
    if (running_.exchange(true)) return;
    last_rendered_line_.clear();
    last_console_repaint_ = std::chrono::steady_clock::now() -
                            std::chrono::milliseconds(100);
    hide_caret();
    render_thread_ = std::thread(&CavaVisualizer::render_loop, this);
}

void CavaVisualizer::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (render_thread_.joinable()) {
        render_thread_.join();
    }
    clear_terminal_line();
    show_caret();
}

void CavaVisualizer::set_enabled(bool enabled) {
    enabled_.store(enabled);
    if (!enabled) {
        clear_terminal_line();
        show_caret();
    }
}

void CavaVisualizer::set_speaking(bool speaking) {
    is_speaking_.store(speaking);
}

void CavaVisualizer::feed_audio(const std::vector<float>& samples) {
    if (!enabled_.load() || samples.empty()) return;

    std::lock_guard<std::mutex> lock(audio_mutex_);
    for (float s : samples) {
        audio_buffer_[buffer_write_pos_] = s;
        buffer_write_pos_ = (buffer_write_pos_ + 1) % audio_buffer_.size();
        if (buffer_available_ < audio_buffer_.size()) {
            buffer_available_++;
        }
    }
    new_samples_arrived_.fetch_add(samples.size());
}

void CavaVisualizer::compute_fft(const float* in, float* out_mag, size_t n) {
    // In-place Radix-2 FFT
    std::vector<float> real(n), imag(n, 0.0f);

    // Apply Hann window and bit-reversal reordering
    for (size_t i = 0; i < n; ++i) {
        real[bit_reverse_[i]] = in[i] * hann_window_[i];
    }

    // Butterfly stages
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1;
        size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < half; ++j) {
                size_t tw_idx = j * step;
                float u_r = real[i + j];
                float u_i = imag[i + j];
                float v_r = real[i + j + half] * twiddle_cos_[tw_idx] - imag[i + j + half] * twiddle_sin_[tw_idx];
                float v_i = real[i + j + half] * twiddle_sin_[tw_idx] + imag[i + j + half] * twiddle_cos_[tw_idx];

                real[i + j] = u_r + v_r;
                imag[i + j] = u_i + v_i;
                real[i + j + half] = u_r - v_r;
                imag[i + j + half] = u_i - v_i;
            }
        }
    }

    // Magnitude spectrum
    for (size_t i = 0; i < n / 2; ++i) {
        out_mag[i] = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2);
    }
}

void CavaVisualizer::update_bands(const float* mag, size_t n, float dt) {
    (void)n;
    constexpr float kGravity = 7.5f;       // CAVA gravity constant
    constexpr float kDecayFactor = 0.88f;   // Velocity damping
    constexpr float kCutoffThreshold = 0.003f;

    float current_frame_max = 0.0f;

    for (size_t i = 0; i < bar_count_; ++i) {
        const auto& b = bands_[i];
        float sum = 0.0f;
        size_t count = b.bin_end - b.bin_start;
        for (size_t bin = b.bin_start; bin < b.bin_end; ++bin) {
            sum += mag[bin] * mag[bin];
        }
        float rms = (count > 0) ? std::sqrt(sum / count) : 0.0f;

        // Equal-loudness tilt: boost higher frequencies slightly to match human ear perception
        float tilt = 1.0f + 1.2f * (static_cast<float>(i) / bar_count_);
        float val = rms * tilt;

        if (val > current_frame_max) current_frame_max = val;

        // Auto-gain tracking
        if (val > max_peak_energy_) {
            max_peak_energy_ = max_peak_energy_ * 0.92f + val * 0.08f;
        }

        // Noise gate
        if (val < noise_floor_ || val < kCutoffThreshold) {
            val = 0.0f;
        }

        // Decibel / logarithmic scale mapping
        float target = 0.0f;
        if (val > 1e-5f) {
            float db = 20.0f * std::log10(val);
            constexpr float kMinDb = -44.0f;
            constexpr float kMaxDb = -12.0f;
            if (db > kMinDb) {
                target = (db - kMinDb) / (kMaxDb - kMinDb);
                if (target > 1.0f) target = 1.0f;
                if (target < 0.0f) target = 0.0f;
            }
        }

        // CAVA Physics: Instant attack, gravity decay
        if (target >= bar_heights_[i]) {
            bar_heights_[i] = target;
            falloff_speeds_[i] = 0.0f;
        } else {
            falloff_speeds_[i] += kGravity * dt;
            bar_heights_[i] -= falloff_speeds_[i] * dt;
            falloff_speeds_[i] *= kDecayFactor;
            if (bar_heights_[i] < 0.0f) bar_heights_[i] = 0.0f;
        }
    }

    // Slowly decay peak sensitivity back down when speech is quiet
    max_peak_energy_ = std::max(0.04f, max_peak_energy_ * 0.995f);
}

std::string CavaVisualizer::format_cava_bar() {
    std::ostringstream ss;

    // Check if any bar is active
    float total_activity = 0.0f;
    for (float h : bar_heights_) total_activity += h;

    // Hysteresis + minimum hold (render thread only, no locking needed):
    // switching to SPEAKING requires a clearly active frame sum, reverting
    // to Listening a much quieter one, and either label sticks for at least
    // kMinHoldSec. This keeps the label steady instead of flickering with
    // frame-to-frame bar-height jitter.
    constexpr float kSpeakOnset = 0.6f;    // sum over 32 bars (avg bar ~0.02)
    constexpr float kSpeakRelease = 0.15f; // avg bar ~0.005 to revert
    constexpr float kMinHoldSec = 0.4f;

    const auto now = std::chrono::steady_clock::now();
    const float held_sec =
        std::chrono::duration<float>(now - label_state_since_).count();
    const bool want_speaking = label_speaking_
        ? (total_activity > kSpeakRelease)
        : (total_activity > kSpeakOnset);
    if (want_speaking != label_speaking_ && held_sec >= kMinHoldSec) {
        label_speaking_ = want_speaking;
        label_state_since_ = now;
    }

    if (label_speaking_) {
        ss << "\033[1;32m[🎤 SPEAKING]\033[0m ";
    } else {
        if (wake_word_enabled_) {
            ss << "\033[90m[🎤 Listening - Say 'Sonny' to activate]\033[0m ";
        } else {
            ss << "\033[90m[🎤 Listening]\033[0m ";
        }
    }

    // Render bars
    for (size_t i = 0; i < bar_count_; ++i) {
        float h = bar_heights_[i];
        int level = static_cast<int>(std::round(h * 8.0f));
        if (level < 0) level = 0;
        if (level > 8) level = 8;

        if (level == 0) {
            ss << "\033[90m \033[0m"; // subtle resting dot or blank
        } else {
            ss << get_bar_color(i, bar_count_) << kBlocks[level] << "\033[0m";
        }
    }

    return ss.str();
}

void CavaVisualizer::render_loop() {
    float fft_input[FFT_SIZE];
    float fft_mag[FFT_SIZE / 2];

    auto last_time = std::chrono::steady_clock::now();

    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt <= 0.0f || dt > 0.1f) dt = 0.022f; // Clamp delta time

        bool has_data = false;
        size_t new_samples = new_samples_arrived_.exchange(0);
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (new_samples > 0 && buffer_available_ >= FFT_SIZE) {
                size_t start_pos = (buffer_write_pos_ + audio_buffer_.size() - FFT_SIZE) % audio_buffer_.size();
                for (size_t i = 0; i < FFT_SIZE; ++i) {
                    fft_input[i] = audio_buffer_[(start_pos + i) % audio_buffer_.size()];
                }
                has_data = true;
            }
        }

        if (has_data) {
            compute_fft(fft_input, fft_mag, FFT_SIZE);
            update_bands(fft_mag, FFT_SIZE / 2, dt);
        } else {
            // Decay smoothly when no new audio is coming in
            for (size_t i = 0; i < bar_count_; ++i) {
                falloff_speeds_[i] += 7.5f * dt;
                bar_heights_[i] = std::max(0.0f, bar_heights_[i] - falloff_speeds_[i] * dt);
            }
        }

        if (enabled_.load()) {
            std::string line = format_cava_bar();

            // Repaint only when the content changed AND at most ~20 FPS.
            // A full \r\033[K erase + rewrite of every idle frame at 45 FPS
            // makes the terminal line flash; an unchanged frame now costs
            // nothing.
            const auto repaint_now = std::chrono::steady_clock::now();
            const bool repaint_due = std::chrono::duration<float>(
                repaint_now - last_console_repaint_).count() >= 0.05f;
            if (repaint_due && line != last_rendered_line_) {
                {
                    std::lock_guard<std::mutex> console_lock(get_console_mutex());
                    std::cout << "\r\033[K" << line << std::flush;
                }
                last_rendered_line_ = std::move(line);
                last_console_repaint_ = repaint_now;
            }
        }

        // Target ~45 FPS (~22 ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(22));
    }

    clear_terminal_line();
}

} // namespace Jarvis
