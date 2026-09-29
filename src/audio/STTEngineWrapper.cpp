#include "STTEngineWrapper.h"
#include "Logger.h"

#include "VADEngine.h"
#include "WakeWordEngine.h"
#include "WhisperFeatures.h"

#include <iostream>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cmath>
#include <iomanip>

#include "ZlibCompress.h"

// ---------------------------------------------------------------------------
// faster-whisper 1.2.1 transcribe() parity helpers (the exact version
// jarvis_ears.py runs from E:\jarvis\Lib\site-packages\faster_whisper).
// ---------------------------------------------------------------------------
namespace {

constexpr float kTimePrecision = 0.02f;    // hop 160 / 16 kHz
constexpr int kInputStride = 2;            // 160-sample hop = 2 x conv stride 80
constexpr int kTokensPerSecond = 50;       // 1 / time_precision
constexpr size_t kMaxWindowFrames = 3000;  // 30 s mel windows (pad_or_trim)

// jarvis_ears.py: stt_model.transcribe(audio_data, beam_size=5, language="en")
// - every other decoding parameter is a faster-whisper 1.2.1 default.
constexpr int kBeamSize = 5;
constexpr int kBestOf = 5;
constexpr float kPatience = 1.0f;
constexpr float kLengthPenalty = 1.0f;
constexpr float kRepetitionPenalty = 1.0f;
constexpr size_t kNoRepeatNgramSize = 0;
constexpr size_t kMaxLength = 448;
constexpr float kTemperatureSchedule[] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
constexpr float kCompressionRatioThreshold = 2.4f;
constexpr float kLogProbThreshold = -1.0f;
constexpr float kNoSpeechThreshold = 0.6f;
constexpr float kPromptResetOnTemperature = 0.5f;
constexpr float kMaxInitialTimestamp = 1.0f;

// Timestamp tokens look like "<|12.34|>" and sit at the end of the whisper
// vocabulary (id >= <|notimestamps|> + 1, one unit per 20 ms). Token STRINGS
// are used because the CTranslate2 pool API returns text tokens.
bool timestamp_position(const std::string& tok, int& position) {
    if (tok.size() < 6 || tok.compare(0, 2, "<|") != 0 ||
        tok.compare(tok.size() - 2, 2, "|>") != 0) {
        return false;
    }
    const std::string inner = tok.substr(2, tok.size() - 4);
    if (inner.empty()) return false;
    for (const char c : inner) {
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) return false;
    }
    try {
        position = static_cast<int>(std::lround(std::stod(inner) * kTokensPerSecond));
        return true;
    } catch (...) {
        return false;
    }
}

// faster-whisper Tokenizer.decode(): drops every token at or above <|eot|>
// (special/task/timestamp tokens) before decoding. EOT is spelled "</s>" in
// the CTranslate2 vocabulary; every other special token starts with "<|".
bool is_special_token(const std::string& tok) {
    return tok == "</s>" ||
           (tok.size() >= 4 && tok.compare(0, 2, "<|") == 0 &&
            tok.compare(tok.size() - 2, 2, "|>") == 0);
}

// Byte-level BPE detokenization: concatenate the text tokens and map the
// Whisper BPE space marker (UTF-8 0xC4 0xA0) to a space.
std::string decode_text_tokens(const std::vector<std::string>& tokens) {
    std::string text;
    for (const auto& tok : tokens) {
        if (is_special_token(tok)) continue;
        text += tok;
    }
    static const std::string g_token = "\xC4\xA0";
    size_t pos = 0;
    while ((pos = text.find(g_token, pos)) != std::string::npos) {
        text.replace(pos, g_token.length(), " ");
        pos += 1;
    }
    return text;
}

std::string trim_copy(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

}  // namespace

