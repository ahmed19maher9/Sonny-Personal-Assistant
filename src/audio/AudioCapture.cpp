#include "AudioCapture.h"
#include <iostream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

static void save_wav_file(const std::string& path, const std::vector<float>& audio, int sample_rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return;

    uint32_t num_samples = static_cast<uint32_t>(audio.size());
    uint32_t byte_rate = static_cast<uint32_t>(sample_rate * 1 * 2);
    uint16_t block_align = 2;
    uint32_t data_size = num_samples * 2;
    uint32_t chunk_size = 36 + data_size;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&chunk_size), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    uint32_t subchunk1_size = 16;
    uint16_t audio_format = 1;
    uint16_t num_channels = 1;
    out.write(reinterpret_cast<const char*>(&subchunk1_size), 4);
    out.write(reinterpret_cast<const char*>(&audio_format), 2);
    out.write(reinterpret_cast<const char*>(&num_channels), 2);
    uint32_t sr = static_cast<uint32_t>(sample_rate);
    out.write(reinterpret_cast<const char*>(&sr), 4);
    out.write(reinterpret_cast<const char*>(&byte_rate), 4);
    out.write(reinterpret_cast<const char*>(&block_align), 2);
    uint16_t bits_per_sample = 16;
    out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_size), 4);

    for (float s : audio) {
        float clamped = std::max(-1.0f, std::min(1.0f, s));
        int16_t val = static_cast<int16_t>(clamped * 32767.0f);
        out.write(reinterpret_cast<const char*>(&val), 2);
    }
}

AudioCapture::AudioCapture() 
    : initialized_(false), capturing_(false), sample_rate_(16000), 
      should_stop_(false), muted_(false) {
}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::initialize(int sample_rate) {
    sample_rate_ = sample_rate;
    initialized_ = true;
    return true;
}

std::wstring AudioCapture::get_device_friendly_name(IMMDevice* device) {
    std::wstring name = L"(unknown device)";
    if (!device) return name;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal) {
            name = v.pwszVal;
        }
        PropVariantClear(&v);
        ps->Release();
    }
    return name;
}

