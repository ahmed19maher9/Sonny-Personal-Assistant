#include "RagEngine.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <thread>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include "Logger.h"

namespace fs = std::filesystem;

namespace Jarvis {

RagEngine::RagEngine()
    : ready_(false)
    , connected_(false) {
}

RagEngine::~RagEngine() {
    shutdown();
}

void RagEngine::ensure_directories() {
    std::string appdata_path;
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        appdata_path = std::string(path);
    } else {
        appdata_path = ".";
    }
#else
    const char* home = getenv("HOME");
    appdata_path = home ? std::string(home) : ".";
#endif

    fs::path base_path = fs::path(appdata_path) / "Sonny" / "rag_data";
    rag_data_dir_ = base_path.string();
    user_knowledge_dir_ = (base_path / "user_knowledge").string();
    conversation_memory_dir_ = (base_path / "conversation_memory").string();
    documents_dir_ = (base_path / "documents").string();
    embeddings_file_ = (base_path / "documents" / "embeddings.bin").string();

    fs::create_directories(rag_data_dir_);
    fs::create_directories(user_knowledge_dir_);
    fs::create_directories(conversation_memory_dir_);
    fs::create_directories(documents_dir_);
}

std::string RagEngine::escape_json(const std::string& input) const {
    std::ostringstream ss;
    for (char c : input) {
        switch (c) {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    ss << "\\u" << std::hex << (int)c;
                } else {
                    ss << c;
                }
                break;
        }
    }
    return ss.str();
}

std::vector<std::string> RagEngine::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (current.length() > 1) {
                tokens.push_back(current);
            }
            current.clear();
        }
    }
    if (current.length() > 1) {
        tokens.push_back(current);
    }
    return tokens;
}

double RagEngine::calculate_bm25_score(const std::vector<std::string>& query_terms, const std::string& chunk_text, const std::string& filename) const {
    if (query_terms.empty() || chunk_text.empty()) return 0.0;
    
    std::vector<std::string> doc_tokens = tokenize(chunk_text);
    if (doc_tokens.empty()) return 0.0;

    double score = 0.0;
    std::string filename_lower = filename;
    std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

    for (const auto& term : query_terms) {
        int count = 0;
        for (const auto& tok : doc_tokens) {
            if (tok == term) count++;
        }
        if (count > 0) {
            // BM25 term frequency saturation
            double tf = (count * 2.5) / (count + 1.5);
            score += tf;
        }
        if (filename_lower.find(term) != std::string::npos) {
            score += 1.5; // Title bonus
        }
    }

    return score;
}

void RagEngine::load_user_knowledge() {
    user_facts_.clear();
    fs::path knowledge_file = fs::path(user_knowledge_dir_) / "user_knowledge.json";
    if (!fs::exists(knowledge_file)) return;

    std::ifstream file(knowledge_file);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t fact_pos = line.find("\"fact\":\"");
        if (fact_pos == std::string::npos) continue;

        fact_pos += 8;
        size_t fact_end = line.find("\"", fact_pos);
        if (fact_end == std::string::npos) continue;
        std::string fact = line.substr(fact_pos, fact_end - fact_pos);

        std::string category = "user_knowledge";
        size_t cat_pos = line.find("\"category\":\"");
        if (cat_pos != std::string::npos) {
            cat_pos += 12;
            size_t cat_end = line.find("\"", cat_pos);
            if (cat_end != std::string::npos) {
                category = line.substr(cat_pos, cat_end - cat_pos);
            }
        }

        std::string timestamp;
        size_t ts_pos = line.find("\"timestamp\":\"");
        if (ts_pos != std::string::npos) {
            ts_pos += 13;
            size_t ts_end = line.find("\"", ts_pos);
            if (ts_end != std::string::npos) {
                timestamp = line.substr(ts_pos, ts_end - ts_pos);
            }
        }

        UserFact uf;
        uf.id = "fact_" + std::to_string(user_facts_.size() + 1);
        uf.fact = fact;
        uf.category = category;
        uf.timestamp = timestamp;
        user_facts_.push_back(uf);
    }
}