bool CTranslate2WhisperRecognizer::initialize(const std::string& model_dir,
                                             const std::string& tokenizer_path,
                                             const std::string& language) {
    model_dir_ = model_dir;
    tokenizer_path_ = tokenizer_path;
    language_ = language;
    initialized_ = false;
    try {
        // AUTO compute type: lets CTranslate2 select the fastest safe quantization
        // on this machine. Do NOT force ComputeType::INT8 here: verified in
        // stt_debug/repro_out.txt that INT8 hangs the model load indefinitely
        // (25+ min), stalling the whole STT pipeline at startup.
        whisper_ = std::make_unique<ctranslate2::models::Whisper>(
            model_dir_, ctranslate2::Device::CPU, ctranslate2::ComputeType::AUTO);
        n_mels_ = whisper_ ? whisper_->n_mels() : 0;
        if (n_mels_ == 0) n_mels_ = 80;
        initialized_ = true;
    } catch (const std::exception& e) {
        std::cerr << "[CT2] Failed to load Whisper model from " << model_dir_
                  << ": " << e.what() << std::endl;
        whisper_.reset();
    }
    return initialized_;
}

// jarvis_ears.py listen_and_transcribe():
//   segments, _ = stt_model.transcribe(audio_data, beam_size=5, language="en")
//   text = " ".join([seg.text for seg in segments]).strip()
// Reimplemented below with the full faster-whisper 1.2.1 decoding pipeline
// (faster-whisper default parameters for everything else).
std::string CTranslate2WhisperRecognizer::transcribe(const std::vector<float>& audio_data,
                                                     int sample_rate) const {
    if (!initialized_ || audio_data.empty()) {
        return "";
    }

    try {
        std::vector<float> audio = whisper_feat::resample_to_16k(audio_data, sample_rate);
        if (audio.empty()) {
            return "";
        }

        // faster-whisper: features = self.feature_extractor(audio) over the
        // whole recording; each window is then sliced and zero-padded to
        // 3000 frames (pad_or_trim).
        std::vector<float> features;
        size_t feature_frames = 0;
        if (!whisper_feat::compute_log_mel(audio, n_mels_, features, feature_frames) ||
            feature_frames == 0 || feature_frames > whisper_feat::MaxFrames) {
            return "";
        }

        // faster-whisper generate_segments(): content_frames = shape[-1] - 1.
        const size_t content_frames = feature_frames - 1;
        if (content_frames == 0) {
            return "";
        }

        const std::string lang_token = "<|" + (language_.empty() ? "en" : language_) + "|>";

        std::vector<std::string> segment_texts;   // sub-segment texts (unstripped)
        std::vector<std::string> all_tokens;      // previous tokens for conditioning
        size_t prompt_reset_since = 0;
        size_t seek = 0;
        float temperature = 0.0f;

        while (seek < content_frames) {
            const size_t segment_size = std::min(kMaxWindowFrames, content_frames - seek);
            const double time_offset = static_cast<double>(seek) * kTimePrecision;

            // Window features: features[:, seek : seek + segment_size],
            // zero-padded to 3000 frames (faster-whisper pad_or_trim).
            std::vector<float> window(n_mels_ * kMaxWindowFrames, 0.0f);
            for (size_t m = 0; m < n_mels_; ++m) {
                std::memcpy(window.data() + m * kMaxWindowFrames,
                            features.data() + m * feature_frames + seek,
                            segment_size * sizeof(float));
            }
            ctranslate2::Shape shape{1, static_cast<ctranslate2::dim_t>(n_mels_),
                                     static_cast<ctranslate2::dim_t>(kMaxWindowFrames)};
            ctranslate2::StorageView encoder_input(shape, 0.0f, ctranslate2::Device::CPU);
            std::memcpy(encoder_input.data<float>(), window.data(),
                        window.size() * sizeof(float));

            // Prompt (faster-whisper get_prompt(), default without_timestamps
            // = False, no prefix/hotwords/initial_prompt):
            //   [<|startofprev|> previous_tokens[-223:]] + sot sequence.
            std::vector<std::vector<std::string>> prompts(1);
            const std::vector<std::string> previous_tokens(
                all_tokens.begin() + static_cast<std::ptrdiff_t>(prompt_reset_since),
                all_tokens.end());
            if (!previous_tokens.empty()) {
                prompts[0].emplace_back("<|startofprev|>");
                const size_t max_prev = kMaxLength / 2 - 1;
                const size_t start = previous_tokens.size() > max_prev
                                         ? previous_tokens.size() - max_prev
                                         : 0;
                prompts[0].insert(prompts[0].end(),
                                  previous_tokens.begin() + static_cast<std::ptrdiff_t>(start),
                                  previous_tokens.end());
            }
            prompts[0].emplace_back("<|startoftranscript|>");
            prompts[0].push_back(lang_token);
            prompts[0].emplace_back("<|transcribe|>");
            // Nothing downstream wants segment timestamps: the text is handed
            // straight to the LLM. Requesting them explicitly matters because
            // the Systran CTranslate2 exports (whisper-small.en and up) return
            // an EMPTY transcript without this token, while the older
            // whisper-base export tolerated its absence. Verified on both.
            prompts[0].emplace_back("<|notimestamps|>");

            // ---- faster-whisper generate_with_fallback() ----
            struct Attempt {
                std::vector<std::string> tokens;
                float no_speech_prob = 0.0f;
                float avg_logprob = 0.0f;
                float compression_ratio = 0.0f;
            };
            Attempt decode;
            std::vector<Attempt> all_results;
            std::vector<Attempt> below_cr_results;

            const size_t max_initial_timestamp_index = static_cast<size_t>(
                std::lround(kMaxInitialTimestamp / kTimePrecision));

            bool broke_early = false;
            for (const float t : kTemperatureSchedule) {
                temperature = t;
                ctranslate2::models::WhisperOptions opts;
                if (t > 0.0f) {
                    // faster-whisper: best_of sampling at non-zero temperature.
                    opts.beam_size = 1;
                    opts.num_hypotheses = kBestOf;
                    opts.sampling_topk = 0;  // sample from the full distribution
                    opts.sampling_temperature = t;
                } else {
                    // faster-whisper: beam search at T = 0.
                    opts.beam_size = kBeamSize;
                    opts.patience = kPatience;
                    opts.sampling_topk = 1;
                    opts.sampling_temperature = 0.0f;
                }
                opts.length_penalty = kLengthPenalty;
                opts.repetition_penalty = kRepetitionPenalty;
                opts.no_repeat_ngram_size = kNoRepeatNgramSize;
                opts.max_length = kMaxLength;
                opts.suppress_blank = true;
                opts.suppress_tokens = {-1};
                opts.return_scores = true;
                opts.return_no_speech_prob = true;
                opts.max_initial_timestamp_index = max_initial_timestamp_index;

                auto futures = whisper_->generate(encoder_input, prompts, opts);
                if (futures.empty()) return "";
                const auto result = futures[0].get();

                Attempt attempt;
                if (!result.sequences.empty()) attempt.tokens = result.sequences[0];
                attempt.no_speech_prob = result.no_speech_prob;
                const size_t seq_len = attempt.tokens.size();
                if (!result.scores.empty() && seq_len > 0) {
                    // faster-whisper: cum_logprob = score * seq_len ** length_penalty;
                    // avg_logprob = cum_logprob / (seq_len + 1).
                    attempt.avg_logprob =
                        result.scores[0] *
                        std::pow(static_cast<float>(seq_len), kLengthPenalty) /
                        static_cast<float>(seq_len + 1);
                }
                const std::string text = trim_copy(decode_text_tokens(attempt.tokens));
                attempt.compression_ratio =
                    static_cast<float>(zlib_lite::compression_ratio(text));
                all_results.push_back(attempt);

                bool needs_fallback = false;
                if (attempt.compression_ratio > kCompressionRatioThreshold) {
                    needs_fallback = true;  // too repetitive
                } else {
                    below_cr_results.push_back(attempt);
                }
                if (attempt.avg_logprob < kLogProbThreshold) {
                    needs_fallback = true;  // average log probability too low
                }
                if (attempt.no_speech_prob > kNoSpeechThreshold &&
                    attempt.avg_logprob < kLogProbThreshold) {
                    needs_fallback = false;  // silence
                }
                if (!needs_fallback) {
                    decode = attempt;
                    broke_early = true;
                    break;
                }
            }
            if (!broke_early) {
                // All temperatures failed: keep the attempt with the highest
                // average log probability (compression-ratio-passing first).
                const std::vector<Attempt>& pool =
                    !below_cr_results.empty() ? below_cr_results : all_results;
                decode = *std::max_element(pool.begin(), pool.end(),
                    [](const Attempt& a, const Attempt& b) {
                        return a.avg_logprob < b.avg_logprob;
                    });
            }

            // No-voice-activity check (jarvis_ears.py then receives no
            // segments -> empty text -> nothing is sent to the brain).
            if (decode.no_speech_prob > kNoSpeechThreshold &&
                !(decode.avg_logprob > kLogProbThreshold)) {
                return "";
            }

            const std::vector<std::string>& tokens = decode.tokens;

            // faster-whisper _split_segments_by_timestamps() (text only).
            std::vector<bool> is_ts(tokens.size(), false);
            std::vector<int> ts_pos(tokens.size(), 0);
            for (size_t i = 0; i < tokens.size(); ++i) {
                is_ts[i] = timestamp_position(tokens[i], ts_pos[i]);
            }
            const bool single_timestamp_ending =
                tokens.size() >= 2 && !is_ts[tokens.size() - 2] && is_ts[tokens.size() - 1];
            std::vector<size_t> consecutive;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (is_ts[i] && is_ts[i - 1]) consecutive.push_back(i);
            }

            if (!consecutive.empty()) {
                std::vector<size_t> slices = consecutive;
                if (single_timestamp_ending) slices.push_back(tokens.size());
                size_t last_slice = 0;
                for (const size_t current_slice : slices) {
                    const std::vector<std::string> sliced(
                        tokens.begin() + static_cast<std::ptrdiff_t>(last_slice),
                        tokens.begin() + static_cast<std::ptrdiff_t>(current_slice));
                    const double seg_start = time_offset + ts_pos[last_slice] * kTimePrecision;
                    const double seg_end = time_offset + ts_pos[current_slice - 1] * kTimePrecision;
                    const std::string seg_text = decode_text_tokens(sliced);
                    if (seg_start != seg_end && !trim_copy(seg_text).empty()) {
                        segment_texts.push_back(seg_text);
                        all_tokens.insert(all_tokens.end(), sliced.begin(), sliced.end());
                    }
                    last_slice = current_slice;
                }
                if (single_timestamp_ending) {
                    seek += segment_size;  // no speech after the last timestamp
                } else {
                    // Ignore the unfinished segment; resume at the last timestamp.
                    seek += static_cast<size_t>(std::max(0, ts_pos[last_slice - 1])) *
                            static_cast<size_t>(kInputStride);
                }
            } else {
                double duration = static_cast<double>(segment_size) * kTimePrecision;
                int last_pos = -1;
                for (size_t i = 0; i < tokens.size(); ++i) {
                    if (is_ts[i]) last_pos = ts_pos[i];
                }
                if (last_pos > 0) {
                    duration = static_cast<double>(last_pos) * kTimePrecision;
                }
                const double seg_start = time_offset;
                const double seg_end = time_offset + duration;
                const std::string seg_text = decode_text_tokens(tokens);
                if (seg_start != seg_end && !trim_copy(seg_text).empty()) {
                    segment_texts.push_back(seg_text);
                    all_tokens.insert(all_tokens.end(), tokens.begin(), tokens.end());
                }
                seek += segment_size;
            }

            // faster-whisper: reset the conditioning prompt after a window that
            // needed a temperature above prompt_reset_on_temperature.
            if (temperature > kPromptResetOnTemperature) {
                prompt_reset_since = all_tokens.size();
            }
        }

        // jarvis_ears.py: " ".join([seg.text for seg in segments]).strip()
        std::string text;
        for (const auto& seg : segment_texts) {
            if (!text.empty()) text += " ";
            text += seg;
        }
        return trim_copy(text);
    } catch (const std::exception& e) {
        std::cerr << "[CT2] Whisper transcription failed: " << e.what() << std::endl;
        return "";
    } catch (...) {
        std::cerr << "[CT2] Whisper transcription failed with unknown error" << std::endl;
        return "";
    }
}
STTEngineWrapper::STTEngineWrapper()
    : initialized_(false), running_(false), processing_thread_running_(false) {
}

