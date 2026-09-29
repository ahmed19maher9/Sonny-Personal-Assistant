#pragma once
// ============================================================================
// EmbeddingEngine.h - local text embeddings via ONNX Runtime.
//
// This is the "space vector" producer for the RAG engine: it maps a piece of
// text to a unit-length dense vector (384-d for all-MiniLM-L6-v2) so retrieval
// can be semantic instead of purely lexical (BM25).
//
// It is deliberately optional. When no ONNX model + vocab.txt can be found,
// initialize() returns false and RagEngine keeps working with BM25 only, so a
// missing model can never break the assistant.
//
// Accepted layouts:
//   <dir>/<any>.onnx  +  <dir>/vocab.txt
//   <file>.onnx       +  vocab.txt next to it
// A bare name such as "all-MiniLM-L6-v2" is resolved against
// resources/models/<name> and models/<name> (see resolve_model_file).
// ============================================================================

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Jarvis {

class EmbeddingEngine {
public:
    EmbeddingEngine();
    ~EmbeddingEngine();

    EmbeddingEngine(const EmbeddingEngine&) = delete;
    EmbeddingEngine& operator=(const EmbeddingEngine&) = delete;

    // Loads the model + vocabulary.
    //
    // Resolution order - no network access is ever performed:
    //   1. the model compiled into the executable as an RCDATA resource
    //      (see SONNY_EMBED_MODEL_FILE in CMakeLists.txt);
    //   2. an ONNX model on disk (resources/models/<name>, models/<name>, ...).
    // Never throws; returns false and leaves the engine unusable when neither
    // is available, in which case RagEngine keeps using BM25 only.
    bool initialize(const std::string& model_path_or_name, int max_seq_len = 256);

    // Loads a model that is already in memory (bundled resource, or bytes read
    // by the caller). `vocab_text` is the contents of vocab.txt.
    // The model buffer must stay valid for the lifetime of the engine.
    bool initialize_from_memory(const void* model_data,
                                size_t model_data_length,
                                const std::string& vocab_text,
                                const std::string& source_label,
                                int max_seq_len = 256);

    void shutdown();

    bool is_ready() const { return ready_; }
    int dimension() const { return dim_; }
    int max_seq_len() const { return max_seq_len_; }
    const std::string& model_path() const { return model_path_; }

    // Unit-length embedding. Empty vector when not ready or on failure.
    std::vector<float> embed(const std::string& text);

    // Cosine similarity (identical to the dot product for unit vectors).
    static float cosine(const std::vector<float>& a, const std::vector<float>& b);

    // Resolves a configured value (path, folder, or bare model name) to an
    // existing .onnx file, or "" when nothing suitable exists.
    static std::string resolve_model_file(const std::string& configured);

    // True when an embedding model was compiled into this executable.
    static bool has_bundled_model();

private:
    // Shared tail of both load paths: discover the I/O signature, resolve the
    // output tensor, read the embedding dimension, then self-test once.
    bool finish_initialization(const std::string& label);

    // Reads the RCDATA-embedded model + vocabulary. Returns false when this
    // executable was built without a bundled model.
    static bool read_bundled_model(std::vector<unsigned char>& model_bytes, std::string& vocab_text);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Keeps a bundled model buffer alive for as long as the session lives.
    std::vector<unsigned char> model_bytes_;

    bool ready_ = false;
    int dim_ = 0;
    int max_seq_len_ = 256;
    std::string model_path_;
};

}  // namespace Jarvis