void RagEngine::save_user_knowledge() {
    fs::path knowledge_file = fs::path(user_knowledge_dir_) / "user_knowledge.json";
    std::ofstream file(knowledge_file);
    if (!file.is_open()) return;

    for (const auto& uf : user_facts_) {
        file << "{\"id\":\"" << escape_json(uf.id)
             << "\",\"fact\":\"" << escape_json(uf.fact)
             << "\",\"category\":\"" << escape_json(uf.category)
             << "\",\"timestamp\":\"" << escape_json(uf.timestamp) << "\"}\n";
    }
}

void RagEngine::load_document_index() {
    document_chunks_.clear();
    fs::path index_file = fs::path(documents_dir_) / "index.json";
    if (!fs::exists(index_file)) return;

    std::ifstream file(index_file);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t text_pos = line.find("\"text\":\"");
        if (text_pos == std::string::npos) continue;

        text_pos += 8;
        size_t text_end = line.find("\"", text_pos);
        if (text_end == std::string::npos) continue;

        DocumentChunk chunk;
        chunk.text = line.substr(text_pos, text_end - text_pos);

        size_t file_pos = line.find("\"filename\":\"");
        if (file_pos != std::string::npos) {
            file_pos += 12;
            size_t file_end = line.find("\"", file_pos);
            if (file_end != std::string::npos) chunk.filename = line.substr(file_pos, file_end - file_pos);
        }

        size_t path_pos = line.find("\"filepath\":\"");
        if (path_pos != std::string::npos) {
            path_pos += 12;
            size_t path_end = line.find("\"", path_pos);
            if (path_end != std::string::npos) chunk.filepath = line.substr(path_pos, path_end - path_pos);
        }

        size_t col_pos = line.find("\"collection\":\"");
        if (col_pos != std::string::npos) {
            col_pos += 14;
            size_t col_end = line.find("\"", col_pos);
            if (col_end != std::string::npos) chunk.collection = line.substr(col_pos, col_end - col_pos);
        } else {
            chunk.collection = "default";
        }

        chunk.id = "chunk_" + std::to_string(document_chunks_.size() + 1);
        document_chunks_.push_back(chunk);
    }
}

void RagEngine::save_document_index() {
    fs::path index_file = fs::path(documents_dir_) / "index.json";
    std::ofstream file(index_file);
    if (!file.is_open()) return;

    for (const auto& chunk : document_chunks_) {
        file << "{\"id\":\"" << escape_json(chunk.id)
             << "\",\"text\":\"" << escape_json(chunk.text)
             << "\",\"filename\":\"" << escape_json(chunk.filename)
             << "\",\"filepath\":\"" << escape_json(chunk.filepath)
             << "\",\"collection\":\"" << escape_json(chunk.collection)
             << "\",\"timestamp\":\"" << escape_json(chunk.timestamp) << "\"}\n";
    }
}

bool RagEngine::initialize(const std::string& python_path, const std::string& script_path,
                           const std::string& embedding_model_path) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    LOG_RAG("Initializing Native C++ RAG Engine...");

    ensure_directories();
    load_user_knowledge();
    load_document_index();

    // Dense vectors are optional. When the model is unavailable the engine keeps
    // working with BM25 ranking alone - nothing here can fail the assistant.
    embeddings_ = std::make_unique<EmbeddingEngine>();
    const std::string model_key = embedding_model_path.empty() ? "all-MiniLM-L6-v2" : embedding_model_path;
    if (embeddings_->initialize(model_key, 256)) {
        embedding_dim_ = embeddings_->dimension();
        load_embeddings();
        start_embedding_worker();
    } else {
        embeddings_.reset();
        embedding_dim_ = 0;
        LOG_RAG("Semantic search unavailable - using BM25 ranking only");
    }

    ready_ = true;
    connected_ = true;

    LOG_RAG("Native C++ RAG Engine initialized successfully!");
    LOG_RAG_DEBUG("Loaded " + std::to_string(user_facts_.size()) + " user facts and " +
                  std::to_string(document_chunks_.size()) + " document chunks.");

    return true;
}