STTEngineWrapper::~STTEngineWrapper() {
    stop();
}

bool STTEngineWrapper::initialize() {
    initialized_ = true;
    return true;
}

void STTEngineWrapper::set_model_paths(const std::string& silero_vad_model,
                                       const std::string& ct2_model_dir) {
    silero_vad_model_ = silero_vad_model;
    ct2_model_dir_ = ct2_model_dir;
}

bool STTEngineWrapper::start() {
    if (!initialized_ || running_) {
        return false;
    }

    stop_capture_ = false;
    {
        std::lock_guard<std::mutex> lock(samples_queue_mutex_);
        std::queue<std::vector<float>> empty;
        samples_queue_.swap(empty);
    }

    if (ct2_model_dir_.empty()) {
        std::cerr << "STTEngine: model paths not configured" << std::endl;
        return false;
    }

    auto stt_load_start = std::chrono::high_resolution_clock::now();

    // 1. Jarvis ears wake word (openWakeWord ONNX models)
    wakeword_ = std::make_unique<WakeWordEngine>();
    if (!wakeword_ || !wakeword_->initialize(wakeword_model_path_, melspec_model_path_,
                                             embedding_model_path_)) {
        std::cerr << "STTEngine: Failed to load wake word models - wake word detection will be disabled" << std::endl;
        wakeword_.reset(); // Disable wake word if models are missing
        // Don't return false - allow the application to continue without wake word
    }

    // 2. Silero VAD only for the wake-word-disabled fallback path
    if (!silero_vad_model_.empty()) {
        vad_ = std::make_unique<VADEngine>();
        if (vad_ && vad_->initialize(silero_vad_model_)) {
            vad_->set_voice_segment_callback([this](const std::vector<float>& segment, int sample_rate) {
                on_vad_segment(segment, sample_rate);
            });
            // std::cout << "[STT] VAD loaded (fallback path only)" << std::endl;
        } else {
            vad_.reset();
        }
    }

    // 3. Whisper recognizer (jarvis_ears.py: WhisperModel("base"))
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    const int inference_threads = hardware_threads > 0
        ? static_cast<int>(std::clamp(hardware_threads, 4u, 16u))
        : 4;
    // CT2 reads its global thread count at load/forward time
    ctranslate2::set_num_threads(inference_threads);
    recognizer_ = std::make_unique<CTranslate2WhisperRecognizer>();
    if (!recognizer_ || !recognizer_->initialize(ct2_model_dir_, "", language_)) {
        std::cerr << "STTEngine: Failed to create recognizer" << std::endl;
        return false;
    }
    // std::cout << "[STT] CTranslate2 Whisper model loaded successfully" << std::endl;
    auto stt_load_end = std::chrono::high_resolution_clock::now();
    auto stt_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stt_load_end - stt_load_start).count();
    LOG_DEBUG("TELEMETRY", "STT_MODEL_LOAD: " + std::to_string(stt_load_ms) + " ms");

    chunk_accumulator_.clear();
    recording_ = false;
    recording_buffer_.clear();

    running_ = true;
    processing_thread_running_ = true;
    processing_thread_ = std::thread(&STTEngineWrapper::processing_loop, this);

    transcription_thread_running_ = true;
    transcription_thread_ = std::thread(&STTEngineWrapper::transcription_loop, this);

    // stdstd::cout << "[STT] Jarvis ears STT pipeline started. Listening for wake word..." << std::endl;
    return true;
}

