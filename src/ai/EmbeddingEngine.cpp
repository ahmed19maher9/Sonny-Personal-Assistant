#include "EmbeddingEngine.h"
#include "HttpClient.h"
#include "Logger.h"
#include "PathUtil.h"
#include "resource_ids.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace Jarvis {

// ============================================================================
// BERT-style WordPiece encoder.
//
// The vocabulary is the plain `vocab.txt` shipped with the model (one token per
// line, line number == token id). Pre-tokenization is intentionally simple and
// predictable: ASCII alphanumerics form words (lowercased), every other UTF-8
// code point (punctuation, CJK, accents) is emitted as its own token. That is a
// small deviation from the reference BasicTokenizer, which also strips accents;
// it costs a little accuracy on non-English text for code that is easy to audit.
// ============================================================================
struct EmbeddingEngine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SonnyEmbedding"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU);

    std::vector<std::string> input_name_storage;
    std::vector<std::string> output_name_storage;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;

    int ids_index = -1;
    int mask_index = -1;
    int types_index = -1;
    int hidden_index = -1;              // index of last_hidden_state
    bool uses_sentence_embedding = false;

    std::unordered_map<std::string, int32_t> vocab;
    int32_t cls_id = 101;
    int32_t sep_id = 102;
    int32_t unk_id = 100;

    std::mutex mutex;

    bool load_vocab(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return load_vocab_text(buffer.str());
    }

    // Accepts the raw contents of vocab.txt so a bundled vocabulary can be
    // parsed without touching the filesystem.
    bool load_vocab_text(const std::string& text) {
        vocab.clear();
        vocab.reserve(32768);

        size_t start = 0;
        int32_t id = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            vocab.emplace(line, id);
            ++id;
            start = end + 1;
        }
        if (vocab.empty()) return false;

        auto id_of = [this](const char* token, int32_t fallback) {
            auto it = vocab.find(token);
            return it == vocab.end() ? fallback : it->second;
        };
        cls_id = id_of("[CLS]", cls_id);
        sep_id = id_of("[SEP]", sep_id);
        unk_id = id_of("[UNK]", unk_id);
        return true;
    }

    void tokenize_word(const std::string& word, std::vector<int32_t>& out) const {
        constexpr size_t kMaxCharsPerWord = 100;
        if (word.empty()) return;
        if (word.size() > kMaxCharsPerWord) {
            out.push_back(unk_id);
            return;
        }

        size_t start = 0;
        std::vector<int32_t> pieces;
        while (start < word.size()) {
            size_t end = word.size();
            bool matched = false;
            while (end > start) {
                std::string piece = word.substr(start, end - start);
                if (start > 0) piece.insert(0, "##");
                auto it = vocab.find(piece);
                if (it != vocab.end()) {
                    pieces.push_back(it->second);
                    start = end;
                    matched = true;
                    break;
                }
                --end;
            }
            if (!matched) {
                out.push_back(unk_id);
                return;
            }
        }
        out.insert(out.end(), pieces.begin(), pieces.end());
    }

    // Fills ids/mask/types with one [CLS] ... [SEP] sequence.
    void encode(const std::string& text,
                int max_seq_len,
                std::vector<int32_t>& ids,
                std::vector<int32_t>& mask,
                std::vector<int32_t>& types) const {
        ids.clear();
        mask.clear();
        types.clear();
        ids.push_back(cls_id);

        const size_t budget = max_seq_len > 4 ? static_cast<size_t>(max_seq_len - 2) : 64;
        std::vector<int32_t> pieces;
        pieces.reserve(64);

        std::string word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            std::string lowered = word;
            for (char& c : lowered) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            tokenize_word(lowered, pieces);
            word.clear();
        };

        for (size_t i = 0; i < text.size() && pieces.size() < budget;) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x80) {
                if (std::isalnum(c)) {
                    word.push_back(static_cast<char>(c));
                } else {
                    flush_word();
                    std::string single(1, static_cast<char>(c));
                    auto it = vocab.find(single);
                    pieces.push_back(it == vocab.end() ? unk_id : it->second);
                }
                ++i;
                continue;
            }

            // Multi-byte code point: keep the whole UTF-8 sequence as one token.
            size_t length = 1;
            if ((c & 0xE0) == 0xC0) length = 2;
            else if ((c & 0xF0) == 0xE0) length = 3;
            else if ((c & 0xF8) == 0xF0) length = 4;
            if (i + length > text.size()) length = 1;

            flush_word();
            std::string code_point = text.substr(i, length);
            auto it = vocab.find(code_point);
            pieces.push_back(it == vocab.end() ? unk_id : it->second);
            i += length;
        }
        flush_word();

        if (pieces.size() > budget) pieces.resize(budget);
        ids.insert(ids.end(), pieces.begin(), pieces.end());
        ids.push_back(sep_id);

        mask.assign(ids.size(), 1);
        types.assign(ids.size(), 0);
    }

    // Runs the encoder and returns a unit-length embedding (empty on failure).
    std::vector<float> run(const std::vector<int32_t>& ids,
                           const std::vector<int32_t>& mask,
                           const std::vector<int32_t>& types);
};