std::string RagEngine::query(const std::string& query_text, int top_k,
                              const std::string& collection) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    if (!ready_ || query_text.empty()) {
        return "{\"documents\":[],\"metadatas\":[],\"distances\":[],\"total_available\":0}";
    }

    std::vector<std::string> query_terms = tokenize(query_text);
    if (query_terms.empty()) {
        return "{\"documents\":[],\"metadatas\":[],\"distances\":[],\"total_available\":0}";
    }

    struct ScoredChunk {
        double score;
        const DocumentChunk* chunk;
    };

    // Query vector for the dense half of the hybrid score. Empty when no model
    // is loaded, in which case the loop below degrades to pure BM25.
    std::vector<float> query_vector;
    if (embeddings_ && embeddings_->is_ready() && embedding_dim_ > 0) {
        query_vector = embed_text(query_text);
    }

    // Cross-node collective relevance signal. The vectors aggregates
    // peer embedding vectors (DP-protected, Byzantine-robust). We don't
    // receive peer text - only the semantic directions that are trending
    // across the network. The most similar cross-node vector tells us
    // "this topic is important to peers", and we boost local chunks that
    // share that semantic direction.
    //
    // This is the cold-start killer: a node with an empty corpus can still
    // surface locally-relevant chunks by following the collective signal.
    std::vector<float> collective_vector;
    {
        std::lock_guard<std::mutex> lock(cross_node_mutex_);
        if (!cross_node_embeddings_.empty() && !query_vector.empty()) {
            double best_sim = -1.0;
            for (const auto& cv : cross_node_embeddings_) {
                if (cv.size() != query_vector.size()) continue;
                const float sim = EmbeddingEngine::cosine(query_vector, cv);
                if (sim > best_sim) {
                    best_sim = sim;
                    collective_vector = cv;
                }
            }
            // Only use the collective signal when it's actually similar
            // to the query - a random cross-node vector would hurt, not
            // help, retrieval quality.
            if (best_sim < 0.3f) collective_vector.clear();
        }
    }

    std::vector<ScoredChunk> scored_list;
    for (const auto& chunk : document_chunks_) {
        if (!collection.empty() && collection != "default" && chunk.collection != collection) {
            continue;
        }

        double score = calculate_bm25_score(query_terms, chunk.text, chunk.filename);

        // Semantic similarity: this is what finds a passage that shares no
        // keywords with the question ("my laptop is slow" -> "thermal
        // throttling on the CPU").
        if (!query_vector.empty() && chunk.embedding.size() == query_vector.size()) {
            const float similarity = EmbeddingEngine::cosine(query_vector, chunk.embedding);
            if (similarity > dense_min_similarity_) {
                score += static_cast<double>(similarity) * dense_weight_;
            }
        }

        // Collective relevance boost: if the cross-node signal says this
        // topic is trending, boost local chunks that share that semantic
        // direction. This is weighted lower than direct query similarity
        // because the collective vector is aggregated and noised.
        if (!collective_vector.empty() && chunk.embedding.size() == collective_vector.size()) {
            const float collective_sim = EmbeddingEngine::cosine(collective_vector, chunk.embedding);
            if (collective_sim > 0.5f) {
                score += static_cast<double>(collective_sim) * dense_weight_ * 0.3;
            }
        }

        if (score > 0.1) {
            scored_list.push_back({score, &chunk});
        }
    }

    std::sort(scored_list.begin(), scored_list.end(), [](const ScoredChunk& a, const ScoredChunk& b) {
        return a.score > b.score;
    });

    std::ostringstream json;
    json << "{\"documents\":[";
    size_t count = std::min(static_cast<size_t>(top_k), scored_list.size());
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) json << ",";
        json << "\"" << escape_json(scored_list[i].chunk->text) << "\"";
    }
    json << "],\"metadatas\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) json << ",";
        json << "{\"filename\":\"" << escape_json(scored_list[i].chunk->filename)
             << "\",\"filepath\":\"" << escape_json(scored_list[i].chunk->filepath) << "\"}";
    }
    json << "],\"distances\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) json << ",";
        json << (1.0 / (1.0 + scored_list[i].score));
    }
    json << "],\"total_available\":" << document_chunks_.size();

    // Expose the collective signal so the orchestrator can log it.
    if (!collective_vector.empty()) {
        json << ",\"collective_signal\":true";
    }

    json << "}";

    return json.str();
}

