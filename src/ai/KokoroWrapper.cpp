#include "KokoroWrapper.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "g2p.hpp"
#include "phonemizer.hpp"

#include "Logger.h"

namespace {

std::filesystem::path first_existing(const std::filesystem::path& root,
                                     std::initializer_list<const char*> rel_paths) {
    for (const char* rel : rel_paths) {
        std::filesystem::path p = root / rel;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

KokoroWrapper::KokoroWrapper() : initialized_(false) {}

KokoroWrapper::~KokoroWrapper() {
    if (initialized_) {
        // Balance the init() calls made in initialize().
        kokoro::G2P::terminate();
        kokoro::Phonemizer::terminate();
    }
}

bool KokoroWrapper::initialize(const std::string& model_dir, const std::string& accelerator) {
    const std::filesystem::path root(model_dir);
    const auto vocab   = first_existing(root, {"Kokoro-82M/config.json", "config.json"});
    const auto encoder = first_existing(root, {"onnx/kokoro_encoder.onnx", "kokoro_encoder.onnx"});
    const auto har     = first_existing(root, {"onnx/har_generator.onnx", "har_generator.onnx"});
    const auto decoder = first_existing(root, {"onnx/kokoro_decoder.onnx", "kokoro_decoder.onnx"});
    const auto voices  = first_existing(root, {"voices_npy"});
    const auto espeak  = first_existing(root, {"espeak-ng-data"});
    const auto lexicon = first_existing(root, {"misaki-data"});

    if (vocab.empty() || encoder.empty() || har.empty() || decoder.empty() ||
        voices.empty() || espeak.empty() || lexicon.empty()) {
        LOG_DEBUG_COMPONENT("Kokoro", "Kokoro model files incomplete under " + model_dir + " - TTS disabled");
        return false;
    }

    try {
        // Same setup sequence as kokoro-cli: global phonemizer/G2P state first,
        // then the engine with its three ONNX sessions.
        kokoro::Phonemizer::init(espeak.string());
        kokoro::G2P::init(lexicon.string(), espeak.string());

        kokoro::EngineConfig cfg;
        cfg.decoderWorkers = 1;  // ONNX decoder (3 is for the RKNN multi-context build)

        auto engine = std::make_unique<kokoro::Engine>();
        engine->load(vocab.string(), encoder.string(), har.string(), decoder.string(),
                     voices.string(), accelerator, cfg);
        engine_ = std::move(engine);

        available_voices_ = engine_->listVoices();
        for (auto& voice : available_voices_) {
            size_t dot = voice.rfind('.');
            if (dot != std::string::npos) voice = voice.substr(0, dot);
        }
    } catch (const std::exception& e) {
        LOG_DEBUG_COMPONENT("Kokoro", std::string("Kokoro engine init failed: ") + e.what());
        engine_.reset();
        return false;
    }

    sample_rate_ = engine_->config().sampleRate;
    initialized_ = true;
    LOG_DEBUG_COMPONENT("Kokoro", "In-process Kokoro TTS ready (" +
                        std::to_string(available_voices_.size()) + " voice packs)");
    return true;
}

std::vector<float> KokoroWrapper::synthesize(const std::string& text, const std::string& voice_name, float speed) {
    if (!initialized_ || !engine_) {
        LOG_DEBUG_COMPONENT("Kokoro", "TTS engine is not initialized");
        return {};
    }

    size_t first_non_space = text.find_first_not_of(" \t\r\n");
    if (first_non_space == std::string::npos) return {};
    size_t last_non_space = text.find_last_not_of(" \t\r\n");
    std::string trimmed_text = text.substr(first_non_space, last_non_space - first_non_space + 1);
    if (trimmed_text.empty()) return {};

    std::lock_guard<std::mutex> lock(tts_mutex_);

    std::vector<int16_t> pcm;
    try {
        // Only the en-US / en-GB distinction is supported by the C++ pipeline.
        std::string lang = to_lower_copy(current_language_);
        bool british = (lang == "en-gb" || lang == "en_gb");
        kokoro::SynthesisResult result = engine_->synthesizeText(
            trimmed_text, voice_name, speed, british,
            [&](const int16_t* data, std::size_t n) {
                pcm.insert(pcm.end(), data, data + n);
            });
        if (pcm.empty()) {
            LOG_DEBUG_COMPONENT("Kokoro", "Synthesis produced no audio for: " + trimmed_text.substr(0, 80));
        } else {
            LOG_DEBUG_COMPONENT("Kokoro",
                "Synthesized " + std::to_string(pcm.size()) + " samples (" +
                std::to_string(result.audioSeconds) + "s audio, RTF " +
                std::to_string(result.realTimeFactor) + ")");
        }
    } catch (const std::exception& e) {
        LOG_DEBUG_COMPONENT("Kokoro", std::string("Synthesis failed: ") + e.what());
        return {};
    }

    std::vector<float> result(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        result[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    return result;
}

std::vector<std::string> KokoroWrapper::get_available_voices() const {
    return available_voices_;
}
