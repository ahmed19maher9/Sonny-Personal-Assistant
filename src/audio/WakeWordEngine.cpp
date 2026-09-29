#include "WakeWordEngine.h"
#include "Logger.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <windows.h>
#include <onnxruntime_cxx_api.h>

namespace {
constexpr int kSampleRate = 16000;
constexpr size_t kChunkSize = 1280;          // 80 ms at 16 kHz
constexpr size_t kMelspecTailExtra = 480;    // 160*3 context samples
constexpr size_t kEmbeddingWindow = 76;      // melspec frames per embedding
constexpr size_t kMelspecBins = 32;
constexpr size_t kEmbeddingDim = 96;
constexpr size_t kRawDataMax = 160000;       // 10 s rolling window
constexpr int kWarmupZeroFrames = 5;         // predictions zeroed at start

std::wstring to_wide(const std::string& utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    if (!wide.empty()) wide.resize(static_cast<size_t>(len - 1));
    return wide;
}

// Create one ONNX session (CPU, single thread like openwakeword onnx usage)
bool create_session(const std::string& model_path,
                    void** env, void** options, void** memory, void** session) {
    try {
        *env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "OpenWakeWord");
        *memory = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        auto* opts = new Ort::SessionOptions();
        opts->SetIntraOpNumThreads(1);
        opts->SetInterOpNumThreads(1);
        opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        *options = opts;
        *session = new Ort::Session(*static_cast<Ort::Env*>(*env),
                                    to_wide(model_path).c_str(), *opts);
        return true;
    } catch (const Ort::Exception& e) {
        std::ostringstream ss;
        ss << "Failed to load model " << model_path << ": " << e.what();
        LOG_WAKE(ss.str());
        return false;
    }
}
}  // namespace

WakeWordEngine::WakeWordEngine() = default;

WakeWordEngine::~WakeWordEngine() {
    destroy_sessions();
}

void WakeWordEngine::destroy_sessions() {
    auto destroy_one = [](void** env, void** options, void** memory, void** session) {
        try {
            if (session && *session) { delete static_cast<Ort::Session*>(*session); *session = nullptr; }
            if (options && *options) { delete static_cast<Ort::SessionOptions*>(*options); *options = nullptr; }
            if (memory && *memory) { delete static_cast<Ort::MemoryInfo*>(*memory); *memory = nullptr; }
            if (env && *env) { delete static_cast<Ort::Env*>(*env); *env = nullptr; }
        } catch (...) {}
    };
    destroy_one(&melspec_env_, &melspec_options_, &melspec_memory_, &melspec_session_);
    destroy_one(&embedding_env_, &embedding_options_, &embedding_memory_, &embedding_session_);
    destroy_one(&wakeword_env_, &wakeword_options_, &wakeword_memory_, &wakeword_session_);
}

bool WakeWordEngine::initialize(const std::string& wakeword_model_path,
                                const std::string& melspec_model_path,
                                const std::string& embedding_model_path) {
    destroy_sessions();
    initialized_ = false;

    if (!create_session(melspec_model_path, &melspec_env_, &melspec_options_,
                        &melspec_memory_, &melspec_session_)) {
        return false;
    }
    if (!create_session(embedding_model_path, &embedding_env_, &embedding_options_,
                        &embedding_memory_, &embedding_session_)) {
        destroy_sessions();
        return false;
    }
    if (!create_session(wakeword_model_path, &wakeword_env_, &wakeword_options_,
                        &wakeword_memory_, &wakeword_session_)) {
        destroy_sessions();
        return false;
    }

    // Initialize streaming buffers (openwakeword AudioFeatures.__init__)
    raw_data_buffer_.clear();
    accumulated_samples_ = 0;
    remainder_.clear();
    melspec_buffer_.assign(kMelspecBins * 76, 1.0f);  // np.ones((76, 32))
    melspec_rows_ = 76;
    prediction_buffer_.clear();
    last_prediction_ = 0.0f;
    init_feature_buffer();

    initialized_ = true;
    LOG_WAKE("openWakeWord models loaded (jarvis_ears pipeline)");
    return true;
}

