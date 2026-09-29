#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <thread>

#include "EmbeddingEngine.h"

namespace Jarvis {

struct DocumentChunk {
    std::string id;
    std::string text;
    std::string filename;
    std::string filepath;
    std::string collection;
    std::string timestamp;
    std::vector<float> embedding;  // unit-length, empty until embedded
};

struct UserFact {
    std::string id;
    std::string fact;
    std::string category;
    std::string timestamp;
};

/**
 * RAG Engine - Native C++ Retrieval Augmented Generation engine.
 * 
 * Provides:
 *   - Hybrid retrieval: BM25 lexical ranking combined with dense semantic
 *     similarity from a local ONNX embedding model (no network access)
 *   - Persistent document indexing
 *   - User knowledge (learning mode)
 *   - Conversation memory management
 *   - System status reporting
 *
 * Embeddings are optional: when no model is available the engine falls back to
 * BM25-only ranking and every other feature keeps working.
 */
class RagEngine {
public:
    RagEngine();
    ~RagEngine();

    // Initialize the RAG engine (loads persistent data from APPDATA)
    bool initialize(const std::string& python_path = "",
                    const std::string& script_path = "",
                    const std::string& embedding_model_path = "");

    // Check if RAG engine is ready
    bool isReady() const { return ready_; }
    bool isConnected() const { return connected_; }

    // === Semantic retrieval ===

    // True when dense vectors are available (model loaded and at least one
    // chunk embedded).
    bool has_semantic_search() const;
    // Number of chunks that currently carry a vector.
    int embedded_chunk_count() const;
    // Corpus size, for the vectors's bucketed scale metric.
    int document_chunk_count() const;
    int user_fact_count() const;
    // Human-readable embedding status for logs / the settings dialog.
    std::string embedding_status() const;

    // === Cross-node embeddings (vectors vector sharing) ===
    // Aggregated embedding vectors from peer nodes, received via the
    // vectors. These are used to expand the semantic search space
    // beyond the local corpus - a node with an empty corpus can still
    // retrieve relevant chunks from peers.
    //
    // Privacy: vectors are only ever received after client-side DP
    // protection (clipped + Gaussian noise) and Byzantine-robust
    // aggregation (trimmed mean). No raw vectors are ever stored or
    // forwarded.
    void set_cross_node_embeddings(const std::vector<std::vector<float>>& vectors);
    std::vector<std::vector<float>> get_cross_node_embeddings() const;
    bool has_cross_node_embeddings() const;

    // Local embedding vectors (for publication to the vectors).
    // These are the raw vectors from the local corpus - the vectors
    // applies DP protection before they leave the machine.
    std::vector<std::vector<float>> local_embedding_vectors() const;
    int embedding_dimension() const { return embedding_dim_; }

    // Term frequencies from the local corpus (for publication to the
    // vectors). Content-free: the encoder drops rare terms and
    // noises the counts before publication.
    std::map<std::string, int> term_frequencies() const;

    // === High-Level API (used by AssistantOrchestrator) ===

    // Query the RAG system: returns JSON string with retrieved contexts
    std::string query(const std::string& query_text, int top_k = 5,
                      const std::string& collection = "default");

    // Get context for LLM injection: formatted string of "source: content"
    std::string getContextForLLM(const std::string& query_text, int top_k = 5);

    // Upper bound for the context returned by getContextForLLM(). The default
    // (kDefaultMaxContextChars) is what the orchestrator used to hardcode, but
    // it is deliberately much larger: a 200-500 char budget throws away almost
    // every retrieval hit and is the reason RAG looked useless.
    static constexpr int kDefaultMaxContextChars = 1500;
    void set_max_context_chars(int chars);
    int get_max_context_chars() const { return max_context_chars_; }

    // Index a document file
    bool indexDocument(const std::string& filepath,
                       const std::string& collection = "default");

    // Index a directory of documents
    bool indexDirectory(const std::string& directory,
                        const std::string& collection = "default",
                        bool recursive = true);

    // Teach Sonny a fact (learning mode)
    bool teach(const std::string& fact, const std::string& category = "user_knowledge");

    // Delete facts whose 'fact' string starts with fact_prefix and match category.
    // Returns the number of facts deleted.
    int deleteUserFact(const std::string& fact_prefix, const std::string& category);

    // Remember all facts the user has taught
    std::string remember(const std::string& category = "user_knowledge");

    // Get user knowledge facts formatted for LLM context
    std::string getUserKnowledgeForLLM();

    // Summarize conversation history
    std::string summarizeConversation(const std::string& history);

    // Save conversation memory
    void saveConversationMemory(const std::string& session_id,
                                const std::string& summary,
                                const std::vector<std::string>& key_points = {});

    // Get RAG system health/status
    std::string health();

    // Get collection statistics
    std::string stats(const std::string& collection = "default");

    // Shutdown the RAG engine gracefully
    void shutdown();

private:
    void ensure_directories();
    void load_user_knowledge();
    void save_user_knowledge();
    void load_document_index();
    void save_document_index();
    
    std::string escape_json(const std::string& input) const;
    std::vector<std::string> tokenize(const std::string& text) const;
    double calculate_bm25_score(const std::vector<std::string>& query_terms, const std::string& chunk_text, const std::string& filename) const;

    // === Dense vectors (hybrid retrieval) ===
    // Vectors live in their own binary file (documents/embeddings.bin) rather
    // than inside index.json: one float32 record per chunk, in chunk order.
    // That keeps the JSONL index human-readable and makes persistence a plain
    // append instead of a full rewrite on every indexed document.
    std::vector<float> embed_text(const std::string& text);
    void load_embeddings();
    void append_embeddings(size_t from_index);
    void reset_embeddings();
    void start_embedding_worker();
    void stop_embedding_worker();
    void embedding_worker_loop();

    std::unique_ptr<EmbeddingEngine> embeddings_;
    std::string embeddings_file_;
    std::thread embedding_worker_;
    std::atomic<bool> embedding_worker_running_{false};
    std::atomic<int> embedded_chunks_{0};
    int embedding_dim_ = 0;
    float dense_weight_ = 6.0f;          // cosine contribution relative to BM25
    float dense_min_similarity_ = 0.2f;  // ignore weaker vector matches
    int max_context_chars_ = kDefaultMaxContextChars;

    std::atomic<bool> ready_{false};
    std::atomic<bool> connected_{false};
    mutable std::mutex rag_mutex_;

    std::string rag_data_dir_;
    std::string user_knowledge_dir_;
    std::string conversation_memory_dir_;
    std::string documents_dir_;

    std::vector<UserFact> user_facts_;
    std::vector<DocumentChunk> document_chunks_;

    // Cross-node embeddings received from the vectors. These are
    // aggregated peer vectors (DP-protected, Byzantine-robust) used to
    // expand the semantic search space beyond the local corpus.
    std::vector<std::vector<float>> cross_node_embeddings_;
    mutable std::mutex cross_node_mutex_;
};

} // namespace Jarvis