bool AudioCapture::start_wavein() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "WASAPI: CoInitializeEx failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&device_enumerator_));
    if (FAILED(hr) || !device_enumerator_) {
        std::cerr << "WASAPI: CoCreateInstance failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = device_enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &capture_device_);
    if (FAILED(hr) || !capture_device_) {
        std::cerr << "WASAPI: GetDefaultAudioEndpoint failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // // std::wcout << L" [EAR] Opening default microphone: "
    ////             << get_device_friendly_name(capture_device_) << std::endl;

    return open_capture_device(capture_device_);
}

// Full WASAPI session setup for a specific capture device. On success the
// stream is running and wasapi_ready_ == true.
bool AudioCapture::open_capture_device(IMMDevice* device) {
    // Release any partially-initialized session from a previous attempt.
    if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (capture_event_) { CloseHandle(capture_event_); capture_event_ = nullptr; }
    if (capture_format_) { CoTaskMemFree(capture_format_); capture_format_ = nullptr; }
    wasapi_ready_ = false;

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || !audio_client_) {
        std::cerr << "WASAPI: Activate failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        std::cerr << "WASAPI: GetMixFormat failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    capture_format_ = nullptr;
    WAVEFORMATEX pcm_format = {};
    pcm_format.wFormatTag = WAVE_FORMAT_PCM;
    pcm_format.nChannels = 1;
    pcm_format.nSamplesPerSec = 16000;
    pcm_format.wBitsPerSample = 16;
    pcm_format.nBlockAlign = pcm_format.nChannels * (pcm_format.wBitsPerSample / 8);
    pcm_format.nAvgBytesPerSec = pcm_format.nSamplesPerSec * pcm_format.nBlockAlign;
    pcm_format.cbSize = 0;

    // jarvis_ears.py parity: PyAudio opens the mic directly as 16 kHz int16
    // mono and Windows/PortAudio performs the sample-rate conversion. Request
    // exactly that format and let the WASAPI audio engine do proper SRC
    // (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY) instead of the device mix format
    // plus channel averaging plus linear resampling.
    capture_format_ = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!capture_format_) {
        CoTaskMemFree(mix_format);
        std::cerr << "WASAPI: CoTaskMemAlloc failed for capture format" << std::endl;
        return false;
    }
    *capture_format_ = pcm_format;
    CoTaskMemFree(mix_format);
    mix_format = nullptr;
    // std::cout << " [EAR] Requesting 16 kHz mono 16-bit PCM capture (Windows SRC, jarvis parity)"
    //           << std::endl;


    // Headroom above the engine period: the capture thread competes with the
    // wake-word ONNX inference (which saturates the CPU with many threads) and
    // the spectrum FFT. A 10 ms buffer leaves no room for preemption, so the
    // shared engine overruns, drops/glitches packets (DATA_DISCONTINUITY) and
    // both the spectrum canvas and whisper receive distorted audio. 50 ms
    // absorbs CPU spikes; latency stays ~one period because every event drains
    // all pending packets.
    REFERENCE_TIME requested_duration = 500000; // 50 ms
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        requested_duration, requested_duration, capture_format_, nullptr);
    if (FAILED(hr)) {
        std::cerr << "WASAPI: Initialize failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    capture_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!capture_event_) {
        std::cerr << "WASAPI: CreateEvent failed with error " << GetLastError() << std::endl;
        return false;
    }

    hr = audio_client_->SetEventHandle(capture_event_);
    if (FAILED(hr)) {
        std::cerr << "WASAPI: SetEventHandle failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&capture_client_));
    if (FAILED(hr) || !capture_client_) {
        std::cerr << "WASAPI: GetService failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        std::cerr << "WASAPI: Start failed with HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    wasapi_ready_ = true;
    return true;
}

void AudioCapture::stop_wavein() {
    if (audio_client_) {
        audio_client_->Stop();
    }
    if (capture_client_) {
        capture_client_->Release();
        capture_client_ = nullptr;
    }
    if (audio_client_) {
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (capture_device_) {
        capture_device_->Release();
        capture_device_ = nullptr;
    }
    if (device_enumerator_) {
        device_enumerator_->Release();
        device_enumerator_ = nullptr;
    }
    if (capture_event_) {
        CloseHandle(capture_event_);
        capture_event_ = nullptr;
    }
    if (capture_format_) {
        CoTaskMemFree(capture_format_);
        capture_format_ = nullptr;
    }
    wasapi_ready_ = false;
    CoUninitialize();
}

// Called by capture_loop when the current mic delivers no packets for a
// while (e.g. a Bluetooth dongle whose headset is unplugged, which stays
// "Active" but never produces audio). Tears down the silent stream and
// opens the next ACTIVE capture endpoint, if any.
void AudioCapture::try_switch_capture_device() {
    std::wstring prev_id;
    if (capture_device_) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(capture_device_->GetId(&id)) && id) {
            prev_id = id;
            CoTaskMemFree(id);
        }
    }

    if (audio_client_) { audio_client_->Stop(); }
    if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (capture_event_) { CloseHandle(capture_event_); capture_event_ = nullptr; }
    if (capture_format_) { CoTaskMemFree(capture_format_); capture_format_ = nullptr; }
    wasapi_ready_ = false;
    if (capture_device_) { capture_device_->Release(); capture_device_ = nullptr; }

    if (!device_enumerator_) {
        return;
    }

    static bool advice_printed = false;
    // std::cerr << " [EAR] Microphone '" << std::string(prev_id.begin(), prev_id.end())
    //           << "' produced no data - trying other capture devices..." << std::endl;

    IMMDeviceCollection* coll = nullptr;
    if (FAILED(device_enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)) || !coll) {
        return;
    }
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d)) || !d) continue;
        std::wstring cand_id;
        LPWSTR cid = nullptr;
        if (SUCCEEDED(d->GetId(&cid)) && cid) {
            cand_id = cid;
            CoTaskMemFree(cid);
        }
        if (!cand_id.empty() && cand_id == prev_id) {
            d->Release();  // skip the device we know is silent
            continue;
        }
        if (open_capture_device(d)) {
            capture_device_ = d;  // keep reference on success
            // std::wcout << L" [EAR] Switched microphone to: "
            //            << get_device_friendly_name(d) << std::endl;
            advice_printed = false;
            coll->Release();
            return;
        }
        d->Release();
    }
    coll->Release();
    if (!advice_printed) {
        std::cerr << " [EAR] No other active microphone available. Sonny cannot hear you:"
                  << " connect a microphone (or enable it in Control Panel > Sound >"
                  << " Recording) and set it as the default recording device."
                  << " Retrying silently..." << std::endl;
        advice_printed = true;
    }
}

