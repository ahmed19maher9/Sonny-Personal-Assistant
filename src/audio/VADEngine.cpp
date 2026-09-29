#include "VADEngine.h"
#include "Logger.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <windows.h>
#include <onnxruntime_cxx_api.h>

// Silero V5 model constants
#define SILERO_V5_CONTEXT_SIZE 64
#define SILERO_V5_STATE_SIZE (2 * 1 * 128)
#define SILERO_V5_SAMPLE_RATE 16000
#define SILERO_V5_CHUNK_SAMPLES 512  // 32ms at 16kHz

VADEngine::VADEngine()
    : env_(nullptr)
    , session_(nullptr)
    , session_options_(nullptr)
    , memory_info_(nullptr)
    , initialized_(false)
    , context_size_(SILERO_V5_CONTEXT_SIZE)
    , size_state_(SILERO_V5_STATE_SIZE)
    , state_(SILERO_V5_STATE_SIZE, 0.0f)
    , context_(SILERO_V5_CONTEXT_SIZE, 0.0f)
    , recording_(false)
    // Middle-ground tuning between the original conservative values
    // (0.7/0.7, ratios 0.5/0.95, 10/57 frames) and the ultra-aggressive ones
    // (0.15/0.15, ratios 0.25/0.6, 3/8 frames). The aggressive set fragmented
    // a single utterance into 4 pieces (verified in stt_debug/app_sim): short
    // onset fragments transcribe to junk and mid-speech pauses cut segments.
    // End detection needs 12 frames (~0.38 s) below 0.35 to fire; the STT
    // recognizer additionally pads 0.8 s of silence to every segment.
    , vad_start_prob_(0.35f)  // Speech onset: sensitive but filters noise
    , vad_end_prob_(0.35f)    // Frame counts as silence below this
    , voice_start_true_ratio_(0.4f)
    , voice_end_false_ratio_(0.7f)
    , voice_start_frame_count_(4)   // ~4x32 ms of speech to start recording
    , voice_end_frame_count_(16)    // ~0.51 s of silence to end recording
                                    // (bridges comma pauses; matches the
                                    //  ~600 ms response latency target)
{
}

VADEngine::~VADEngine() {
    reset_hidden_layer_value();
    try {
        if (session_) {
            delete static_cast<Ort::Session*>(session_);
            session_ = nullptr;
        }
        if (session_options_) {
            delete static_cast<Ort::SessionOptions*>(session_options_);
            session_options_ = nullptr;
        }
        if (memory_info_) {
            delete static_cast<Ort::MemoryInfo*>(memory_info_);
            memory_info_ = nullptr;
        }
        if (env_) {
            delete static_cast<Ort::Env*>(env_);
            env_ = nullptr;
        }
    } catch (...) {
        // Prevent exceptions in destructor
        session_ = nullptr;
        session_options_ = nullptr;
        memory_info_ = nullptr;
        env_ = nullptr;
    }
}

void VADEngine::reset_hidden_layer_value() {
    state_.assign(size_state_, 0.0f);
    context_.assign(context_size_, 0.0f);
    recording_ = false;
    pre_vad_list_.clear();
    start_vad_list_.clear();
    audio_buffer_.clear();
    pre_audio_buffer_.clear();
    chunk_buffer_.clear();
    chunk_buffer_.shrink_to_fit();
}