std::string RagEngine::getContextForLLM(const std::string& query_text, int top_k) {
    std::string q_res = query(query_text, top_k);
    std::string context;

    size_t docs_start = q_res.find("\"documents\":[");
    if (docs_start != std::string::npos) {
        docs_start += 13;
        size_t docs_end = q_res.find("],\"metadatas\"", docs_start);
        if (docs_end != std::string::npos) {
            std::string docs_str = q_res.substr(docs_start, docs_end - docs_start);
            size_t doc_pos = 0;
            int count = 0;
            while ((doc_pos = docs_str.find("\"", doc_pos)) != std::string::npos) {
                doc_pos++;
                size_t doc_end = docs_str.find("\"", doc_pos);
                if (doc_end == std::string::npos) break;

                std::string doc_text = docs_str.substr(doc_pos, doc_end - doc_pos);
                if (!doc_text.empty()) {
                    // Respect the context budget: an oversized block is worse
                    // than no block, because the orchestrator used to drop the
                    // whole thing when it exceeded the limit.
                    const size_t overhead = 10;  // "[n] " + "\n\n"
                    if (context.size() + doc_text.size() + overhead > (size_t)max_context_chars_) {
                        break;
                    }
                    context += "[" + std::to_string(++count) + "] " + doc_text + "\n\n";
                }
                doc_pos = doc_end + 1;
            }
        }
    }

    std::string user_knowledge = getUserKnowledgeForLLM();
    if (!user_knowledge.empty() && context.size() < (size_t)max_context_chars_) {
        const size_t remaining = (size_t)max_context_chars_ - context.size();
        if (user_knowledge.size() > remaining) {
            user_knowledge.resize(remaining);
        }
        context += "\n--- User Knowledge ---\n" + user_knowledge + "\n";
    }

    return context;
}

bool RagEngine::indexDocument(const std::string& filepath, const std::string& collection) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    if (!fs::exists(filepath)) return false;

    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    if (content.empty()) return true;

    fs::path p(filepath);
    std::string filename = p.filename().string();

    const size_t chunk_size = 512;
    const size_t overlap = 64;
    const size_t first_new_index = document_chunks_.size();

    size_t start = 0;
    while (start < content.length()) {
        size_t end = std::min(start + chunk_size, content.length());
        std::string chunk_text = content.substr(start, end - start);

        DocumentChunk chunk;
        chunk.id = "chunk_" + std::to_string(document_chunks_.size() + 1);
        chunk.text = chunk_text;
        chunk.filename = filename;
        chunk.filepath = filepath;
        chunk.collection = collection;

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        chunk.timestamp = std::to_string(in_time_t);

        document_chunks_.push_back(chunk);

        if (end == content.length()) break;
        start += (chunk_size - overlap);
    }

    save_document_index();

    // Embed the fresh chunks right away so a newly indexed document is
    // searchable immediately. This is only valid while the new chunks continue
    // the embedded prefix (the binary vector file is append-ordered); a backlog
    // left over from a previous run is picked up by the background worker.
    if (embeddings_ && embeddings_->is_ready() && embedding_dim_ > 0 &&
        document_chunks_.size() > first_new_index) {
        if ((size_t)embedded_chunks_.load() == first_new_index) {
            for (size_t i = first_new_index; i < document_chunks_.size(); ++i) {
                std::vector<float> vector = embed_text(document_chunks_[i].text);
                if ((int)vector.size() != embedding_dim_) break;
                document_chunks_[i].embedding = std::move(vector);
                embedded_chunks_ = (int)(i + 1);
            }
            append_embeddings(first_new_index);
        }
        start_embedding_worker();
    }

    LOG_RAG("Indexed document: " + filename);
    return true;
}

bool RagEngine::indexDirectory(const std::string& directory, const std::string& collection, bool recursive) {
    if (!fs::exists(directory) || !fs::is_directory(directory)) return false;

    static const std::set<std::string> valid_exts = {
        ".txt", ".md", ".json", ".cpp", ".h", ".c", ".hpp", ".py", ".js", ".ts", ".html", ".htm"
    };

    bool any_indexed = false;
    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file() && valid_exts.count(entry.path().extension().string()) > 0) {
                if (indexDocument(entry.path().string(), collection)) {
                    any_indexed = true;
                }
            }
        }
    } else {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file() && valid_exts.count(entry.path().extension().string()) > 0) {
                if (indexDocument(entry.path().string(), collection)) {
                    any_indexed = true;
                }
            }
        }
    }

    return any_indexed;
}

