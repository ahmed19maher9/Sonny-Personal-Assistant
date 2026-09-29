#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kokoro.hpp"

class KokoroWrapper {
public:
    KokoroWrapper();
    ~KokoroWrapper();

    // Initialize the in-process Kokoro TTS engine. model_dir is the kokoro
    // model folder containing onnx/ (encoder/har/decoder), Kokoro-82M/
    // (config.json), voices_npy/, espeak-ng-data/ and misaki-data/.
    // accelerator: "cuda" or "tensorrt" (only honored when the kokoro static
    // library was built with USE_ORT_CUDA) or "" for CPU.
    bool initialize(const std::string& model_dir, const std::string& accelerator = "");

    // Set language code for multilingual TTS (e.g., "en", "de", "fr").
    // Note: the C++ pipeline phonemizes English via misaki with an espeak
    // fallback; only the en-US / en-GB distinction currently affects output
    // (parity with the previous kokoro-server HTTP API).
    void set_language(const std::string& lang) { current_language_ = lang; }
    std::string get_language() const { return current_language_; }

    // Synthesize text to audio (returns float PCM samples, no disk writes)
    std::vector<float> synthesize(const std::string& text, const std::string& voice_name = "af_heart", float speed = 1.0f);

    // Get sample rate
    int get_sample_rate() const { return sample_rate_; }

    // Get available voices (from voices_npy/, extension stripped)
    std::vector<std::string> get_available_voices() const;

    // Check if initialized
    bool is_initialized() const { return initialized_; }

private:
    bool initialized_;
    std::mutex tts_mutex_;
    int sample_rate_ = 24000;
    std::string current_language_ = "en";
    std::vector<std::string> available_voices_;
    std::unique_ptr<kokoro::Engine> engine_;
};
