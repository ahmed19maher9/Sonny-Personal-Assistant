#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <deque>
#include <functional>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

#include <ctranslate2/models/whisper.h>

#include "VADEngine.h"

class WakeWordEngine;

class CTranslate2WhisperRecognizer {
public:
    bool initialize(const std::string& model_dir,
                    const std::string& tokenizer_path,
                    const std::string& language = "en");
    std::string transcribe(const std::vector<float>& audio_data, int sample_rate) const;

private:
    std::string model_dir_;
    std::string tokenizer_path_;
    std::string language_ = "en";
    bool initialized_ = false;
    std::unique_ptr<ctranslate2::models::Whisper> whisper_;
    size_t n_mels_ = 80;
};

// C++ port of the jarvis_ears.py STT pipeline:
//   microphone (16 kHz int16 mono) -> openWakeWord audio wake word (ONNX) ->
//   on detection: AEC check (jarvis_ears.py is_mouth_speaking: play the wake
//   sound but skip recording while the assistant is speaking) -> record
//   exactly 4 seconds -> one faster-whisper transcribe call
//   (beam_size=5, language=en) -> text.
// transcribe() reproduces the faster-whisper 1.2.1 defaults (the exact
// version jarvis_ears.py runs from E:\jarvis\Lib\site-packages):
// 30 s zero-padded mel windows, timestamps enabled, suppress_tokens=-1,
// temperature fallback over [0, 0.2, 0.4, 0.6, 0.8, 1.0] (beam search at
// T=0, best-of-5 sampling at T>0), compression_ratio 2.4 / avg_logprob -1.0
// fallback, no_speech 0.6 skip, segment splitting by timestamp tokens, texts
// joined with " ". No VAD segmentation, no silence padding, no junk
// filtering, no dedup, matching jarvis_ears.py listen_and_transcribe().
// When the wake word is disabled via settings, the legacy Silero VAD
// segmentation path is used instead.
class STTEngineWrapper {
public:
    using WakeWordCallback = std::function<void()>;

    STTEngineWrapper();
    ~STTEngineWrapper();

    bool initialize();

    void set_model_paths(const std::string& silero_vad_model,
                         const std::string& ct2_model_dir);

    // openWakeWord ONNX models (wake word model + shared feature models),
    // mirroring jarvis_ears.py Jarvis.onnx / melspectrogram.onnx /
    // embedding_model.onnx.
    void set_wake_word_models(const std::string& wakeword_model,
                              const std::string& melspec_model,
                              const std::string& embedding_model) {
        wakeword_model_path_ = wakeword_model;
        melspec_model_path_ = melspec_model;
        embedding_model_path_ = embedding_model;
    }

    void set_model_type(const std::string& type) { model_type_ = type; }
    std::string get_model_type() const { return model_type_; }

    void set_language(const std::string& lang) { language_ = lang; }
    std::string get_language() const { return language_; }

    void set_wake_word_enabled(bool enabled) { wake_word_enabled_ = enabled; }

    bool start();
    void stop();

    void send_audio(const std::vector<float>& audio_data, int sample_rate = 16000);
    void set_muted(bool muted);
    bool is_muted() const { return muted_; }

    // jarvis_ears.py AEC (is_mouth_speaking): while the assistant is speaking,
    // a detected wake word still plays the confirmation sound and resets the
    // wake model, but no 4 s recording/transcription is started.
    void set_assistant_speaking(bool speaking) { assistant_speaking_.store(speaking); }

    std::string get_transcription();

    void set_wake_word_callback(WakeWordCallback callback) { wake_word_callback_ = callback; }

    bool is_initialized() const { return initialized_; }
    bool is_running() const { return running_; }

private:
    bool initialized_;
    std::string silero_vad_model_;
    std::string ct2_model_dir_;
    std::string wakeword_model_path_;
    std::string melspec_model_path_;
    std::string embedding_model_path_;
    std::string language_ = "en";
    std::string model_type_ = "whisper";
    std::atomic<bool> running_;

    std::unique_ptr<VADEngine> vad_;
    std::unique_ptr<WakeWordEngine> wakeword_;
    std::unique_ptr<CTranslate2WhisperRecognizer> recognizer_;
    int sample_rate_ = 16000;
    int vad_window_size_ = 512;
    int mic_sample_rate_ = 16000;
    int mic_device_index_ = -1;

    std::thread processing_thread_;
    std::atomic<bool> processing_thread_running_;

    std::queue<std::string> transcription_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Completed utterances (4 s jarvis recording or VAD segment) awaiting
    // transcription on the dedicated thread.
    std::queue<std::vector<float>> segment_queue_;
    std::mutex segment_queue_mutex_;
    std::condition_variable segment_queue_cv_;
    std::thread transcription_thread_;
    std::atomic<bool> transcription_thread_running_{false};

    // Capture queue fed by send_audio (WASAPI capture callback).
    static constexpr size_t kMaxAudioQueueChunks = 1000;
    std::queue<std::vector<float>> samples_queue_;
    std::mutex samples_queue_mutex_;
    std::condition_variable samples_queue_cv_;
    std::atomic<bool> stop_capture_{false};

    // Jarvis ears streaming state (see jarvis_ears.py)
    std::vector<int16_t> chunk_accumulator_;   // builds 512-sample int16 frames
    bool recording_ = false;                   // 4 s command recording in progress
    std::vector<float> recording_buffer_;

    WakeWordCallback wake_word_callback_ = nullptr;
    bool wake_word_enabled_ = true;
    bool muted_ = false;
    std::atomic<bool> assistant_speaking_{false};  // jarvis_ears.py is_mouth_speaking()

    // One-time slow-transcription diagnostic flag (mirrors LlamaWrapper::warned_slow_prefill_)
    bool warned_slow_transcription_ = false;

    void on_vad_segment(const std::vector<float>& segment, int sample_rate);

    void processing_loop();
    void transcription_loop();
    void enqueue_segment(std::vector<float> segment);

    static constexpr int kProcessingIntervalMs = 5;
    // jarvis_ears.py: CHUNK = 512, record int(RATE / CHUNK * 4) chunks.
    static constexpr size_t kEarsChunkSamples = 512;
    static constexpr size_t kRecordSamples = 16000 * 4;  // exactly 4 seconds
};