bool AudioCapture::start(AudioCallback callback) {
    if (!initialized_ || capturing_) {
        return false;
    }

    capturing_ = true;
    should_stop_ = false;

    // Start capture thread
    capture_thread_ = std::thread(&AudioCapture::capture_loop, this, callback);

    return true;
}

void AudioCapture::stop() {
    if (!capturing_) {
        return;
    }

    should_stop_ = true;
    capturing_ = false;

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    reset_voice_gate();
    stop_wavein();
}

std::vector<float> AudioCapture::compute_voice_features(const std::vector<float>& frame) const {
    std::vector<float> features(6, 0.0f);
    if (frame.empty()) {
        return features;
    }

    float sum_sq = 0.0f;
    float peak = 0.0f;
    int zero_crossings = 0;
    float spectral_centroid = 0.0f;
    float energy_200 = 0.0f;
    float energy_800 = 0.0f;
    float energy_2200 = 0.0f;
    float best_corr = 0.0f;
    int best_lag = 0;

    const float sample_rate = static_cast<float>(sample_rate_);
    for (size_t i = 0; i < frame.size(); ++i) {
        float sample = frame[i];
        peak = std::max(peak, std::abs(sample));
        sum_sq += sample * sample;
        if (i > 0 && ((frame[i - 1] >= 0.0f && sample < 0.0f) || (frame[i - 1] < 0.0f && sample >= 0.0f))) {
            ++zero_crossings;
        }

        const float x = sample;
        const float angle_200 = 2.0f * 3.14159265f * 200.0f * static_cast<float>(i) / sample_rate;
        const float angle_800 = 2.0f * 3.14159265f * 800.0f * static_cast<float>(i) / sample_rate;
        const float angle_2200 = 2.0f * 3.14159265f * 2200.0f * static_cast<float>(i) / sample_rate;
        energy_200 += std::abs(x * std::sin(angle_200));
        energy_800 += std::abs(x * std::sin(angle_800));
        energy_2200 += std::abs(x * std::sin(angle_2200));
        spectral_centroid += std::abs(sample) * static_cast<float>(i);
    }

    const float rms = std::sqrt(sum_sq / static_cast<float>(frame.size()));
    const float zcr = static_cast<float>(zero_crossings) / std::max(1, static_cast<int>(frame.size()) - 1);
    const float centroid = spectral_centroid / std::max(1.0f, sum_sq);
    const float band_ratio = (energy_800 + 1e-6f) / (energy_200 + 1e-6f);

    for (int lag = 40; lag < static_cast<int>(frame.size()) / 2; ++lag) {
        float corr = 0.0f;
        for (int i = 0; i + lag < static_cast<int>(frame.size()); ++i) {
            corr += frame[i] * frame[i + lag];
        }
        if (corr > best_corr) {
            best_corr = corr;
            best_lag = lag;
        }
    }

    const float pitch = best_lag > 0 ? sample_rate / static_cast<float>(best_lag) : 0.0f;
    const float harmonicity = best_corr > 0.0f ? std::min(1.0f, best_corr / (sum_sq + 1e-6f)) : 0.0f;

    features[0] = rms;
    features[1] = zcr;
    features[2] = std::min(1.0f, std::max(0.0f, centroid / static_cast<float>(frame.size())));
    features[3] = std::min(8.0f, std::max(0.0f, band_ratio));
    features[4] = std::min(1.0f, std::max(0.0f, pitch / 400.0f));
    features[5] = std::min(1.0f, std::max(0.0f, harmonicity));

    return features;
}

std::vector<float> AudioCapture::resample_audio(const std::vector<float>& input, int source_rate, int target_rate) const {
    if (input.empty() || source_rate <= 0 || target_rate <= 0 || source_rate == target_rate) {
        return input;
    }

    std::vector<float> output;
    if (input.size() == 1) {
        output.push_back(input[0]);
        return output;
    }

    output.resize(static_cast<size_t>((input.size() * target_rate) / source_rate) + 1);
    for (size_t i = 0; i < output.size(); ++i) {
        const float src_pos = static_cast<float>(i) * static_cast<float>(source_rate) / static_cast<float>(target_rate);
        const size_t idx0 = std::min<size_t>(static_cast<size_t>(std::floor(src_pos)), input.size() - 1);
        const size_t idx1 = std::min<size_t>(idx0 + 1, input.size() - 1);
        const float frac = src_pos - static_cast<float>(idx0);
        output[i] = input[idx0] + (input[idx1] - input[idx0]) * frac;
    }

    return output;
}