bool VADEngine::initialize(const std::string& model_path) {
    // Clean up any existing resources first
    try {
        if (session_) {
            delete static_cast<Ort::Session*>(session_);
            session_ = nullptr;
        }
        if (session_options_) {
            delete static_cast<Ort::SessionOptions*>(session_options_);
            session_options_ = nullptr;
        }
        if (memory_info_) {
            delete static_cast<Ort::MemoryInfo*>(memory_info_);
            memory_info_ = nullptr;
        }
        if (env_) {
            delete static_cast<Ort::Env*>(env_);
            env_ = nullptr;
        }
    } catch (...) {
        session_ = nullptr;
        session_options_ = nullptr;
        memory_info_ = nullptr;
        env_ = nullptr;
    }

    try {
        // Create ONNX Runtime environment
        env_ = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "SileroV5");
        if (!env_) {
            LOG_DEBUG_COMPONENT("VAD", "Failed to create ONNX environment");
            return false;
        }

        // Create memory info (CPU)
        memory_info_ = new Ort::MemoryInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU));
        if (!memory_info_) {
            LOG_DEBUG_COMPONENT("VAD", "Failed to create memory info");
            return false;
        }

        // Configure session options
        session_options_ = new Ort::SessionOptions();
        auto* opts = static_cast<Ort::SessionOptions*>(session_options_);
        if (!opts) {
            LOG_DEBUG_COMPONENT("VAD", "Failed to create session options");
            return false;
        }
        opts->SetIntraOpNumThreads(1);
        opts->SetInterOpNumThreads(1);
        opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Create session with model
        int wide_length = MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, nullptr, 0);
        if (wide_length <= 0) {
            LOG_DEBUG_COMPONENT("VAD", "Failed to convert model path to wide string");
            return false;
        }
        std::wstring wide_model_path(static_cast<size_t>(wide_length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, wide_model_path.data(), wide_length);
        
        session_ = new Ort::Session(*static_cast<Ort::Env*>(env_), wide_model_path.c_str(), *opts);
        if (!session_) {
            LOG_DEBUG_COMPONENT("VAD", "Failed to create ONNX session");
            return false;
        }

        reset_hidden_layer_value();
        initialized_ = true;
        LOG_DEBUG_COMPONENT("VAD", "Silero VAD model loaded: " + model_path);
        return true;
    } catch (const Ort::Exception& e) {
        LOG_DEBUG_COMPONENT("VAD", "Failed to initialize ONNX session: " + std::string(e.what()));
        // Clean up partial initialization
        try {
            if (session_) {
                delete static_cast<Ort::Session*>(session_);
                session_ = nullptr;
            }
            if (session_options_) {
                delete static_cast<Ort::SessionOptions*>(session_options_);
                session_options_ = nullptr;
            }
            if (memory_info_) {
                delete static_cast<Ort::MemoryInfo*>(memory_info_);
                memory_info_ = nullptr;
            }
            if (env_) {
                delete static_cast<Ort::Env*>(env_);
                env_ = nullptr;
            }
        } catch (...) {
            session_ = nullptr;
            session_options_ = nullptr;
            memory_info_ = nullptr;
            env_ = nullptr;
        }
        return false;
    } catch (const std::exception& e) {
        LOG_DEBUG_COMPONENT("VAD", "Failed to initialize: " + std::string(e.what()));
        // Clean up partial initialization
        try {
            if (session_) {
                delete static_cast<Ort::Session*>(session_);
                session_ = nullptr;
            }
            if (session_options_) {
                delete static_cast<Ort::SessionOptions*>(session_options_);
                session_options_ = nullptr;
            }
            if (memory_info_) {
                delete static_cast<Ort::MemoryInfo*>(memory_info_);
                memory_info_ = nullptr;
            }
            if (env_) {
                delete static_cast<Ort::Env*>(env_);
                env_ = nullptr;
            }
        } catch (...) {
            session_ = nullptr;
            session_options_ = nullptr;
            memory_info_ = nullptr;
            env_ = nullptr;
        }
        return false;
    }
}