std::vector<float> EmbeddingEngine::Impl::run(const std::vector<int32_t>& ids,
                                              const std::vector<int32_t>& mask,
                                              const std::vector<int32_t>& types) {
    if (!session) return std::vector<float>();

    std::vector<int64_t> ids64(ids.begin(), ids.end());
    std::vector<int64_t> mask64(mask.begin(), mask.end());
    std::vector<int64_t> types64(types.begin(), types.end());
    const std::array<int64_t, 2> shape{1, static_cast<int64_t>(ids64.size())};

    // ONNX Runtime matches inputs by name, but the values must be supplied in
    // the session's own input order - so build names and tensors together and
    // refuse (rather than guess) if the model declares an unexpected input.
    std::vector<const char*> names;
    std::vector<Ort::Value> values;
    for (size_t i = 0; i < input_name_storage.size(); ++i) {
        const int index = static_cast<int>(i);
        names.push_back(input_names[i]);
        if (index == ids_index) {
            values.emplace_back(Ort::Value::CreateTensor<int64_t>(memory, ids64.data(), ids64.size(),
                                                                 shape.data(), shape.size()));
        } else if (index == mask_index) {
            values.emplace_back(Ort::Value::CreateTensor<int64_t>(memory, mask64.data(),
                                                                  mask64.size(), shape.data(),
                                                                  shape.size()));
        } else if (index == types_index) {
            values.emplace_back(Ort::Value::CreateTensor<int64_t>(memory, types64.data(),
                                                                  types64.size(), shape.data(),
                                                                  shape.size()));
        } else {
            return std::vector<float>();
        }
    }
    if (values.empty()) return std::vector<float>();

    auto outputs = session->Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(),
                                output_names.data(), output_names.size());
    if (outputs.empty() || hidden_index < 0 || (size_t)hidden_index >= outputs.size()) {
        return std::vector<float>();
    }

    std::vector<float> result;
    if (uses_sentence_embedding) {
        const auto dims = outputs[hidden_index].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.empty()) return std::vector<float>();
        const size_t count = static_cast<size_t>(dims.back());
        if (count == 0) return std::vector<float>();
        const float* data = outputs[hidden_index].GetTensorMutableData<float>();
        result.assign(data, data + count);
    } else {
        const auto dims = outputs[hidden_index].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.size() != 3) return std::vector<float>();
        const size_t seq = static_cast<size_t>(dims[1]);
        const size_t dim = static_cast<size_t>(dims[2]);
        if (seq == 0 || dim == 0) return std::vector<float>();

        const float* data = outputs[hidden_index].GetTensorMutableData<float>();
        result.assign(dim, 0.0f);
        float weight_sum = 0.0f;
        for (size_t t = 0; t < seq; ++t) {
            const float weight = t < mask64.size() ? static_cast<float>(mask64[t]) : 0.0f;
            if (weight == 0.0f) continue;
            const float* row = data + t * dim;
            for (size_t d = 0; d < dim; ++d) result[d] += row[d] * weight;
            weight_sum += weight;
        }
        if (weight_sum <= 0.0f) return std::vector<float>();
        for (float& value : result) value /= weight_sum;
    }

    double norm = 0.0;
    for (float value : result) norm += static_cast<double>(value) * value;
    norm = std::sqrt(norm);
    if (norm > 1e-9) {
        for (float& value : result) value = static_cast<float>(value / norm);
    }
    return result;
}

EmbeddingEngine::EmbeddingEngine() : impl_(std::make_unique<Impl>()) {}

EmbeddingEngine::~EmbeddingEngine() {
    shutdown();
}