float AudioCapture::compare_voice_features(const std::vector<float>& a, const std::vector<float>& b) const {
    if (a.size() != b.size()) {
        return 999.0f;
    }

    const float rms_delta = std::abs(a[0] - b[0]) / (0.15f + std::abs(b[0]));
    const float zcr_delta = std::abs(a[1] - b[1]) / (0.08f + std::abs(b[1]));
    const float centroid_delta = std::abs(a[2] - b[2]) / (0.15f + std::abs(b[2]));
    const float band_delta = std::abs(a[3] - b[3]) / (1.0f + std::abs(b[3]));
    const float pitch_delta = std::abs(a[4] - b[4]) / (0.2f + std::abs(b[4]));
    const float harmonicity_delta = std::abs(a[5] - b[5]) / (0.2f + std::abs(b[5]));

    return rms_delta * 0.25f + zcr_delta * 0.2f + centroid_delta * 0.15f + band_delta * 0.1f + pitch_delta * 0.15f + harmonicity_delta * 0.15f;
}

void AudioCapture::update_enrollment(const std::vector<float>& features) {
    if (features.empty()) {
        return;
    }

    if (!has_enrolled_voice_) {
        enrolled_voice_features_ = features;
        enrolled_frame_count_ = 1;
        has_enrolled_voice_ = true;
        return;
    }

    for (size_t i = 0; i < features.size(); ++i) {
        enrolled_voice_features_[i] = (enrolled_voice_features_[i] * static_cast<float>(enrolled_frame_count_) + features[i]) /
            static_cast<float>(enrolled_frame_count_ + 1);
    }
    ++enrolled_frame_count_;
}

void AudioCapture::accumulate_segment_features(const std::vector<float>& features) {
    if (features.empty()) {
        return;
    }

    if (pending_segment_features_.empty()) {
        pending_segment_features_ = features;
    } else {
        for (size_t i = 0; i < features.size(); ++i) {
            pending_segment_features_[i] = (pending_segment_features_[i] * static_cast<float>(pending_segment_frames_) + features[i]) /
                static_cast<float>(pending_segment_frames_ + 1);
        }
    }
    ++pending_segment_frames_;
}

bool AudioCapture::is_voice_like_frame(const std::vector<float>& frame) const {
    if (frame.empty()) {
        return false;
    }

    const auto features = compute_voice_features(frame);
    const float rms = features[0];
    const float zcr = features[1];
    const float centroid = features[2];
    const float band_ratio = features[3];

    const bool generic_speech_like = rms > (noise_floor_rms_ * 2.5f) && rms < 0.45f &&
        zcr > 0.01f && zcr < 0.32f && centroid > 0.0005f && centroid < 0.4f &&
        band_ratio > 0.35f && band_ratio < 7.0f && features[4] > 0.08f && features[4] < 0.95f &&
        features[5] > 0.05f && features[5] < 0.95f;

    if (!generic_speech_like) {
        return false;
    }

    if (!has_enrolled_voice_) {
        return true;
    }

    const float similarity_score = compare_voice_features(features, enrolled_voice_features_);
    return similarity_score < 0.95f;
}

void AudioCapture::reset_voice_gate() {
    pending_speech_buffer_.clear();
    pending_segment_features_.clear();
    pending_segment_frames_ = 0;
    consecutive_voice_frames_ = 0;
    consecutive_nonvoice_frames_ = 0;
    speech_segment_active_ = false;
    noise_floor_rms_ = 0.001f;
}

void AudioCapture::flush_speech_segment(AudioCallback callback) {
    if (pending_speech_buffer_.empty()) {
        pending_segment_features_.clear();
        pending_segment_frames_ = 0;
        speech_segment_active_ = false;
        consecutive_voice_frames_ = 0;
        consecutive_nonvoice_frames_ = 0;
        return;
    }

    const size_t min_samples = static_cast<size_t>(sample_rate_ / 5); // 200ms minimum
    if (pending_speech_buffer_.size() >= min_samples && has_enrolled_voice_ && pending_segment_frames_ > 0) {
        const float segment_similarity = compare_voice_features(pending_segment_features_, enrolled_voice_features_);
        if (segment_similarity < 1.1f) {
            if (callback) {
                std::vector<float> gained = pending_speech_buffer_;
                if (mic_gain_ != 1.0f) {
                    for (float& s : gained) s = std::max(-1.0f, std::min(1.0f, s * mic_gain_));
                }
                callback(gained);
            }
        }
    } else if (pending_speech_buffer_.size() >= min_samples) {
        if (callback) {
            std::vector<float> gained = pending_speech_buffer_;
            if (mic_gain_ != 1.0f) {
                for (float& s : gained) s = std::max(-1.0f, std::min(1.0f, s * mic_gain_));
            }
            callback(gained);
        }
    }

    pending_speech_buffer_.clear();
    pending_segment_features_.clear();
    pending_segment_frames_ = 0;
    speech_segment_active_ = false;
    consecutive_voice_frames_ = 0;
    consecutive_nonvoice_frames_ = 0;
}

