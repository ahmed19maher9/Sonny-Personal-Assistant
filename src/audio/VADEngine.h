#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

// Forward declarations for ONNX Runtime types (kept opaque to avoid header dependency)
struct _OrtEnv;
struct _OrtSession;
struct _OrtSessionOptions;
struct _OrtMemoryInfo;
struct _OrtValue;

class VADEngine {
public:
    // Callback invoked when a complete voice segment is captured.
    // audio_data: 16kHz mono float PCM of the full recorded utterance.
    // sample_rate: always 16000.
    using VoiceSegmentCallback = std::function<void(const std::vector<float>& audio_data, int sample_rate)>;

    VADEngine();
    ~VADEngine();

    // Initialize the Silero VAD ONNX model. model_path should point to
    // a silero_vad_v5.onnx (or v4) model file.
    bool initialize(const std::string& model_path);

    // Feed 16kHz mono float audio into the VAD engine. This processes
    // the audio in real-time and invokes voice_segment_callback_ when
    // a complete utterance is detected.
    void process_audio(const std::vector<float>& audio_data);

    // Set callback for completed voice segments.
    void set_voice_segment_callback(VoiceSegmentCallback callback) { voice_segment_callback_ = callback; }

    // Reset the VAD state machine (call after processing a segment).
    void reset();

    // Check if initialized.
    bool is_initialized() const { return initialized_; }

private:
    // ONNX Runtime objects (opaque pointers)
    void* env_;
    void* session_;
    void* session_options_;
    void* memory_info_;

    bool initialized_;

    // Silero V5 state
    std::vector<float> state_;
    std::vector<float> context_;
    int context_size_;
    int size_state_;

    // Voice activity segmentation state (ported from RealTimeCutVAD AlgorithmImpl)
    bool recording_;
    std::vector<bool> pre_vad_list_;
    std::vector<bool> start_vad_list_;
    std::vector<float> audio_buffer_;
    std::vector<float> pre_audio_buffer_;
    std::vector<float> chunk_buffer_;

    // Thresholds (Silero V5 defaults from RealTimeCutVAD)
    float vad_start_prob_;
    float vad_end_prob_;
    float voice_start_true_ratio_;
    float voice_end_false_ratio_;
    int voice_start_frame_count_;
    int voice_end_frame_count_;

    // Callback for completed voice segments
    VoiceSegmentCallback voice_segment_callback_;

    // Silero V5 inference: returns speech probability for a 512-sample (32ms) chunk.
    float silero_predict(const std::vector<float>& data);
    void reset_hidden_layer_value();

    // Segmentation algorithm (from RealTimeCutVAD)
    void run_algorithm(float vad_probability, const std::vector<float>& sample);
    bool true_probability(const std::vector<bool>& list, float prob);
    bool false_probability(const std::vector<bool>& list, float prob);
};