void WakeWordEngine::buffer_raw_data(const std::vector<int16_t>& x) {
    for (int16_t s : x) {
        raw_data_buffer_.push_back(s);
    }
    while (raw_data_buffer_.size() > kRawDataMax) {
        raw_data_buffer_.pop_front();
    }
}

// openwakeword _get_melspectrogram: int16 -> float32 [1, N]; output squeezed
// [T, 32]; transform x/10 + 2.
std::vector<float> WakeWordEngine::compute_melspec(const std::vector<int16_t>& audio,
                                                   size_t& out_rows) {
    out_rows = 0;
    if (!initialized_ || audio.empty()) return {};
    try {
        std::vector<float> input(audio.begin(), audio.end());
        int64_t dims[2] = {1, static_cast<int64_t>(input.size())};
        auto tensor = Ort::Value::CreateTensor<float>(
            *static_cast<Ort::MemoryInfo*>(melspec_memory_),
            input.data(), input.size(), dims, 2);
        const char* input_names[] = {"input"};
        const char* output_names[] = {"output"};
        auto outputs = static_cast<Ort::Session*>(melspec_session_)->Run(
            Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
        float* data = outputs[0].GetTensorMutableData<float>();
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        // Output: [T, 1, 1, 32] (squeezed to [T, 32]); last dim always 32 bins.
        const int64_t bins = shape[shape.size() - 1];
        const int64_t frames = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount() / bins;
        out_rows = static_cast<size_t>(frames);
        std::vector<float> spec(data, data + frames * bins);
        // melspec_transform: lambda x: x/10 + 2
        for (float& v : spec) {
            v = v / 10.0f + 2.0f;
        }
        return spec;
    } catch (const Ort::Exception& e) {
        std::ostringstream ss;
        ss << "melspec failed: " << e.what();
        LOG_WAKE(ss.str());
        return {};
    }
}

// openwakeword embedding model: [batch, 76, 32, 1] -> squeeze -> 96 floats
std::vector<float> WakeWordEngine::run_embedding(const float* window76x32) {
    try {
        int64_t dims[4] = {1, static_cast<int64_t>(kEmbeddingWindow),
                           static_cast<int64_t>(kMelspecBins), 1};
        auto tensor = Ort::Value::CreateTensor<float>(
            *static_cast<Ort::MemoryInfo*>(embedding_memory_),
            const_cast<float*>(window76x32), kEmbeddingWindow * kMelspecBins,
            dims, 4);
        const char* input_names[] = {"input_1"};
        const char* output_names[] = {"conv2d_19"};
        auto outputs = static_cast<Ort::Session*>(embedding_session_)->Run(
            Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
        float* data = outputs[0].GetTensorMutableData<float>();
        return std::vector<float>(data, data + kEmbeddingDim);
    } catch (const Ort::Exception& e) {
        std::ostringstream ss;
        ss << "embedding failed: " << e.what();
        LOG_WAKE(ss.str());
        return {};
    }
}

// openwakeword wakeword model: [1, 16, 96] -> [1, 1]
float WakeWordEngine::run_wakeword(const float* features16x96) {
    try {
        int64_t dims[3] = {1, static_cast<int64_t>(wakeword_input_frames_),
                           static_cast<int64_t>(kEmbeddingDim)};
        auto tensor = Ort::Value::CreateTensor<float>(
            *static_cast<Ort::MemoryInfo*>(wakeword_memory_),
            const_cast<float*>(features16x96),
            wakeword_input_frames_ * kEmbeddingDim, dims, 3);
        const char* input_names[] = {"onnx::Flatten_0"};
        const char* output_names[] = {"39"};
        auto outputs = static_cast<Ort::Session*>(wakeword_session_)->Run(
            Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
        return outputs[0].GetTensorMutableData<float>()[0];
    } catch (const Ort::Exception& e) {
        std::ostringstream ss;
        ss << "wakeword inference failed: " << e.what();
        LOG_WAKE(ss.str());
        return 0.0f;
    }
}

// openwakeword __init__/reset: feature_buffer = embeddings of 4 s of random
// int16 noise; _get_embeddings: full-clip melspec, 76-frame windows stepped
// by 8, single batched embedding run.
void WakeWordEngine::init_feature_buffer() {
    std::vector<int16_t> noise(16000 * 4);
    for (int16_t& v : noise) {
        v = static_cast<int16_t>((std::rand() % 2000) - 1000);
    }
    feature_buffer_.clear();
    feature_rows_ = 0;
    size_t rows = 0;
    const std::vector<float> spec = compute_melspec(noise, rows);
    if (rows < kEmbeddingWindow) {
        // Fallback: 76 rows of ones so get_features(16) always has data.
        feature_buffer_.assign(kEmbeddingDim * 16, 0.0f);
        feature_rows_ = 16;
        return;
    }
    std::vector<size_t> window_starts;
    for (size_t i = 0; i + kEmbeddingWindow <= rows; i += 8) {
        window_starts.push_back(i);
    }
    if (window_starts.empty()) {
        feature_buffer_.assign(kEmbeddingDim * 16, 0.0f);
        feature_rows_ = 16;
        return;
    }
    try {
        std::vector<float> batch(window_starts.size() * kEmbeddingWindow * kMelspecBins);
        for (size_t w = 0; w < window_starts.size(); ++w) {
            std::memcpy(batch.data() + w * kEmbeddingWindow * kMelspecBins,
                        spec.data() + window_starts[w] * kMelspecBins,
                        kEmbeddingWindow * kMelspecBins * sizeof(float));
        }
        int64_t dims[4] = {static_cast<int64_t>(window_starts.size()),
                           static_cast<int64_t>(kEmbeddingWindow),
                           static_cast<int64_t>(kMelspecBins), 1};
        auto tensor = Ort::Value::CreateTensor<float>(
            *static_cast<Ort::MemoryInfo*>(embedding_memory_), batch.data(),
            batch.size(), dims, 4);
        const char* input_names[] = {"input_1"};
        const char* output_names[] = {"conv2d_19"};
        auto outputs = static_cast<Ort::Session*>(embedding_session_)->Run(
            Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
        float* data = outputs[0].GetTensorMutableData<float>();
        feature_buffer_.assign(data, data + window_starts.size() * kEmbeddingDim);
        feature_rows_ = window_starts.size();
    } catch (const Ort::Exception& e) {
        std::ostringstream ss;
        ss << "feature buffer init failed: " << e.what();
        LOG_WAKE(ss.str());
        feature_buffer_.assign(kEmbeddingDim * 16, 0.0f);
        feature_rows_ = 16;
    }
}

void WakeWordEngine::reset() {
    if (!initialized_) return;
    raw_data_buffer_.clear();
    accumulated_samples_ = 0;
    remainder_.clear();
    melspec_buffer_.assign(kMelspecBins * 76, 1.0f);
    melspec_rows_ = 76;
    prediction_buffer_.clear();
    last_prediction_ = 0.0f;
    init_feature_buffer();
    LOG_WAKE("Wake word state reset");
}

// Exact port of openwakeword AudioFeatures._streaming_features +
// Model.predict (as called by jarvis_ears.py with 512-sample chunks).
float WakeWordEngine::predict(const std::vector<int16_t>& audio) {
    if (!initialized_ || audio.empty()) return last_prediction_;

    int n_prepared = 0;

    // _streaming_features: prepend remainder, split into even 80 ms chunks
    std::vector<int16_t> xx;
    if (!remainder_.empty()) {
        xx = remainder_;
        remainder_.clear();
    }
    xx.insert(xx.end(), audio.begin(), audio.end());

    if (accumulated_samples_ + xx.size() >= kChunkSize) {
        const size_t r = (accumulated_samples_ + xx.size()) % kChunkSize;
        const size_t even_len = xx.size() - r;
        if (even_len > 0) {
            buffer_raw_data(std::vector<int16_t>(xx.begin(), xx.begin() + even_len));
            accumulated_samples_ += even_len;
        }
        if (r != 0) {
            remainder_.assign(xx.end() - static_cast<long long>(r), xx.end());
        }
    } else {
        buffer_raw_data(xx);
        accumulated_samples_ += xx.size();
    }

    // Only run the feature chain once even chunks are accumulated
    if (accumulated_samples_ >= kChunkSize && accumulated_samples_ % kChunkSize == 0) {
        // _streaming_melspectrogram(accumulated_samples_): melspec of the last
        // (accumulated + 480) raw samples
        const size_t tail_n = accumulated_samples_ + kMelspecTailExtra;
        if (raw_data_buffer_.size() >= 400 && tail_n <= raw_data_buffer_.size()) {
            std::vector<int16_t> tail(raw_data_buffer_.end() - static_cast<long long>(tail_n),
                                      raw_data_buffer_.end());
            size_t rows = 0;
            const std::vector<float> spec = compute_melspec(tail, rows);
            if (!spec.empty() && rows > 0) {
                melspec_buffer_.insert(melspec_buffer_.end(), spec.begin(), spec.end());
                melspec_rows_ += rows;
                if (melspec_rows_ > kMelspecMaxRows) {
                    const size_t drop = melspec_rows_ - kMelspecMaxRows;
                    melspec_buffer_.erase(melspec_buffer_.begin(),
                                          melspec_buffer_.begin() + static_cast<long long>(drop * kMelspecBins));
                    melspec_rows_ = kMelspecMaxRows;
                }
            }
        }

        // Per 1280-sample chunk, one embedding from a 76-row window:
        // for i in range(n//1280 - 1, -1, -1): ndx = -8*i (len(buffer) if 0)
        const int n_chunks = static_cast<int>(accumulated_samples_ / kChunkSize);
        for (int i = n_chunks - 1; i >= 0; --i) {
            const long long end64 = (i == 0)
                ? static_cast<long long>(melspec_rows_)
                : -8LL * i;
            const long long start64 = end64 - static_cast<long long>(kEmbeddingWindow);
            if (start64 >= 0 && end64 <= static_cast<long long>(melspec_rows_)) {
                std::vector<float> window(kEmbeddingWindow * kMelspecBins);
                std::memcpy(window.data(),
                            melspec_buffer_.data() + start64 * kMelspecBins,
                            kEmbeddingWindow * kMelspecBins * sizeof(float));
                const std::vector<float> embedding = run_embedding(window.data());
                if (embedding.size() == kEmbeddingDim) {
                    feature_buffer_.insert(feature_buffer_.end(), embedding.begin(), embedding.end());
                    feature_rows_ += 1;
                }
            }
        }
        if (feature_rows_ > kFeatureMaxRows) {
            const size_t drop = feature_rows_ - kFeatureMaxRows;
            feature_buffer_.erase(feature_buffer_.begin(),
                                  feature_buffer_.begin() + static_cast<long long>(drop * kEmbeddingDim));
            feature_rows_ = kFeatureMaxRows;
        }

        n_prepared = static_cast<int>(accumulated_samples_);
        accumulated_samples_ = 0;
    } else {
        // openwakeword returns accumulated_samples when nothing was processed
        n_prepared = static_cast<int>(accumulated_samples_);
    }

    // Model.predict: wake word inference on the feature window
    if (n_prepared < static_cast<int>(kChunkSize)) {
        // get previous prediction when there are not enough samples
        return last_prediction_;
    }

    if (n_prepared > static_cast<int>(kChunkSize)) {
        // multiple windows, take the max (jarvis never hits this with 512-sample
        // chunks, but kept for openwakeword parity)
        float prediction = 0.0f;
        for (int i = n_prepared / static_cast<int>(kChunkSize) - 1; i >= 0; --i) {
            const long long start64 = -static_cast<long long>(wakeword_input_frames_) - i;
            if (feature_rows_ < static_cast<size_t>(wakeword_input_frames_)) break;
            const long long abs_start = static_cast<long long>(feature_rows_) + start64;
            if (abs_start < 0) continue;
            const float pred = run_wakeword(feature_buffer_.data() + abs_start * kEmbeddingDim);
            prediction = std::max(prediction, pred);
        }
        last_prediction_ = prediction;
    } else {
        // n_prepared == 1280: single window, the last 16 feature frames
        if (feature_rows_ >= static_cast<size_t>(wakeword_input_frames_)) {
            last_prediction_ = run_wakeword(
                feature_buffer_.data() + (feature_rows_ - wakeword_input_frames_) * kEmbeddingDim);
        } else {
            last_prediction_ = 0.0f;
        }
    }

    // Zero predictions for the first frames during model initialization
    prediction_buffer_.push_back(last_prediction_);
    if (prediction_buffer_.size() < kWarmupZeroFrames) {
        last_prediction_ = 0.0f;
    }
    return last_prediction_;
}
