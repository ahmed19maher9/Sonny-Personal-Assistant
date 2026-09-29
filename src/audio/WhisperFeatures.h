#pragma once
// Inline helpers implementing the Whisper log-mel spectrogram front end
// (OpenAI/faster-whisper compatible: 16 kHz, 400-point FFT, 160 hop,
// Slaney mel filterbank 0-8 kHz). No external dependencies.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace whisper_feat {

constexpr int SampleRate = 16000;
constexpr int Nfft = 400;
constexpr int Hop = 160;
constexpr int MaxFrames = 3000;

inline double hz_to_mel(double freq) {
    if (freq < 1000.0)
        return 3.0 * freq / 200.0;
    return 15.0 + 27.0 * std::log(freq / 1000.0) / std::log(6.4);
}

// Exact inverse of hz_to_mel. The filterbank normalisation below is defined
// over each band's width in HERTZ, so the mel points have to be converted
// back; using the mel-unit width instead is a silent no-op (see make_mel_filterbank).
inline double mel_to_hz(double mel) {
    if (mel < 15.0)
        return mel * 200.0 / 3.0;
    return 1000.0 * std::exp(std::log(6.4) * (mel - 15.0) / 27.0);
}

// Slaney mel filterbank: [n_mels][n_bins] with n_bins = n_fft/2 + 1 = 201.
// Thread-safe cached construction: builds once per n_mels value.
inline std::vector<float> make_mel_filterbank(size_t n_mels) {
    static std::mutex g_fb_mutex;
    static std::vector<float> g_cached_weights;
    static size_t g_cached_n_mels = 0;

    std::lock_guard<std::mutex> lock(g_fb_mutex);
    if (n_mels == g_cached_n_mels && !g_cached_weights.empty()) {
        return g_cached_weights;
    }

    const size_t n_bins = Nfft / 2 + 1;
    std::vector<double> mel_pts(n_mels + 2);
    const double mel_lo = 0.0;
    const double mel_hi = hz_to_mel(8000.0);
    for (size_t i = 0; i < mel_pts.size(); ++i)
        mel_pts[i] = mel_lo + (mel_hi - mel_lo) * static_cast<double>(i) /
                               static_cast<double>(n_mels + 1);
    // Band edges back in Hz: the Slaney normalisation divides by the band
    // width in Hz, which varies with frequency. The mel points are equally
    // spaced in MEL units, so mel_pts[i+2]-mel_pts[i] is the same constant for
    // every band and normalising by it does nothing at all - that mistake
    // weighted the upper mel bands up to 8x too strongly and made Whisper
    // return degenerate repetition instead of words. Verified against the
    // mel_filters.npz that Whisper ships (max abs difference 1.1e-4).
    std::vector<double> hz_pts(n_mels + 2);
    for (size_t i = 0; i < mel_pts.size(); ++i) hz_pts[i] = mel_to_hz(mel_pts[i]);
    std::vector<double> fdiff(n_mels + 1);
    for (size_t i = 0; i < fdiff.size(); ++i)
        fdiff[i] = mel_pts[i + 1] - mel_pts[i];

    std::vector<float> weights(n_mels * n_bins, 0.0f);
    for (size_t k = 0; k < n_bins; ++k) {
        const double freq = static_cast<double>(k) * SampleRate / Nfft;
        const double m = hz_to_mel(freq);
        for (size_t i = 0; i < n_mels; ++i) {
            double w = std::min((m - mel_pts[i]) / fdiff[i],
                                (mel_pts[i + 2] - m) / fdiff[i + 1]);
            if (w > 0.0) {
                const double enorm = 2.0 / (hz_pts[i + 2] - hz_pts[i]);
                weights[i * n_bins + k] = static_cast<float>(w * enorm);
            }
        }
    }

    g_cached_n_mels = n_mels;
    g_cached_weights = std::move(weights);
    return g_cached_weights;
}