void EmbeddingEngine::shutdown() {
    ready_ = false;
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->session.reset();
}

std::vector<float> EmbeddingEngine::embed(const std::string& text) {
    if (!ready_ || !impl_ || text.empty()) return std::vector<float>();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        std::vector<int32_t> ids;
        std::vector<int32_t> mask;
        std::vector<int32_t> types;
        impl_->encode(text, max_seq_len_, ids, mask, types);
        std::vector<float> vector = impl_->run(ids, mask, types);
        if (dim_ == 0 && !vector.empty()) dim_ = static_cast<int>(vector.size());
        return vector;
    } catch (const Ort::Exception& e) {
        LOG_DEBUG_COMPONENT("Embed", std::string("Inference failed: ") + e.what());
    } catch (const std::exception& e) {
        LOG_DEBUG_COMPONENT("Embed", std::string("Inference failed: ") + e.what());
    }
    return std::vector<float>();
}

float EmbeddingEngine::cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        norm_a += static_cast<double>(a[i]) * a[i];
        norm_b += static_cast<double>(b[i]) * b[i];
    }
    if (norm_a <= 0.0 || norm_b <= 0.0) return 0.0f;
    return static_cast<float>(dot / (std::sqrt(norm_a) * std::sqrt(norm_b)));
}

std::string EmbeddingEngine::resolve_model_file(const std::string& configured) {
    const std::string name = configured.empty() ? "all-MiniLM-L6-v2" : configured;

    auto is_regular_file = [](const std::string& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    };
    auto is_directory = [](const std::string& path) {
        std::error_code ec;
        return std::filesystem::is_directory(path, ec);
    };
    auto ends_with_onnx = [](const std::string& path) {
        return path.size() > 5 && path.compare(path.size() - 5, 5, ".onnx") == 0;
    };

    // Preferred names, best first: the quantized AVX2 export is ~4x smaller and
    // markedly faster on CPU than the fp32 export, with a negligible quality gap.
    const std::vector<std::string> preferred = {
        "model_quint8_avx2.onnx",
        "model_quint8_avx512_vnni.onnx",
        "model_qint8_avx512_vnni.onnx",
        "model_quantized.onnx",
        "model.onnx"};

    auto scan_directory = [&](const std::string& directory) -> std::string {
        for (const std::string& file : preferred) {
            const std::string candidate = directory + "/" + file;
            if (is_regular_file(candidate)) return candidate;
        }
        std::error_code ec;
        std::filesystem::directory_iterator it(directory, ec);
        const std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            const std::string candidate = it->path().string();
            if (ends_with_onnx(candidate)) return candidate;
        }
        return "";
    };

    const std::vector<std::string> roots = {
        name,
        "resources/models/" + name,
        "resources/models/" + name + "-onnx",
        "models/" + name,
        "external/models/" + name};

    for (const std::string& root : roots) {
        if (is_regular_file(root) && ends_with_onnx(root)) return root;
        if (!is_directory(root)) continue;

        std::string found = scan_directory(root);
        if (!found.empty()) return found;

        // HuggingFace keeps the ONNX export one level down (repo/onnx/model.onnx).
        const std::string nested = root + "/onnx";
        if (is_directory(nested)) {
            found = scan_directory(nested);
            if (!found.empty()) return found;
        }
    }
    return "";
}