void STTEngineWrapper::stop() {
    processing_thread_running_ = false;
    stop_capture_ = true;
    samples_queue_cv_.notify_all();

    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }

    transcription_thread_running_ = false;
    segment_queue_cv_.notify_all();
    if (transcription_thread_.joinable()) {
        transcription_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(samples_queue_mutex_);
        std::queue<std::vector<float>> empty;
        samples_queue_.swap(empty);
    }
    {
        std::lock_guard<std::mutex> lock(segment_queue_mutex_);
        std::queue<std::vector<float>> empty;
        segment_queue_.swap(empty);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::queue<std::string> empty;
        transcription_queue_.swap(empty);
    }

    wakeword_.reset();
    vad_.reset();
    recognizer_.reset();

    running_ = false;
    queue_cv_.notify_all();
}

void STTEngineWrapper::set_muted(bool muted) {
    muted_ = muted;
    if (!muted) return;

    {
        std::lock_guard<std::mutex> lock(samples_queue_mutex_);
        std::queue<std::vector<float>> empty;
        samples_queue_.swap(empty);
    }

    {
        std::lock_guard<std::mutex> seg_lock(segment_queue_mutex_);
        std::queue<std::vector<float>> empty_seg;
        segment_queue_.swap(empty_seg);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::queue<std::string> empty;
        transcription_queue_.swap(empty);
    }

    chunk_accumulator_.clear();
    recording_ = false;
    recording_buffer_.clear();

    if (vad_) {
        vad_->reset();
    }
}