bool RagEngine::teach(const std::string& fact, const std::string& category) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    if (fact.empty()) return false;

    UserFact uf;
    uf.id = "fact_" + std::to_string(user_facts_.size() + 1);
    uf.fact = fact;
    uf.category = category;

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    uf.timestamp = std::to_string(in_time_t);

    user_facts_.push_back(uf);
    save_user_knowledge();

    LOG_RAG("Sonny learned new fact: " + fact);
    return true;
}

int RagEngine::deleteUserFact(const std::string& fact_prefix, const std::string& category) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    if (fact_prefix.empty()) return 0;

    std::string prefix_lower = fact_prefix;
    std::transform(prefix_lower.begin(), prefix_lower.end(), prefix_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    int deleted = 0;
    user_facts_.erase(
        std::remove_if(user_facts_.begin(), user_facts_.end(),
            [&](const UserFact& uf) {
                if (!category.empty() && uf.category != category) return false;
                std::string fact_lower = uf.fact;
                std::transform(fact_lower.begin(), fact_lower.end(), fact_lower.begin(),
                               [](unsigned char c) { return static_cast<char>(::tolower(c)); });
                bool match = (fact_lower.find(prefix_lower) != std::string::npos);
                if (match) deleted++;
                return match;
            }),
        user_facts_.end());

    if (deleted > 0) {
        save_user_knowledge();
        LOG_RAG("Deleted " + std::to_string(deleted) + " fact(s) matching: " + fact_prefix);
    }
    return deleted;
}

std::string RagEngine::remember(const std::string& category) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    std::ostringstream json;
    json << "{\"success\":true,\"facts\":[";

    bool first = true;
    int count = 0;
    for (const auto& uf : user_facts_) {
        if (!category.empty() && uf.category != category) continue;
        if (!first) json << ",";
        json << "{\"fact\":\"" << escape_json(uf.fact)
             << "\",\"category\":\"" << escape_json(uf.category)
             << "\",\"timestamp\":\"" << escape_json(uf.timestamp)
             << "\",\"id\":\"" << escape_json(uf.id) << "\"}";
        first = false;
        count++;
    }
    json << "],\"count\":" << count << "}";
    return json.str();
}

std::string RagEngine::getUserKnowledgeForLLM() {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    std::string facts_str;
    for (const auto& uf : user_facts_) {
        facts_str += "- " + uf.fact + "\n";
    }
    return facts_str;
}

std::string RagEngine::summarizeConversation(const std::string& history) {
    if (history.empty()) return "";

    std::vector<std::string> lines;
    std::stringstream ss(history);
    std::string item;
    while (std::getline(ss, item, '\n')) {
        if (!item.empty()) lines.push_back(item);
    }

    if (lines.empty()) return "";

    std::string summary = "Conversation Summary:\n";
    size_t start = lines.size() > 6 ? lines.size() - 6 : 0;
    for (size_t i = start; i < lines.size(); ++i) {
        summary += lines[i] + "\n";
    }

    if (summary.length() > 500) {
        summary = summary.substr(0, 497) + "...";
    }
    return summary;
}

void RagEngine::saveConversationMemory(const std::string& session_id,
                                       const std::string& summary,
                                       const std::vector<std::string>& key_points) {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    fs::path mem_file = fs::path(conversation_memory_dir_) / (session_id + ".json");
    std::ofstream file(mem_file);
    if (!file.is_open()) return;

    file << "{\"session_id\":\"" << escape_json(session_id)
         << "\",\"summary\":\"" << escape_json(summary) << "\"}\n";

    LOG_RAG("Saved conversation memory for session: " + session_id);
}

std::string RagEngine::health() {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    std::ostringstream json;
    json << "{\"status\":\"ok\",\"engine\":\"C++ Native RAG Engine\",\"user_facts_count\":"
         << user_facts_.size() << ",\"document_chunks_count\":" << document_chunks_.size()
         << ",\"embedded_chunks\":" << embedded_chunks_.load()
         << ",\"embedding_dim\":" << embedding_dim_
         << ",\"max_context_chars\":" << max_context_chars_ << "}";
    return json.str();
}

std::string RagEngine::stats(const std::string& collection) {
    return health();
}