bool EmbeddingEngine::initialize(const std::string& model_path_or_name, int max_seq_len) {
    ready_ = false;
    max_seq_len_ = max_seq_len > 8 ? max_seq_len : 256;
    if (!impl_) impl_ = std::make_unique<Impl>();

    // 1. A model compiled into the executable wins: it needs no file on disk and
    //    no network access, which is what a normal install ships.
    std::vector<unsigned char> bundled_bytes;
    std::string bundled_vocab;
    if (read_bundled_model(bundled_bytes, bundled_vocab) &&
        initialize_from_memory(bundled_bytes.data(), bundled_bytes.size(), bundled_vocab,
                               "embedded resource", max_seq_len)) {
        // Keep the bytes: the ONNX session references this buffer.
        model_bytes_ = std::move(bundled_bytes);
        return true;
    }

    // 2. Otherwise use an ONNX model from disk. Nothing is ever downloaded.
    const std::string model_file = resolve_model_file(model_path_or_name);
    if (model_file.empty()) {
        LOG_INFO("Embed", "No embedding model available (checked the embedded resource and '" +
                              model_path_or_name + "') - retrieval stays lexical (BM25 only).");
        LOG_INFO("Embed", "Ship an ONNX model + vocab.txt under models/<name>/, or compile one into "
                          "the executable with -DSONNY_EMBED_MODEL_FILE=<path to model.onnx>");
        return false;
    }

    const std::filesystem::path model_path(model_file);
    std::string vocab_file;
    const std::vector<std::filesystem::path> vocab_candidates = {
        model_path.parent_path() / "vocab.txt",
        model_path.parent_path().parent_path() / "vocab.txt"};
    for (const std::filesystem::path& candidate : vocab_candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            vocab_file = candidate.string();
            break;
        }
    }
    if (vocab_file.empty()) {
        LOG_WARN("Embed", "vocab.txt not found next to " + model_file + " - embeddings disabled.");
        return false;
    }
    if (!impl_->load_vocab(vocab_file)) {
        LOG_WARN("Embed", "Failed to read the WordPiece vocabulary: " + vocab_file);
        return false;
    }

    bool session_created = false;
    try {
        impl_->options.SetIntraOpNumThreads(2);
        impl_->options.SetInterOpNumThreads(1);
        impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const std::wstring wide_model = pathutil::utf8_to_wide(model_file);
        impl_->session = std::make_unique<Ort::Session>(impl_->env, wide_model.c_str(), impl_->options);
        session_created = true;
    } catch (const Ort::Exception& e) {
        LOG_WARN("Embed", std::string("ONNX session creation failed: ") + e.what());
    } catch (const std::exception& e) {
        LOG_WARN("Embed", std::string("Embedding model load failed: ") + e.what());
    }

    if (!session_created) {
        impl_->session.reset();
        return false;
    }

    model_path_ = model_file;
    return finish_initialization(model_file);
}

// Loads a model that is already in memory: the RCDATA resource compiled into
// this executable, or bytes supplied by a caller. No file or network access.
bool EmbeddingEngine::initialize_from_memory(const void* model_data,
                                             size_t model_data_length,
                                             const std::string& vocab_text,
                                             const std::string& source_label,
                                             int max_seq_len) {
    ready_ = false;
    max_seq_len_ = max_seq_len > 8 ? max_seq_len : 256;
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (!model_data || model_data_length == 0 || vocab_text.empty()) return false;
    if (!impl_->load_vocab_text(vocab_text)) {
        LOG_WARN("Embed", "Failed to parse the embedded WordPiece vocabulary.");
        return false;
    }

    try {
        impl_->options.SetIntraOpNumThreads(2);
        impl_->options.SetInterOpNumThreads(1);
        impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->session = std::make_unique<Ort::Session>(impl_->env, model_data, model_data_length,
                                                        impl_->options);
    } catch (const Ort::Exception& e) {
        LOG_WARN("Embed", std::string("ONNX session creation from memory failed: ") + e.what());
        impl_->session.reset();
        return false;
    } catch (const std::exception& e) {
        LOG_WARN("Embed", std::string("Embedding model load failed: ") + e.what());
        impl_->session.reset();
        return false;
    }

    model_path_ = source_label;
    return finish_initialization(source_label);
}