void STTEngineWrapper::send_audio(const std::vector<float>& audio_data, int sample_rate) {
    (void)sample_rate;
    // static int send_audio_count = 0;
    // if (send_audio_count < 3 || (send_audio_count % 100) == 0) {
    //     std::cout << "[STT] send_audio called: " << audio_data.size() << " samples, running=" << running_ << ", muted=" << muted_ << std::endl;
    // }
    // send_audio_count++;

    if (!running_ || audio_data.empty() || muted_) {
        return;
    }

    std::lock_guard<std::mutex> lock(samples_queue_mutex_);
    if (samples_queue_.size() >= kMaxAudioQueueChunks) {
        // Drop the oldest audio so recognition stays near live input.
        samples_queue_.pop();
    }
    samples_queue_.push(audio_data);
    samples_queue_cv_.notify_one();
}

std::string STTEngineWrapper::get_transcription() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return !transcription_queue_.empty(); });

    if (transcription_queue_.empty()) {
        return "";
    }

    std::string result = transcription_queue_.front();
    transcription_queue_.pop();
    return result;
}

void STTEngineWrapper::enqueue_segment(std::vector<float> segment) {
    if (segment.empty()) return;
    {
        std::lock_guard<std::mutex> lock(segment_queue_mutex_);
        segment_queue_.push(std::move(segment));
    }
    segment_queue_cv_.notify_one();
}