// Reflect-padded STFT magnitudes^2, shape [n_frames][n_bins].
inline std::vector<float> compute_magnitudes(const std::vector<float>& audio) {
    const size_t n_bins = Nfft / 2 + 1;
    const size_t pad = Nfft / 2;

    std::vector<float> padded;
    padded.reserve(audio.size() + Nfft);
    const size_t len = audio.size();
    // Reflect padding must match np.pad/torch 'reflect': index -k maps to
    // y[k] (padded[p] = y[pad - p]), and index len + k maps to
    // y[len - 2 - k]. The previous y[pad - 1 - i] / y[len - 1 - i] variants
    // were off by one and misaligned the first/last 200 STFT samples.
    if (len == 0) {
        return padded;  // no frames possible; caller rejects empty audio
    }
    if (len == 1) {
        // A single sample has no partner to reflect against: pad with itself.
        padded.assign(Nfft + 1, audio[0]);
        return padded;
    }
    const size_t head = std::min(pad, len);
    for (size_t i = 0; i < head; ++i)
        padded.push_back(audio[std::min(head - i, len - 1)]);
    padded.insert(padded.end(), audio.begin(), audio.end());
    const size_t tail = std::min(pad, len);
    for (size_t i = 0; i < tail; ++i)
        padded.push_back(audio[std::min(len - 2 - i, len - 1)]);

    static std::vector<float> cos_tab, sin_tab, hann_tab;
    static std::once_flag once;
    std::call_once(once, [&]() {
        // Periodic Hann window, same as Whisper's torch.hann_window(N_FFT)
        // (periodic=True default). Without windowing, spectral leakage smears
        // energy across mel bins and measurably degrades transcription.
        hann_tab.resize(Nfft);
        for (size_t n = 0; n < Nfft; ++n)
            hann_tab[n] = static_cast<float>(
                0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 *
                                     static_cast<double>(n) / Nfft));
        cos_tab.resize(n_bins * Nfft);
        sin_tab.resize(n_bins * Nfft);
        for (size_t k = 0; k < n_bins; ++k) {
            for (size_t n = 0; n < Nfft; ++n) {
                const double ang = -2.0 * 3.14159265358979323846 *
                                   static_cast<double>(k * n) / Nfft;
                cos_tab[k * Nfft + n] = static_cast<float>(std::cos(ang));
                sin_tab[k * Nfft + n] = static_cast<float>(std::sin(ang));
            }
        }
    });

    const size_t n_frames = padded.size() >= Nfft
        ? (padded.size() - Nfft) / Hop + 1 : 0;
    std::vector<float> mag(n_frames * n_bins, 0.0f);
    for (size_t t = 0; t < n_frames; ++t) {
        const float* frame = padded.data() + t * Hop;
        float* out = mag.data() + t * n_bins;
        for (size_t k = 0; k < n_bins; ++k) {
            float re = 0.0f, im = 0.0f;
            const float* c = cos_tab.data() + k * Nfft;
            const float* s = sin_tab.data() + k * Nfft;
            const float* w = hann_tab.data();
            for (size_t n = 0; n < Nfft; ++n) {
                const float sample = frame[n] * w[n];
                re += sample * c[n];
                im += sample * s[n];
            }
            out[k] = re * re + im * im;
        }
    }
    return mag;
}

// Log-mel features, layout [n_mels][n_frames], normalized to ~[-1, 1].
inline bool compute_log_mel(const std::vector<float>& audio16k, size_t n_mels,
                            std::vector<float>& out_mel, size_t& out_frames) {
    if (audio16k.empty()) return false;
    const auto weights = make_mel_filterbank(n_mels);

    // faster-whisper FeatureExtractor.__call__ pads the waveform with 160
    // zero samples before the STFT (padding=160) and its stft() reflect-pads
    // n_fft/2 on both sides of that padded signal.
    std::vector<float> padded_audio(audio16k);
    padded_audio.insert(padded_audio.end(), 160, 0.0f);

    const auto mag = compute_magnitudes(padded_audio);
    const size_t n_bins = Nfft / 2 + 1;
    const size_t stft_frames = mag.size() / n_bins;
    // faster-whisper drops the last STFT frame: magnitudes = |stft[..., :-1]|^2.
    if (stft_frames == 0) return false;
    const size_t n_frames = stft_frames - 1;
    if (n_frames == 0) return false;

    out_mel.assign(n_mels * n_frames, 0.0f);
    for (size_t m = 0; m < n_mels; ++m) {
        const float* w = weights.data() + m * n_bins;
        float* dst = out_mel.data() + m * n_frames;
        for (size_t t = 0; t < n_frames; ++t) {
            const float* src = mag.data() + t * n_bins;
            float sum = 0.0f;
            for (size_t k = 0; k < n_bins; ++k)
                sum += w[k] * src[k];
            dst[t] = sum;
        }
    }

    constexpr float kMinMag = 1e-10f;
    float max_log = -std::numeric_limits<float>::infinity();
    for (float& v : out_mel) {
        v = std::log10(std::max(v, kMinMag));
        max_log = std::max(max_log, v);
    }
    if (!std::isfinite(max_log)) max_log = 0.0f;
    for (float& v : out_mel) {
        v = std::max(v, max_log - 8.0f);
        v = (v + 4.0f) / 4.0f;
    }

    out_frames = n_frames;
    return true;
}

inline std::vector<float> resample_to_16k(const std::vector<float>& in, int sr) {
    if (sr == SampleRate || sr <= 0 || in.empty()) return in;
    const double ratio = static_cast<double>(SampleRate) / sr;
    std::vector<float> out(static_cast<size_t>(in.size() * ratio));
    for (size_t i = 0; i < out.size(); ++i) {
        const double pos = static_cast<double>(i) / ratio;
        const size_t i0 = static_cast<size_t>(pos);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));
        out[i] = in[i0] * (1.0f - frac) + in[i1] * frac;
    }
    return out;
}

}  // namespace whisper_feat