void RagEngine::shutdown() {
    stop_embedding_worker();
    ready_ = false;
    connected_ = false;
    if (embeddings_) {
        embeddings_->shutdown();
    }
    LOG_RAG("Native C++ RAG Engine shut down");
}

// ============================================================================
// Dense vector support (hybrid retrieval)
// ============================================================================

bool RagEngine::has_semantic_search() const {
    return embeddings_ && embeddings_->is_ready() && embedded_chunks_.load() > 0;
}

int RagEngine::embedded_chunk_count() const {
    return embedded_chunks_.load();
}

int RagEngine::document_chunk_count() const {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    return static_cast<int>(document_chunks_.size());
}

int RagEngine::user_fact_count() const {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    return static_cast<int>(user_facts_.size());
}

std::string RagEngine::embedding_status() const {
    std::lock_guard<std::mutex> lock(rag_mutex_);
    if (!embeddings_ || !embeddings_->is_ready()) {
        return "disabled (no embedding model)";
    }
    return std::to_string(embedded_chunks_.load()) + "/" +
           std::to_string(document_chunks_.size()) + " chunks embedded, " +
           std::to_string(embedding_dim_) + " dimensions";
}

void RagEngine::set_max_context_chars(int chars) {
    max_context_chars_ = chars < 200 ? 200 : chars;
}

std::vector<float> RagEngine::embed_text(const std::string& text) {
    if (!embeddings_ || !embeddings_->is_ready() || text.empty()) return std::vector<float>();
    return embeddings_->embed(text);
}

void RagEngine::load_embeddings() {
    embedded_chunks_ = 0;
    for (DocumentChunk& chunk : document_chunks_) chunk.embedding.clear();

    if (!embeddings_ || !embeddings_->is_ready() || embedding_dim_ <= 0) return;
    if (embeddings_file_.empty()) return;

    std::ifstream file(embeddings_file_, std::ios::binary);
    if (!file.is_open()) return;

    const size_t record_bytes = (size_t)embedding_dim_ * sizeof(float);
    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    if (file_size <= 0 || (file_size % (std::streamoff)record_bytes) != 0) {
        LOG_RAG_DEBUG("Vector file does not match the current model - vectors will be rebuilt");
        return;
    }

    const size_t records = (size_t)(file_size / (std::streamoff)record_bytes);
    const size_t usable = std::min(records, document_chunks_.size());
    file.seekg(0, std::ios::beg);

    int loaded = 0;
    for (size_t i = 0; i < usable; ++i) {
        std::vector<float> vector((size_t)embedding_dim_);
        file.read(reinterpret_cast<char*>(vector.data()), (std::streamsize)record_bytes);
        if (!file) break;
        document_chunks_[i].embedding = std::move(vector);
        ++loaded;
    }
    embedded_chunks_ = loaded;

    if (loaded > 0) {
        LOG_RAG("Loaded " + std::to_string(loaded) + " document vectors (" +
                std::to_string(embedding_dim_) + "-d)");
    }
}

void RagEngine::append_embeddings(size_t from_index) {
    if (embeddings_file_.empty() || embedding_dim_ <= 0) return;
    if (from_index >= document_chunks_.size()) return;

    std::ofstream file(embeddings_file_, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        LOG_WARN("RAG", "Cannot append to the vector file: " + embeddings_file_);
        return;
    }

    // Records are written in chunk order and must never skip: a gap would
    // silently shift every later vector when the file is read back.
    for (size_t i = from_index; i < document_chunks_.size(); ++i) {
        const std::vector<float>& vector = document_chunks_[i].embedding;
        if ((int)vector.size() != embedding_dim_) return;
        file.write(reinterpret_cast<const char*>(vector.data()),
                   (std::streamsize)(vector.size() * sizeof(float)));
    }
}