// Wires the I/O signature, reads the embedding dimension, then self-tests once
// so a mismatched or corrupt model is reported at startup rather than on the
// user's first question.
bool EmbeddingEngine::finish_initialization(const std::string& label) {
    if (!impl_ || !impl_->session) return false;

    try {
        // Discover the I/O signature instead of assuming it: exports differ on
        // whether token_type_ids exists and on how many outputs are exposed.
        Ort::AllocatorWithDefaultOptions allocator;
        impl_->input_name_storage.clear();
        impl_->ids_index = -1;
        impl_->mask_index = -1;
        impl_->types_index = -1;
        for (size_t i = 0; i < impl_->session->GetInputCount(); ++i) {
            auto allocated = impl_->session->GetInputNameAllocated(i, allocator);
            const std::string name = allocated.get();
            impl_->input_name_storage.push_back(name);
            if (name == "input_ids") impl_->ids_index = static_cast<int>(i);
            else if (name == "attention_mask") impl_->mask_index = static_cast<int>(i);
            else if (name == "token_type_ids") impl_->types_index = static_cast<int>(i);
        }
        impl_->input_names.clear();
        for (const std::string& name : impl_->input_name_storage) {
            impl_->input_names.push_back(name.c_str());
        }

        impl_->output_name_storage.clear();
        impl_->uses_sentence_embedding = false;
        impl_->hidden_index = -1;
        for (size_t i = 0; i < impl_->session->GetOutputCount(); ++i) {
            auto allocated = impl_->session->GetOutputNameAllocated(i, allocator);
            const std::string name = allocated.get();
            impl_->output_name_storage.push_back(name);
            if (name == "sentence_embedding") {
                impl_->uses_sentence_embedding = true;
                impl_->hidden_index = static_cast<int>(i);
            } else if (name == "last_hidden_state" && !impl_->uses_sentence_embedding) {
                impl_->hidden_index = static_cast<int>(i);
            }
        }
        impl_->output_names.clear();
        for (const std::string& name : impl_->output_name_storage) {
            impl_->output_names.push_back(name.c_str());
        }

        if (impl_->ids_index < 0 || impl_->mask_index < 0) {
            LOG_WARN("Embed", "Model exposes no input_ids/attention_mask inputs - embeddings disabled.");
            impl_->session.reset();
            return false;
        }
        if (impl_->hidden_index < 0) {
            if (impl_->session->GetOutputCount() == 0) {
                impl_->session.reset();
                return false;
            }
            impl_->hidden_index = 0;  // only sensible fallback: the first output
        }

        dim_ = 0;
        const std::vector<int64_t> shape = impl_->session
                                               ->GetOutputTypeInfo((size_t)impl_->hidden_index)
                                               .GetTensorTypeAndShapeInfo()
                                               .GetShape();
        if (!shape.empty() && shape.back() > 0) dim_ = static_cast<int>(shape.back());
        if (dim_ <= 0) dim_ = 384;
    } catch (const Ort::Exception& e) {
        LOG_WARN("Embed", std::string("Failed to inspect the embedding model I/O: ") + e.what());
        impl_->session.reset();
        return false;
    } catch (const std::exception& e) {
        LOG_WARN("Embed", std::string("Embedding model wiring failed: ") + e.what());
        impl_->session.reset();
        return false;
    }

    ready_ = true;

    // Probe once: a mismatched or partially-loaded model must be reported here,
    // not on the user's first question.
    const std::vector<float> probe = embed("sonny embedding self test");
    if (probe.empty()) {
        LOG_WARN("Embed", "Self-test embedding failed - embeddings disabled.");
        shutdown();
        return false;
    }

    LOG_INFO("Embed", "Embedding model ready (" + label + "): " + std::to_string(probe.size()) +
                          "-d, " + std::to_string(impl_->vocab.size()) + " vocab tokens");
    return true;
}

bool EmbeddingEngine::has_bundled_model() {
    std::vector<unsigned char> model_bytes;
    std::string vocab_text;
    return read_bundled_model(model_bytes, vocab_text);
}

// Reads the ONNX model + vocabulary compiled into the executable as RCDATA
// resources (see SONNY_EMBED_MODEL_FILE in CMakeLists.txt). This is what makes
// semantic retrieval work with no extra files on disk and no download step.
bool EmbeddingEngine::read_bundled_model(std::vector<unsigned char>& model_bytes,
                                         std::string& vocab_text) {
    model_bytes.clear();
    vocab_text.clear();

    HMODULE module = GetModuleHandle(nullptr);
    if (!module) return false;

    auto read_resource = [module](int resource_id, std::vector<unsigned char>& out) {
        HRSRC found = FindResource(module, MAKEINTRESOURCE(resource_id), RT_RCDATA);
        if (!found) return false;
        const DWORD size = SizeofResource(module, found);
        if (size == 0) return false;
        HGLOBAL loaded = LoadResource(module, found);
        if (!loaded) return false;
        const void* data = LockResource(loaded);
        if (!data) return false;
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        out.assign(bytes, bytes + size);
        // RCDATA lives in the mapped image: it stays valid for the whole
        // process and must not be freed.
        return true;
    };

    std::vector<unsigned char> vocab_bytes;
    if (!read_resource(IDR_EMBEDDING_MODEL, model_bytes)) return false;
    if (!read_resource(IDR_EMBEDDING_VOCAB, vocab_bytes)) {
        model_bytes.clear();
        return false;
    }

    vocab_text.assign(vocab_bytes.begin(), vocab_bytes.end());
    return !model_bytes.empty() && !vocab_text.empty();
}

}  // namespace Jarvis