void AudioCapture::capture_loop(AudioCallback callback) {
    if (!start_wavein()) {
        std::cerr << "Failed to start wave input" << std::endl;
        capturing_ = false;
        return;
    }

    std::cout << " [EAR] Audio capture started" << std::endl;

    // Pre-allocate float buffer to avoid repeated allocations
    // Increased to hold up to 2 seconds of audio (32000 samples)
    std::vector<float> float_buffer;
    float_buffer.reserve(sample_rate_ * 2);

    static int capture_loop_count = 0;
    ULONGLONG last_packet_tick = GetTickCount64();
    ULONGLONG last_switch_tick = GetTickCount64();
    while (!should_stop_) {
        if (!capture_client_) {
            std::cerr << " [EAR] Capture stream unavailable - stopping audio capture" << std::endl;
            capturing_ = false;
            return;
        }

        DWORD wait_result = WaitForSingleObject(capture_event_, 20);
        if (wait_result != WAIT_OBJECT_0) {
            continue;
        }

        UINT32 packet_count = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&packet_count);
        if (FAILED(hr)) {
            break;
        }

        // if (capture_loop_count < 3 || (capture_loop_count % 100) == 0) {
        //     std::cout << "[EAR] Capture loop: packet_count=" << packet_count << std::endl;
        // }
        capture_loop_count++;

        // Watchdog: a device can be enumerated as Active yet never deliver
        // audio (e.g. a Bluetooth dongle whose headset is unplugged). After
        // 3 s of total silence, transparently try the other active mics.
        if (packet_count > 0) {
            last_packet_tick = GetTickCount64();
        } else if (GetTickCount64() - last_packet_tick > 3000 &&
                   GetTickCount64() - last_switch_tick > 3000) {
            last_switch_tick = GetTickCount64();
            try_switch_capture_device();
            if (capture_client_) {
                last_packet_tick = GetTickCount64();
            }
        }

        while (packet_count > 0 && !should_stop_) {
            BYTE* data = nullptr;
            UINT32 frame_count = 0;
            DWORD flags = 0;
            hr = capture_client_->GetBuffer(&data, &frame_count, &flags, nullptr, nullptr);
            if (FAILED(hr) || !data) {
                break;
            }

            if (muted_) {
                capture_client_->ReleaseBuffer(frame_count);
                capture_client_->GetNextPacketSize(&packet_count);
                continue;
            }

            int channels = 1;
            if (capture_format_) {
                channels = std::max(1, static_cast<int>(capture_format_->nChannels));
            }

            // WASAPI sets AUDCLNT_BUFFERFLAGS_SILENT for a silence packet, but
            // per MSDN the data pointer contents are UNDEFINED in that case.
            // Converting undefined bytes injects garbage into the STT recording
            // and the spectrum canvas, so redirect to a zeroed packet instead.
            std::vector<BYTE> zero_packet;
            const BYTE* packet_data = data;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                zero_packet.assign(static_cast<size_t>(frame_count) *
                    std::max<WORD>(1, capture_format_ ? capture_format_->nBlockAlign : 2), 0);
                packet_data = zero_packet.data();
            }

            float_buffer.resize(frame_count);
            const bool is_float_format = capture_format_ && (
                capture_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (capture_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(capture_format_)->SubFormat,
                     KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            );

            if (capture_format_ && capture_format_->wFormatTag == WAVE_FORMAT_PCM && capture_format_->wBitsPerSample == 16) {
                const auto* pcm_data = reinterpret_cast<const int16_t*>(packet_data);
                for (UINT32 i = 0; i < frame_count; ++i) {
                    float sample_sum = 0.0f;
                    for (int ch = 0; ch < channels; ++ch) {
                        const int index = static_cast<int>(i * channels + ch);
                        sample_sum += static_cast<float>(pcm_data[index]) / 32768.0f;
                    }
                    float_buffer[i] = sample_sum / static_cast<float>(channels);
                }
            } else if (capture_format_ && capture_format_->wFormatTag == WAVE_FORMAT_PCM && capture_format_->wBitsPerSample == 32) {
                const auto* pcm32_data = reinterpret_cast<const int32_t*>(packet_data);
                for (UINT32 i = 0; i < frame_count; ++i) {
                    float sample_sum = 0.0f;
                    for (int ch = 0; ch < channels; ++ch) {
                        const int index = static_cast<int>(i * channels + ch);
                        sample_sum += static_cast<float>(pcm32_data[index]) / 2147483648.0f;
                    }
                    float_buffer[i] = sample_sum / static_cast<float>(channels);
                }
            } else if (is_float_format) {
                const auto* float_data = reinterpret_cast<const float*>(packet_data);
                for (UINT32 i = 0; i < frame_count; ++i) {
                    float sample_sum = 0.0f;
                    for (int ch = 0; ch < channels; ++ch) {
                        const int index = static_cast<int>(i * channels + ch);
                        sample_sum += float_data[index];
                    }
                    float_buffer[i] = sample_sum / static_cast<float>(channels);
                }
            } else {
                for (UINT32 i = 0; i < frame_count; ++i) {
                    float_buffer[i] = 0.0f;
                }
            }

            std::vector<float> audio_to_send = float_buffer;
            if (capture_format_ && capture_format_->nSamplesPerSec != sample_rate_) {
                audio_to_send = resample_audio(float_buffer, static_cast<int>(capture_format_->nSamplesPerSec), sample_rate_);
            }

            // static int audio_data_count = 0;
            // if (audio_data_count < 3 || (audio_data_count % 100) == 0) {
            //     std::cout << "[EAR] Audio data: frames=" << frame_count << " voice_gate=" << voice_gate_enabled_ << " muted=" << muted_ << " callback=" << (callback != nullptr) << " samples=" << audio_to_send.size() << std::endl;
            // }
            // audio_data_count++;

            if (!voice_gate_enabled_) {
                // Removed verbose forwarding logging to reduce console spam
                // static int callback_count = 0;
                if (callback && !muted_ && !audio_to_send.empty()) {
                    // if (callback_count < 3 || (callback_count % 100) == 0) {
                    //     std::cout << "[EAR] Forwarding " << audio_to_send.size() << " samples to STT" << std::endl;
                    // }
                    // ++callback_count;
                    if (mic_gain_ != 1.0f) {
                        for (float& s : audio_to_send) {
                            s = std::max(-1.0f, std::min(1.0f, s * mic_gain_));
                        }
                    }
                    callback(audio_to_send);
                }
            } else if (!muted_) {
                // Adaptive noise floor tracking so the gate can learn the room.
                float rms = 0.0f;
                for (float sample : float_buffer) {
                    rms += sample * sample;
                }
                rms = std::sqrt(rms / static_cast<float>(float_buffer.size()));
                if (noise_floor_rms_ < 0.0001f) {
                    noise_floor_rms_ = rms > 0.0001f ? rms : 0.0001f;
                } else {
                    noise_floor_rms_ = noise_floor_rms_ * 0.95f + rms * 0.05f;
                }

                const auto frame_features = compute_voice_features(float_buffer);
                const bool voice_like = is_voice_like_frame(float_buffer);
                if (voice_like) {
                    if (!has_enrolled_voice_) {
                        update_enrollment(frame_features);
                    }

                    accumulate_segment_features(frame_features);
                    consecutive_voice_frames_++;
                    consecutive_nonvoice_frames_ = 0;
                    if (!speech_segment_active_ && consecutive_voice_frames_ >= 3) {
                        speech_segment_active_ = true;
                    }
                    if (speech_segment_active_) {
                        pending_speech_buffer_.insert(pending_speech_buffer_.end(), float_buffer.begin(), float_buffer.end());
                    }
                } else {
                    consecutive_nonvoice_frames_++;
                    consecutive_voice_frames_ = 0;
                    if (speech_segment_active_ && consecutive_nonvoice_frames_ >= 6) {
                        flush_speech_segment(callback);
                    }
                }
            }

            capture_client_->ReleaseBuffer(frame_count);
            capture_client_->GetNextPacketSize(&packet_count);
        }
    }

    stop_wavein();
}