float VADEngine::silero_predict(const std::vector<float>& data) {
    if (!initialized_ || !session_ || !memory_info_) {
        return 0.0f;
    }

    try {
        auto* sess = static_cast<Ort::Session*>(session_);
        auto* mem_info = static_cast<Ort::MemoryInfo*>(memory_info_);

        // Concatenate context + input data
        std::vector<float> input_with_context;
        input_with_context.reserve(context_.size() + data.size());
        input_with_context.insert(input_with_context.end(), context_.begin(), context_.end());
        input_with_context.insert(input_with_context.end(), data.begin(), data.end());

        // Input dimensions: [1, context_size + data.size()]
        int64_t input_dims[2] = {1, static_cast<int64_t>(input_with_context.size())};
        int64_t state_dims[3] = {2, 1, 128};
        int64_t sr_dims[1] = {1};
        int64_t sr = SILERO_V5_SAMPLE_RATE;

        // Create input tensors
        Ort::Value input_ort = Ort::Value::CreateTensor<float>(
            *mem_info, input_with_context.data(), input_with_context.size(), input_dims, 2);

        Ort::Value state_ort = Ort::Value::CreateTensor<float>(
            *mem_info, state_.data(), state_.size(), state_dims, 3);

        Ort::Value sr_ort = Ort::Value::CreateTensor<int64_t>(
            *mem_info, &sr, 1, sr_dims, 1);

        // Input names for Silero V5: input, state, sr
        const char* input_names[] = {"input", "state", "sr"};
        const char* output_names[] = {"output", "stateN"};

        std::vector<Ort::Value> ort_inputs;
        ort_inputs.emplace_back(std::move(input_ort));
        ort_inputs.emplace_back(std::move(state_ort));
        ort_inputs.emplace_back(std::move(sr_ort));

        // Run inference
        auto ort_outputs = sess->Run(
            Ort::RunOptions{nullptr},
            input_names,
            ort_inputs.data(),
            ort_inputs.size(),
            output_names,
            2
        );

        // Save new context from end of concatenated input
        context_.assign(input_with_context.end() - context_size_, input_with_context.end());

        // Get speech probability
        float speech_prob = ort_outputs[0].GetTensorMutableData<float>()[0];

        // Update hidden state
        float* stateN = ort_outputs[1].GetTensorMutableData<float>();
        std::memcpy(state_.data(), stateN, size_state_ * sizeof(float));

        return speech_prob;
    } catch (const Ort::Exception& e) {
        LOG_DEBUG_COMPONENT("VAD", "Prediction failed: " + std::string(e.what()));
        return 0.0f;
    }
}

bool VADEngine::true_probability(const std::vector<bool>& list, float prob) {
    int true_count = std::count(list.begin(), list.end(), true);
    return (true_count / static_cast<float>(list.size())) > prob;
}

bool VADEngine::false_probability(const std::vector<bool>& list, float prob) {
    int false_count = std::count(list.begin(), list.end(), false);
    return (false_count / static_cast<float>(list.size())) > prob;
}

void VADEngine::run_algorithm(float vad_probability, const std::vector<float>& sample) {
    if (!recording_) {
        bool is_speech = vad_probability > vad_start_prob_;

        pre_vad_list_.push_back(is_speech);
        pre_audio_buffer_.insert(pre_audio_buffer_.end(), sample.begin(), sample.end());

        // Keep a rolling pre-speech buffer: about 0.32s * 4 - 0.032s = ~1.248s
        size_t desired_size = sample.size() * voice_start_frame_count_ * 4;
        if (pre_audio_buffer_.size() >= desired_size) {
            pre_audio_buffer_.erase(pre_audio_buffer_.begin(), pre_audio_buffer_.begin() + sample.size());
        }

        // Safety check to prevent unbounded buffer growth
        const size_t max_pre_buffer_size = 16000 * 5; // 5 seconds max
        if (pre_audio_buffer_.size() > max_pre_buffer_size) {
            LOG_DEBUG_COMPONENT("VAD", "Pre-speech buffer overflow, truncating");
            pre_audio_buffer_.erase(pre_audio_buffer_.begin(), pre_audio_buffer_.begin() + (pre_audio_buffer_.size() - max_pre_buffer_size));
        }

        // Check if we have enough frames to confirm voice start
        if (pre_vad_list_.size() >= static_cast<size_t>(voice_start_frame_count_)) {
            if (true_probability(pre_vad_list_, voice_start_true_ratio_)) {
                // Voice started - copy pre-speech buffer into the recording buffer
                audio_buffer_.assign(pre_audio_buffer_.begin(), pre_audio_buffer_.end());
                pre_vad_list_.clear();
                recording_ = true;
                // std::cout << "[VAD] Voice recording started!" << std::endl;
            } else {
                pre_vad_list_.erase(pre_vad_list_.begin());
            }
        }
    } else {
        bool is_speech = vad_probability > vad_end_prob_;
        start_vad_list_.push_back(is_speech);
        audio_buffer_.insert(audio_buffer_.end(), sample.begin(), sample.end());

        if (!pre_audio_buffer_.empty()) {
            pre_audio_buffer_.clear();
        }

        // Safety check to prevent unbounded buffer growth (max 30 seconds)
        const size_t max_audio_buffer_size = 16000 * 30; 
        if (audio_buffer_.size() > max_audio_buffer_size) {
            LOG_DEBUG_COMPONENT("VAD", "Audio buffer overflow, forcing segment delivery");
            if (voice_segment_callback_ && !audio_buffer_.empty()) {
                // std::cout << "[VAD] Voice recording ended (overflow)! Delivering " 
                //           << audio_buffer_.size() << " samples ("
                //           << audio_buffer_.size() / 16000.0f << "s)" << std::endl;
                voice_segment_callback_(audio_buffer_, SILERO_V5_SAMPLE_RATE);
            }
            // Reset state
            pre_vad_list_.clear();
            start_vad_list_.clear();
            audio_buffer_.clear();
            pre_audio_buffer_.clear();
            recording_ = false;
            reset_hidden_layer_value();
            return;
        }

        // Check if we have enough frames to confirm voice end
        if (start_vad_list_.size() >= static_cast<size_t>(voice_end_frame_count_)) {
            if (false_probability(start_vad_list_, voice_end_false_ratio_)) {
                // Voice ended - deliver the complete recording if it meets minimum duration (0.3s)
                const size_t min_speech_samples = SILERO_V5_SAMPLE_RATE * 3 / 10;
                if (voice_segment_callback_ && audio_buffer_.size() >= min_speech_samples) {
                    // std::cout << "[VAD] Voice recording ended! Delivering " 
                    //           << audio_buffer_.size() << " samples ("
                    //           << audio_buffer_.size() / 16000.0f << "s)" << std::endl;
                    voice_segment_callback_(audio_buffer_, SILERO_V5_SAMPLE_RATE);
                } else if (audio_buffer_.size() < min_speech_samples && !audio_buffer_.empty()) {
                    // std::cout << "[VAD] Discarded short noise burst (" << audio_buffer_.size() << " samples)" << std::endl;
                }

                // Reset state
                pre_vad_list_.clear();
                start_vad_list_.clear();
                audio_buffer_.clear();
                pre_audio_buffer_.clear();
                recording_ = false;
                reset_hidden_layer_value();
            } else {
                start_vad_list_.erase(start_vad_list_.begin());
            }
        }
    }
}