void RagEngine::reset_embeddings() {
    for (DocumentChunk& chunk : document_chunks_) chunk.embedding.clear();
    embedded_chunks_ = 0;
    if (embeddings_file_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(embeddings_file_, ec);
}

void RagEngine::start_embedding_worker() {
    if (!embeddings_ || !embeddings_->is_ready() || embedding_dim_ <= 0) return;
    if (embedding_worker_.joinable()) return;  // one worker at a time
    embedding_worker_running_ = true;
    embedding_worker_ = std::thread(&RagEngine::embedding_worker_loop, this);
}

void RagEngine::stop_embedding_worker() {
    embedding_worker_running_ = false;
    if (embedding_worker_.joinable()) {
        embedding_worker_.join();
    }
}

// Fills in missing vectors in the background, oldest chunk first. Index order
// is not cosmetic: the vector file is append-only, so record N must always
// belong to chunk N.
void RagEngine::embedding_worker_loop() {
    constexpr size_t kBatchSize = 32;

    LOG_RAG("Embedding pass started (" +
            std::to_string((int)document_chunks_.size() - embedded_chunks_.load()) +
            " chunks pending)");

    while (embedding_worker_running_) {
        size_t start_index = 0;
        {
            std::lock_guard<std::mutex> lock(rag_mutex_);
            while (start_index < document_chunks_.size() &&
                   !document_chunks_[start_index].embedding.empty()) {
                ++start_index;
            }
            if (start_index >= document_chunks_.size()) break;  // caught up
            embedded_chunks_ = (int)start_index;
        }

        std::vector<size_t> indices;
        std::vector<std::string> texts;
        {
            std::lock_guard<std::mutex> lock(rag_mutex_);
            for (size_t i = start_index; i < document_chunks_.size() && texts.size() < kBatchSize;
                 ++i) {
                if (!document_chunks_[i].embedding.empty()) break;
                indices.push_back(i);
                texts.push_back(document_chunks_[i].text);
            }
        }
        if (texts.empty()) break;

        // Inference happens without the lock: a 20k-chunk corpus must not make
        // voice queries wait.
        std::vector<std::vector<float>> vectors;
        vectors.reserve(texts.size());
        for (const std::string& text : texts) {
            if (!embedding_worker_running_) return;
            vectors.push_back(embed_text(text));
        }

        bool stored_any = false;
        {
            std::lock_guard<std::mutex> lock(rag_mutex_);
            for (size_t i = 0; i < indices.size() && i < vectors.size(); ++i) {
                if ((int)vectors[i].size() != embedding_dim_) continue;
                document_chunks_[indices[i]].embedding = std::move(vectors[i]);
                stored_any = true;
            }
            if (stored_any) {
                append_embeddings(start_index);
                int next = (int)start_index;
                while (next < (int)document_chunks_.size() &&
                       !document_chunks_[next].embedding.empty()) {
                    ++next;
                }
                embedded_chunks_ = next;
            }
        }

        if (!stored_any) {
            LOG_WARN("RAG", "Embedding pass stopped: the model returned no usable vectors");
            break;
        }
    }

    LOG_RAG("Embedding pass finished: " + embedding_status());
    embedding_worker_running_ = false;
}

void RagEngine::set_cross_node_embeddings(const std::vector<std::vector<float>>& vectors) {
    std::lock_guard<std::mutex> lock(cross_node_mutex_);
    cross_node_embeddings_ = vectors;
}

std::vector<std::vector<float>> RagEngine::get_cross_node_embeddings() const {
    std::lock_guard<std::mutex> lock(cross_node_mutex_);
    return cross_node_embeddings_;
}

bool RagEngine::has_cross_node_embeddings() const {
    std::lock_guard<std::mutex> lock(cross_node_mutex_);
    return !cross_node_embeddings_.empty();
}

std::vector<std::vector<float>> RagEngine::local_embedding_vectors() const {
    std::vector<std::vector<float>> vectors;
    for (const DocumentChunk& chunk : document_chunks_) {
        if (!chunk.embedding.empty()) {
            vectors.push_back(chunk.embedding);
        }
    }
    return vectors;
}

std::map<std::string, int> RagEngine::term_frequencies() const {
    std::map<std::string, int> frequencies;
    std::set<std::string> seen_terms;

    // Collect unique terms from all document chunks.
    for (const DocumentChunk& chunk : document_chunks_) {
        const std::vector<std::string> terms = tokenize(chunk.text);
        for (const std::string& term : terms) {
            if (term.size() > 48) continue;
            if (seen_terms.insert(term).second) {
                frequencies[term] = 1;
            } else {
                frequencies[term]++;
            }
        }
    }
    return frequencies;
}

} // namespace Jarvis