void STTEngineWrapper::on_vad_segment(const std::vector<float>& segment, int sample_rate) {
    // Wake-word-disabled fallback path only (no jarvis equivalent).
    LOG_STT_DEBUG("VAD segment delivered: " + std::to_string(segment.size()) + " samples");
    enqueue_segment(segment);
}

// jarvis_ears.py main loop, line by line:
//   data = stream.read(CHUNK, exception_on_overflow=False)   -> 512-sample frames
//   prediction = wakeword_model.predict(audio_frame)          -> WakeWordEngine
//   if prediction["Jarvis"] > 0.5:                            -> score > 0.5
//       play_wake_sound(); send_to_node(...wake_word_detected)
//       listen_and_transcribe(stt_model, stream)              -> 4 s, transcribe
//       wakeword_model.reset()                                -> reset
void STTEngineWrapper::processing_loop() {
    LOG_STT("Jarvis ears processing loop started");

    // One-shot VAD reset marker for the AEC window (see the
    // assistant_speaking_ gate in the wake-word-disabled path below).
    bool vad_reset_after_aec_ = false;

    while (processing_thread_running_) {
        try {
            std::vector<float> chunk;
            {
                std::unique_lock<std::mutex> lock(samples_queue_mutex_);
                if (samples_queue_.empty()) {
                    samples_queue_cv_.wait_for(lock, std::chrono::milliseconds(kProcessingIntervalMs));
                }
                if (samples_queue_.empty()) {
                    continue;
                }
                chunk = std::move(samples_queue_.front());
                samples_queue_.pop();
            }

            if (chunk.empty() || muted_) {
                continue;
            }

            // Wake-word-disabled fallback: legacy VAD segmentation.
            if (!wake_word_enabled_ || !wakeword_) {
                // AEC (jarvis_ears.py is_mouth_speaking()): the
                // assistant-speaking gate must cover the VAD path too, not
                // just the wake-word path. Otherwise, while Sonny's TTS
                // plays through the speakers, Silero segments Sonny's own
                // voice and whisper transcribes it as the user's command
                // (the distortion reported with the wake word disabled).
                if (assistant_speaking_) {
                    if (vad_ && !vad_reset_after_aec_) {
                        // Drop any half-completed recording so its tail is
                        // not glued to the next utterance after the AEC
                        // window closes.
                        vad_->reset();
                        vad_reset_after_aec_ = true;
                    }
                    continue;
                }
                vad_reset_after_aec_ = false;
                if (vad_) {
                    vad_->process_audio(chunk);
                }
                continue;
            }

            // Accumulate into 512-sample int16 frames (jarvis CHUNK = 512).
            // AudioCapture delivers float = int16 / 32768, so * 32768 recovers
            // the exact original int16 samples jarvis_ears.py reads from PyAudio.
            for (float s : chunk) {
                float clamped = std::max(-1.0f, std::min(1.0f, s));
                chunk_accumulator_.push_back(static_cast<int16_t>(
                    std::max(-32768.0f, std::min(32767.0f, clamped * 32768.0f))));
            }

            while (chunk_accumulator_.size() >= kEarsChunkSamples) {
                std::vector<int16_t> frame(chunk_accumulator_.begin(),
                                           chunk_accumulator_.begin() + static_cast<long long>(kEarsChunkSamples));
                chunk_accumulator_.erase(chunk_accumulator_.begin(),
                                         chunk_accumulator_.begin() + static_cast<long long>(kEarsChunkSamples));

                if (recording_) {
                    // listen_and_transcribe: record exactly 4 seconds of command.
                    // frame is int16 PCM; convert back to normalized [-1, 1] float
                    // (the wake word consumed int16, but Whisper needs the same
                    // [-1, 1] scale faster-whisper/PyAudio delivers).
                    for (int16_t sample : frame) {
                        recording_buffer_.push_back(static_cast<float>(sample) / 32768.0f);
                    }
                    if (recording_buffer_.size() >= kRecordSamples) {
                        // std::cout << "[STT] Recording complete (" << recording_buffer_.size()
                        //           << " samples), transcribing..." << std::endl;
                        enqueue_segment(std::move(recording_buffer_));
                        recording_buffer_.clear();
                        recording_ = false;
                    }
                    continue;
                }

                const float prediction = wakeword_->predict(frame);

                static int wake_debug_count = 0;
                if (wake_debug_count < 5 || (wake_debug_count % 200) == 0) {
                    LOG_DEBUG_COMPONENT("WAKE", "score: " + std::to_string(prediction));
                }
                wake_debug_count++;

                if (prediction > 0.5f) {
                    // AEC: while the assistant is speaking, do not trigger the wake sound
                    // or start recording (prevents assistant from triggering itself via speakers).
                    if (assistant_speaking_) {
                        LOG_STT("AEC: Assistant is speaking, ignoring wake word trigger.");
                        wakeword_->reset();
                        continue;
                    }
                    LOG_WAKE("Wake word detected (score " + std::to_string(prediction) + ")");
                    if (wake_word_callback_) {
                        wake_word_callback_();   // wake sound (jarvis play_wake_sound)
                    }
                    wakeword_->reset();          // jarvis wakeword_model.reset()
                    recording_ = true;           // listen_and_transcribe starts
                    recording_buffer_.clear();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[STT] Exception in processing loop: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[STT] Unknown exception in processing loop" << std::endl;
        }
    }

    // std::cout << "[STT] Jarvis ears processing loop stopped" << std::endl;
}

void STTEngineWrapper::transcription_loop() {
    // std::cout << "[STT] Transcription thread started" << std::endl;

    while (transcription_thread_running_) {
        std::vector<float> segment;
        {
            std::unique_lock<std::mutex> lock(segment_queue_mutex_);
            if (!segment_queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                            [this] { return !segment_queue_.empty() || !transcription_thread_running_; })) {
                continue;
            }
            if (!transcription_thread_running_) break;
            if (segment_queue_.empty()) continue;

            segment = std::move(segment_queue_.front());
            segment_queue_.pop();
        }

        if (segment.empty()) continue;

        try {
            // jarvis_ears.py: one transcribe call per 4 s recording, text sent
            // as-is (no junk filtering, no deduplication).
            auto stt_start = std::chrono::high_resolution_clock::now();
            std::string text = recognizer_->transcribe(segment, 16000);
            auto stt_end = std::chrono::high_resolution_clock::now();
            auto stt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(stt_end - stt_start).count();
            if (!text.empty()) {
                LOG_PERF("STT Transcription: " + std::to_string(stt_ms) + " ms");

                // CPU thermal throttling warning (one-time, mirrors LlamaWrapper GPU warning):
                // on a healthy CPU, 4 s of audio should transcribe in well under 4 s
                // (real-time factor < 1.0). RTF > 1.0 indicates thermal/power throttling
                // or CPU starvation.
                const double audio_seconds = static_cast<double>(segment.size()) / 16000.0;
                if (!warned_slow_transcription_ && audio_seconds > 0.0 && stt_ms > 0) {
                    const double rtf = static_cast<double>(stt_ms) / 1000.0 / audio_seconds;
                    if (rtf > 1.0) {
                        warned_slow_transcription_ = true;
                        std::ostringstream warn;
                        warn << "Slow transcription (RTF " << std::fixed << std::setprecision(2) << rtf
                             << "x, " << stt_ms << " ms for " << std::fixed << std::setprecision(1)
                             << audio_seconds << " s audio). Causes: CPU thermal/power throttling, "
                             << "CPU starvation, or insufficient threads.";
                        LOG_WARN("STT", warn.str());
                    }
                }

                std::lock_guard<std::mutex> lock(queue_mutex_);
                // Bound the queue so a stalled consumer cannot grow it forever.
                if (transcription_queue_.size() >= 10) {
                    std::queue<std::string> empty;
                    transcription_queue_.swap(empty);
                }
                transcription_queue_.push(text);
                queue_cv_.notify_one();
            }
        } catch (const std::exception& e) {
            LOG_ERROR("STT", std::string("Transcription error: ") + e.what());
        } catch (...) {
            LOG_ERROR("STT", "Unknown transcription error");
        }
    }

    // std::cout << "[STT] Transcription thread stopped" << std::endl;
}