void VADEngine::process_audio(const std::vector<float>& audio_data) {
    if (!initialized_ || audio_data.empty()) {
        return;
    }

    // Check audio amplitude to detect silent audio
    static int amplitude_check_counter = 0;
    // Removed verbose amplitude logging to reduce console spam
    // if (amplitude_check_counter < 3 || (amplitude_check_counter % 100) == 0) {
    //     float max_amplitude = 0.0f;
    //     float rms = 0.0f;
    //     for (float sample : audio_data) {
    //         max_amplitude = std::max(max_amplitude, std::abs(sample));
    //         rms += sample * sample;
    //     }
    //     rms = std::sqrt(rms / static_cast<float>(audio_data.size()));
    //     std::cout << "[VAD] Audio amplitude check: max=" << max_amplitude << " rms=" << rms << " samples=" << audio_data.size() << std::endl;
    // }
    // amplitude_check_counter++;

    chunk_buffer_.insert(chunk_buffer_.end(), audio_data.begin(), audio_data.end());

    static int debug_counter = 0;
    while (chunk_buffer_.size() >= SILERO_V5_CHUNK_SAMPLES) {
        std::vector<float> chunk;
        chunk.reserve(SILERO_V5_CHUNK_SAMPLES);
        chunk.insert(chunk.end(), chunk_buffer_.begin(), chunk_buffer_.begin() + SILERO_V5_CHUNK_SAMPLES);
        chunk_buffer_.erase(chunk_buffer_.begin(), chunk_buffer_.begin() + SILERO_V5_CHUNK_SAMPLES);

        float prob = silero_predict(chunk);
        // Removed verbose speech probability logging to reduce console spam
        // if (debug_counter < 5 || (debug_counter % 50) == 0) {
        //     std::cout << "[VAD] Speech probability: " << prob << " (threshold: " << vad_start_prob_ << ")" << std::endl;
        // }
        // debug_counter++;
        
        run_algorithm(prob, chunk);
    }
}

void VADEngine::reset() {
    reset_hidden_layer_value();
    // std::cout << "[VAD] VAD state reset" << std::endl;
}