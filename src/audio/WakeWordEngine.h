#pragma once
// Exact C++ port of the openWakeWord streaming pipeline used by
// E:\code java\scripts\jarvis_ears.py (openwakeword/utils.py AudioFeatures
// streaming chain + model.py Model.predict), running the same ONNX models via
// ONNX Runtime:
//   melspectrogram.onnx : float32 [batch, samples] -> [frames, 1, 1, 32]
//   embedding_model.onnx: float32 [batch, 76, 32, 1] -> [batch, 1, 1, 96]
//   wakeword .onnx      : float32 [1, 16, 96] -> [1, 1]
// Streaming chain (openwakeword _streaming_features):
//   - audio buffered as raw 16 kHz int16 (rolling 10 s window)
//   - melspec computed over the last (n + 480) samples, transform x/10 + 2
//   - one 76-frame window per 1280-sample chunk, stepped 8 rows back per
//     extra chunk, one 96-dim embedding frame each
//   - wake word runs on the last 16 feature frames
//   - predictions zeroed for the first 5 frames after init/reset

#include <string>
#include <vector>
#include <deque>
#include <cstdint>

// Opaque ONNX Runtime objects (defined in the .cpp)
struct _OrtEnv;
struct _OrtSession;
struct _OrtSessionOptions;
struct _OrtMemoryInfo;

class WakeWordEngine {
public:
    WakeWordEngine();
    ~WakeWordEngine();

    // Load the three openWakeWord ONNX models.
    bool initialize(const std::string& wakeword_model_path,
                    const std::string& melspec_model_path,
                    const std::string& embedding_model_path);
    bool is_initialized() const { return initialized_; }

    // Feed 16 kHz int16 PCM (ideally 512/1280-sample chunks; any length is
    // supported, non-even remainders are carried over, identical to
    // openwakeword raw_data_remainder handling). Returns the score [0..1].
    float predict(const std::vector<int16_t>& audio);

    // Re-initialize all buffers (openwakeword Model.reset()).
    void reset();

private:
    void destroy_sessions();

    // openwakeword AudioFeatures chain
    std::vector<float> compute_melspec(const std::vector<int16_t>& audio, size_t& out_rows);
    std::vector<float> run_embedding(const float* window76x32); // -> 96 floats
    float run_wakeword(const float* features16x96);

    // Seed feature_buffer with embeddings of 4 s of random noise
    // (openwakeword __init__/reset: np.random.randint(-1000, 1000, 16000*4)).
    void init_feature_buffer();

    void buffer_raw_data(const std::vector<int16_t>& x);

    // Per-model ONNX handles
    void* melspec_env_ = nullptr;
    void* melspec_session_ = nullptr;
    void* melspec_options_ = nullptr;
    void* melspec_memory_ = nullptr;

    void* embedding_env_ = nullptr;
    void* embedding_session_ = nullptr;
    void* embedding_options_ = nullptr;
    void* embedding_memory_ = nullptr;

    void* wakeword_env_ = nullptr;
    void* wakeword_session_ = nullptr;
    void* wakeword_options_ = nullptr;
    void* wakeword_memory_ = nullptr;

    bool initialized_ = false;

    // Streaming state (openwakeword AudioFeatures fields)
    std::deque<int16_t> raw_data_buffer_;   // maxlen 160000 (10 s)
    size_t accumulated_samples_ = 0;
    std::vector<int16_t> remainder_;        // raw_data_remainder
    std::vector<float> melspec_buffer_;     // rows of 32; starts 76 rows of 1.0
    size_t melspec_rows_ = 76;
    static constexpr size_t kMelspecMaxRows = 970;   // 10*97
    std::vector<float> feature_buffer_;     // rows of 96
    size_t feature_rows_ = 0;
    static constexpr size_t kFeatureMaxRows = 120;   // ~10 s history

    // Model.predict state
    std::deque<float> prediction_buffer_;
    float last_prediction_ = 0.0f;
    int wakeword_input_frames_ = 16;
};
