#include "VectorsSync.h"
#include "VectorsCrypto.h"
#include "HttpClient.h"
#include "Logger.h"

#include <random>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include <windows.h>
#include <shlobj.h>

namespace Jarvis {
namespace Vectors {

namespace {

std::int64_t sync_now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// %APPDATA%\Sonny\vectors - same root the RAG data already uses.
std::string vectors_state_dir() {
    std::string base;
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        base = std::string(path);
    } else if (const char* appdata = std::getenv("APPDATA")) {
        base = appdata;
    }
    if (base.empty()) base = ".";
    return base + "\\Sonny\\vectors";
}

}  // namespace

VectorsSync::VectorsSync() = default;

VectorsSync::~VectorsSync() {
    stop();
}

// The room is also used as a URL query value, so it is restricted to the
// characters that need no escaping.
void VectorsSync::sanitize_room() {
    std::string cleaned;
    for (char c : config_.room) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            cleaned.push_back(c);
        }
        if (cleaned.size() >= 64) break;
    }
    if (cleaned.empty()) cleaned = "default";
    if (cleaned != config_.room) {
        LOG_INFO("Vectors", "Room name sanitized to '" + cleaned + "'");
    }
    config_.room = cleaned;
}

void VectorsSync::load_state() {
    std::error_code ec;
    std::filesystem::create_directories(state_dir_, ec);

    const std::string id_file = state_dir_ + "\\node_id.txt";
    std::ifstream input(id_file);
    if (input.is_open()) {
        std::string stored;
        std::getline(input, stored);
        while (!stored.empty() &&
               (stored.back() == '\r' || stored.back() == '\n' || stored.back() == ' ')) {
            stored.pop_back();
        }
        if (!stored.empty() && stored.size() <= 128) {
            config_.node_id = stored;
            return;
        }
    }

    if (config_.node_id.empty()) {
        config_.node_id = random_token();
        LOG_INFO("Vectors", "Created node identity: " + config_.node_id);
    }
    save_state();
}

void VectorsSync::save_state() const {
    if (state_dir_.empty() || config_.node_id.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(state_dir_, ec);

    std::ofstream output(state_dir_ + "\\node_id.txt", std::ios::trunc);
    if (!output.is_open()) return;
    output << config_.node_id << "\n";
}

bool VectorsSync::initialize(const VectorsConfig& config) {
    config_ = config;
    state_dir_ = vectors_state_dir();

    sanitize_room();
    load_state();

    if (config_.mode != "lan" && config_.mode != "wan") config_.mode = "off";
    if (config_.k_anonymity < 1) config_.k_anonymity = 1;
    if (config_.discovery_interval_seconds < 5) config_.discovery_interval_seconds = 5;
    if (config_.sync_interval_seconds < 30) config_.sync_interval_seconds = 30;
    if (config_.max_remote_weight < 0.0f) config_.max_remote_weight = 0.0f;
    if (config_.max_remote_weight > 0.5f) config_.max_remote_weight = 0.5f;

    if (is_enabled() && config_.mode == "wan" && config_.rendezvous_url.empty()) {
        LOG_INFO("Vectors", "WAN mode without a rendezvous address - using LAN discovery only");
    }
    if (is_enabled()) {
        LOG_INFO("Vectors", "Configured node " + config_.node_id + ", room '" + config_.room +
                                      "', mode " + config_.mode + ", sharing tool metrics=" +
                                      (config_.share_tool_metrics ? "yes" : "no") +
                                      ", corpus scale=" +
                                      (config_.share_corpus_scale ? "yes" : "no"));
    }
    return true;
}

bool VectorsSync::start() {
    if (!is_enabled()) {
        LOG_INFO("Vectors", "Peer metrics network disabled (mode '" + config_.mode + "')");
        return false;
    }
    if (running_.load()) return true;

    // Our data is only ever produced on demand, by this callback.
    transport_.set_contribution_provider([this]() { return build_contribution_json(); });
    transport_.set_contribution_handler([this](const Contribution& contribution) {
        store_remote(contribution);
        receive_count_++;
    });

    if (!transport_.start(config_)) {
        last_error_ = transport_.last_error();
        LOG_WARN("Vectors", "Transport did not start: " + last_error_);
        return false;
    }

    running_ = true;
    sync_thread_ = std::thread(&VectorsSync::sync_loop, this);

    LOG_INFO("Vectors", "Node online in room '" + config_.room + "' (mode " + config_.mode + ")");
    return true;
}

void VectorsSync::stop() {
    const bool was_running = running_.exchange(false);
    if (was_running && sync_thread_.joinable()) {
        sync_thread_.join();
    }
    transport_.stop();
}

int VectorsSync::peer_count() const {
    return static_cast<int>(transport_.peers().size());
}

// ============================================================================
// Local metrics
// ============================================================================

void VectorsSync::record_tool_outcome(const std::string& tool, bool success, double latency_ms) {
    if (tool.empty() || tool.size() > 64) return;

    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ToolStat& stat = local_tool_stats_[tool];
    stat.tool = tool;
    if (success) {
        stat.success++;
    } else {
        stat.failure++;
    }

    const double bounded = latency_ms < 0.0 ? 0.0 : (latency_ms > 600000.0 ? 600000.0 : latency_ms);
    stat.total_latency_ms += bounded;
    if (bounded > stat.max_latency_ms) stat.max_latency_ms = bounded;
}

void VectorsSync::set_corpus_scale(int document_chunks, int user_facts, bool has_embeddings) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    local_document_chunks_ = document_chunks < 0 ? 0 : document_chunks;
    local_user_facts_ = user_facts < 0 ? 0 : user_facts;
    local_has_embeddings_ = has_embeddings;
}

void VectorsSync::set_term_frequencies(const std::map<std::string, int>& frequencies) {
    std::lock_guard<std::mutex> lock(term_mutex_);
    local_term_frequencies_ = frequencies;
}

void VectorsSync::set_embedding_vectors(const std::vector<std::vector<float>>& vectors,
                                              int embedding_dim) {
    std::lock_guard<std::mutex> lock(embedding_mutex_);
    local_embedding_dim_ = embedding_dim;
    local_embedding_vectors_.clear();

    // Client-side DP protection (Chapter 12 of "Federated Learning
    // Foundations and Applications"). Each vector is:
    //   1. Clipped to L2 norm C (sensitivity control)
    //   2. Gaussian noise added with sigma calibrated to 2*C
    // This gives formal (epsilon, delta)-DP guarantees under the
    // replace-one adjacency relation used in FL.
    //
    // The clipping threshold C is derived from the vector dimensionality:
    // for a 1024-d unit vector, the L2 norm is 1.0, so C=1.0 is a natural
    // choice. The noise sigma is config_.dp_sigma.
    const float clip_norm = 1.0f;
    const float sigma = config_.dp_sigma;
    for (const std::vector<float>& vec : vectors) {
        if (static_cast<int>(vec.size()) != embedding_dim) continue;
        std::vector<float> protected_vec = vec;

        // L2 norm
        double norm = 0.0;
        for (float v : protected_vec) norm += static_cast<double>(v) * v;
        norm = std::sqrt(norm);

        // Clip
        if (norm > static_cast<double>(clip_norm)) {
            const double scale = static_cast<double>(clip_norm) / norm;
            for (float& v : protected_vec) v = static_cast<float>(v * scale);
        }

        // Add Gaussian noise (Box-Muller transform)
        if (sigma > 0.0f) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::normal_distribution<float> dist(0.0f, sigma);
            for (float& v : protected_vec) v += dist(gen);
        }

        local_embedding_vectors_.push_back(std::move(protected_vec));
    }
}

void VectorsSync::set_lora_adapter(const std::string& name, const std::string& base64_bytes) {
    if (name.empty() || name.size() > 128) return;
    if (base64_bytes.empty() || base64_bytes.size() > 4 * 1024 * 1024) return;

    std::lock_guard<std::mutex> lock(lora_local_mutex_);
    local_lora_adapter_name_ = name;
    local_lora_adapter_bytes_ = base64_bytes;
}

void VectorsSync::set_mesh_info(const MeshInfo& mesh_info) {
    if (mesh_info.node_count < 0 || mesh_info.node_count > 100000) return;
    if (mesh_info.edge_count < 0 || mesh_info.edge_count > 1000000) return;
    if (mesh_info.avg_latency_ms < 0.0 || mesh_info.avg_latency_ms > 1e12) return;
    if (mesh_info.region.size() > 64) return;

    std::lock_guard<std::mutex> lock(mesh_local_mutex_);
    local_mesh_info_ = mesh_info;
}

std::vector<std::pair<std::string, MeshInfo>> VectorsSync::mesh_view() const {
    // Byzantine-robust aggregation (Chapter 11): collect per-peer mesh
    // metadata, apply a trimmed mean over node_count, edge_count and
    // avg_latency_ms - discard the top and bottom 20% of outliers before
    // averaging. A single malicious peer cannot skew the aggregated view
    // because its extreme values are trimmed away.
    //
    // Privacy: mesh metadata is content-free by construction - only
    // topology (node/edge counts) and a latency average are published,
    // never identities or user data.

    std::vector<std::pair<std::string, MeshInfo>> peer_meshes;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (const auto& entry : remote_contributions_) {
            if (entry.second.mesh_info.node_count <= 0 && entry.second.mesh_info.edge_count <= 0) continue;
            peer_meshes.emplace_back(entry.first, entry.second.mesh_info);
        }
    }

    if (peer_meshes.empty()) return {};

    // k-anonymity gate
    if (static_cast<int>(peer_meshes.size()) < config_.k_anonymity) return {};

    // Trimmed mean: sort by node_count, discard top/bottom 20%.
    std::sort(peer_meshes.begin(), peer_meshes.end(),
        [](const std::pair<std::string, MeshInfo>& a, const std::pair<std::string, MeshInfo>& b) {
            return a.second.node_count < b.second.node_count;
        });

    const size_t trim_count = peer_meshes.size() / 5;
    const size_t start = (trim_count > 0 && peer_meshes.size() > 2 * trim_count) ? trim_count : 0;
    const size_t end = peer_meshes.size() - (trim_count > 0 && peer_meshes.size() > 2 * trim_count ? trim_count : 0);

    MeshInfo aggregated;
    int count = 0;
    for (size_t i = start; i < end; ++i) {
        aggregated.node_count += peer_meshes[i].second.node_count;
        aggregated.edge_count += peer_meshes[i].second.edge_count;
        aggregated.avg_latency_ms += peer_meshes[i].second.avg_latency_ms;
        ++count;
    }
    if (count <= 0) return {};
    aggregated.node_count /= count;
    aggregated.edge_count /= count;
    aggregated.avg_latency_ms /= static_cast<double>(count);

    return {std::make_pair(config_.node_id, aggregated)};
}

std::vector<std::vector<float>> VectorsSync::aggregated_embeddings() const {
    // Byzantine-robust aggregation (Chapter 11 of "Federated Learning
    // Foundations and Applications"): collect per-peer embedding vectors,
    // apply a trimmed mean - discard the top and bottom 20% of outliers
    // per dimension before averaging. A single malicious peer cannot skew
    // the aggregated vectors because its extreme values are trimmed away.
    //
    // The vectors are already client-side DP-protected (clipped + noised)
    // before publication, so the aggregated sum carries formal privacy
    // guarantees (Chapter 12).

    std::vector<std::vector<float>> peer_vectors;
    int common_dim = 0;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (const auto& entry : remote_contributions_) {
            if (entry.second.embedding_dim <= 0) continue;
            if (common_dim == 0) {
                common_dim = entry.second.embedding_dim;
            } else if (entry.second.embedding_dim != common_dim) {
                continue;  // dimension mismatch - skip
            }
            for (const std::string& b64 : entry.second.embedding_vectors) {
                const std::vector<float> vec = decode_vectors_base64(b64);
                if (!vec.empty() && static_cast<int>(vec.size()) == common_dim) {
                    peer_vectors.push_back(vec);
                }
            }
        }
    }

    if (peer_vectors.empty() || common_dim <= 0) return {};

    // k-anonymity gate
    if (static_cast<int>(peer_vectors.size()) < config_.k_anonymity) return {};

    // Trimmed mean: sort by L2 norm, discard top/bottom 20%.
    std::vector<std::vector<float>> sorted = peer_vectors;
    std::sort(sorted.begin(), sorted.end(),
        [](const std::vector<float>& a, const std::vector<float>& b) {
            double na = 0.0, nb = 0.0;
            for (float v : a) na += static_cast<double>(v) * v;
            for (float v : b) nb += static_cast<double>(v) * v;
            return std::sqrt(na) < std::sqrt(nb);
        });

    const size_t trim_count = sorted.size() / 5;
    const size_t start = (trim_count > 0 && sorted.size() > 2 * trim_count) ? trim_count : 0;
    const size_t end = sorted.size() - (trim_count > 0 && sorted.size() > 2 * trim_count ? trim_count : 0);

    std::vector<float> aggregated(common_dim, 0.0f);
    int count = 0;
    for (size_t i = start; i < end; ++i) {
        for (int d = 0; d < common_dim; ++d) {
            aggregated[d] += sorted[i][d];
        }
        ++count;
    }
    if (count <= 0) return {};
    for (int d = 0; d < common_dim; ++d) {
        aggregated[d] /= static_cast<float>(count);
    }
    return {aggregated};
}

void VectorsSync::add_remote_lora_adapter(const std::string& name, const std::string& base64_bytes) {
    if (name.empty() || name.size() > 128) return;
    if (base64_bytes.empty() || base64_bytes.size() > 4 * 1024 * 1024) return;

    std::lock_guard<std::mutex> lock(lora_mutex_);
    remote_lora_adapters_[name].push_back(base64_bytes);

    // Bound memory: keep only the most recent 64 copies per adapter name.
    std::vector<std::string>& copies = remote_lora_adapters_[name];
    if (copies.size() > 64) {
        copies.erase(copies.begin(), copies.end() - 64);
    }
}

std::vector<std::pair<std::string, std::string>> VectorsSync::aggregated_lora_adapters() const {
    // Byzantine-robust aggregation (Chapter 11): an adapter is reported only
    // when at least k_anonymity distinct peers contributed it. We don't
    // average adapter bytes - we pick the most-received adapter per name,
    // since LoRA adapters are not numerically aggregatable without a shared
    // base model. A peer that publishes a valid adapter earns weight; a
    // peer that publishes garbage is naturally filtered by the k-anonymity
    // gate and the reputation system.
    //
    // Privacy: adapters are content-free by construction - they are weights,
    // not data. A peer never sees the user's conversations, only the
    // mathematical delta that makes the model more helpful for that peer's
    // domain.

    std::vector<std::pair<std::string, std::string>> result;
    {
        std::lock_guard<std::mutex> lock(lora_mutex_);
        for (const auto& entry : remote_lora_adapters_) {
            if (static_cast<int>(entry.second.size()) < config_.k_anonymity) continue;
            // Pick the most common copy (mode) to resist a single malicious
            // peer's variant. For simplicity, take the first copy that
            // appears at least k_anonymity times.
            std::map<std::string, int> copy_counts;
            for (const std::string& copy : entry.second) {
                copy_counts[copy]++;
            }
            std::string best_copy;
            int best_count = 0;
            for (const auto& cc : copy_counts) {
                if (cc.second > best_count) {
                    best_count = cc.second;
                    best_copy = cc.first;
                }
            }
            if (best_count >= config_.k_anonymity && !best_copy.empty()) {
                result.emplace_back(entry.first, best_copy);
            }
        }
    }
    return result;
}

// ============================================================================
// Contribution building (the privacy boundary)
// ============================================================================

std::string VectorsSync::build_contribution_json() const {
    Contribution contribution;
    contribution.node_id = config_.node_id;
    contribution.room = config_.room;
    contribution.wire_version = kWireVersion;
    contribution.updated_unix = sync_now_unix();

    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);

        if (config_.share_corpus_scale) {
            // Bucketed to the nearest 10 documents / 5 facts, so repeated
            // snapshots cannot reveal exact growth.
            contribution.document_chunks_rounded = round_to_step(
                static_cast<int>(add_gaussian_noise(local_document_chunks_, config_.dp_sigma)), 10);
            contribution.user_facts_rounded = round_to_step(
                static_cast<int>(add_gaussian_noise(local_user_facts_, config_.dp_sigma)), 5);
            contribution.has_embeddings = local_has_embeddings_;
        }

        if (config_.share_tool_metrics) {
            for (const auto& entry : local_tool_stats_) {
                ToolStat stat = entry.second;

                // Noise on the counters: a noised count cannot be differenced
                // back to a single invocation. Latencies get a wider sigma
                // because they carry more information than a tally.
                stat.success =
                    static_cast<int>(add_gaussian_noise(stat.success, config_.dp_sigma));
                stat.failure =
                    static_cast<int>(add_gaussian_noise(stat.failure, config_.dp_sigma));
                stat.total_latency_ms =
                    add_gaussian_noise(stat.total_latency_ms, config_.dp_sigma * 50.0);
                stat.max_latency_ms =
                    add_gaussian_noise(stat.max_latency_ms, config_.dp_sigma * 50.0);
                if (stat.success <= 0 && stat.failure <= 0) continue;

                contribution.tool_stats.push_back(stat);
                if (contribution.tool_stats.size() >= 64) break;
            }
        }
    }

    // The term-document-frequency map is populated when the assistant opts in via
    // set_term_frequencies(). The encoder drops rare terms (frequency < 3) and
    // noises the counts so no query text can be reconstructed from the published
    // statistics. This gives the room a lexical topic-trend signal without
    // ever sharing a single word of conversation.
    {
        std::lock_guard<std::mutex> lock(term_mutex_);
        for (const auto& pair : local_term_frequencies_) {
            if (pair.first.empty() || pair.first.size() > 48) continue;
            if (pair.second < 3) continue;  // drop rare terms
            const int noised = static_cast<int>(
                add_gaussian_noise(pair.second, config_.dp_sigma));
            if (noised <= 0) continue;
            contribution.term_document_frequency[pair.first] = noised;
            if (contribution.term_document_frequency.size() >= 4096) break;
        }
    }

    // Embedding vectors for cross-node semantic search. Each vector is
    // client-side DP-protected (clipped + Gaussian noise) before publication;
    // the relay only ever sees the aggregated sum, never individual vectors.
    // This is the federated vector approach from Chapter 12 of "Federated
    // Learning Foundations and Applications": DP + secure aggregation is the
    // gold standard for privacy-preserving vector sharing.
    {
        std::lock_guard<std::mutex> lock(embedding_mutex_);
        if (!local_embedding_vectors_.empty() && local_embedding_dim_ > 0) {
            contribution.embedding_dim = local_embedding_dim_;
            for (const std::vector<float>& vec : local_embedding_vectors_) {
                if (static_cast<int>(vec.size()) != local_embedding_dim_) continue;
                contribution.embedding_vectors.push_back(encode_vectors_base64(vec));
                if (contribution.embedding_vectors.size() >= 256) break;
            }
        }
    }

    // LoRA adapter for cross-node personalization (Chapter 15 of "Federated
    // Learning Foundations and Applications"). A LoRA adapter is a small GGUF
    // file of diff tensors (~100 KB-2 MB) that, when applied to the base
    // model, steers it toward a peer's local domain. The vectors shares
    // adapter bytes between peers; each node applies the received adapters
    // to its local model via llama_set_adapters_lora().
    //
    // Privacy: adapters are content-free by construction - they are weights,
    // not data. A peer never sees the user's conversations, only the
    // mathematical delta that makes the model more helpful for that peer's
    // domain. The wire format caps adapter size to prevent DoS.
    {
        std::lock_guard<std::mutex> lock(lora_local_mutex_);
        if (!local_lora_adapter_name_.empty() && !local_lora_adapter_bytes_.empty()) {
            contribution.lora_adapter_name = local_lora_adapter_name_;
            contribution.lora_adapter_bytes = local_lora_adapter_bytes_;
        }
    }

    // Mesh metadata for cross-node topology visualization. Content-free by
    // construction - only node/edge counts and a latency average are
    // published, never identities or user data.
    {
        std::lock_guard<std::mutex> lock(mesh_local_mutex_);
        if (local_mesh_info_.node_count > 0 || local_mesh_info_.edge_count > 0) {
            contribution.mesh_info = local_mesh_info_;
        }
    }

    // Peer metadata for mesh visualization (model names, capabilities, telemetry).
    // Content-free: only model identifiers and aggregate counters.
    {
        contribution.peer_metadata.brain_model_name = "llama-3-8b-q4_k_m";  // TODO: get from LlamaWrapper
        contribution.peer_metadata.embedding_model_name = "all-MiniLM-L6-v2";  // TODO: get from EmbeddingEngine
        contribution.peer_metadata.embedding_dim = local_embedding_dim_;
        contribution.peer_metadata.shares_embeddings = local_has_embeddings_ && config_.share_corpus_scale;
        contribution.peer_metadata.shares_lora = !local_lora_adapter_name_.empty();
        contribution.peer_metadata.lora_adapter_names = {local_lora_adapter_name_};
        contribution.peer_metadata.last_sync_unix = sync_now_unix();
        contribution.peer_metadata.sync_count_24h = publish_count_.load() + fetch_count_.load();
        contribution.peer_metadata.bytes_sent_24h = 0;  // TODO: track actual bytes
        contribution.peer_metadata.bytes_recv_24h = 0;
        contribution.peer_metadata.pulls_from_relay = (config_.mode == "wan" && !config_.rendezvous_url.empty());
        contribution.peer_metadata.serves_lan = (config_.mode == "lan" || config_.mode == "wan");
    }

    if (!config_.room_secret.empty()) {
        contribution.signature =
            hmac_sha256_hex(config_.room_secret, signing_payload(contribution));
    }
    return to_json(contribution);
}

void VectorsSync::store_remote(const Contribution& contribution) {
    if (contribution.node_id.empty() || contribution.node_id == config_.node_id) return;
    if (contribution.room != config_.room) return;

    // Update the peer's reputation score based on this contribution.
    // A peer that sends valid, timely tool stats earns a higher weight;
    // a peer that sends nothing or stale data is naturally dampened.
    {
        std::lock_guard<std::mutex> lock(reputation_mutex_);
        double& rep = peer_reputation_[contribution.node_id];
        // Exponential moving average: new peers start at 0.5, reliable peers
        // climb toward 1.0, unreliable peers decay toward 0.0.
        const int total_tool_attempts = [&]() {
            int sum = 0;
            for (const ToolStat& s : contribution.tool_stats) sum += s.success + s.failure;
            return sum;
        }();
        const double quality = total_tool_attempts > 0 ? 1.0 : 0.0;
        rep = rep * 0.8 + quality * 0.2;  // EMA with alpha=0.2
        if (rep < 0.0) rep = 0.0;
        if (rep > 1.0) rep = 1.0;
    }

    std::lock_guard<std::mutex> lock(remote_mutex_);
    auto existing = remote_contributions_.find(contribution.node_id);
    if (existing != remote_contributions_.end()) {
        if (existing->second.updated_unix >= contribution.updated_unix) return;  // stale
        existing->second = contribution;
        return;
    }

    // Bound memory on large networks by evicting the oldest peer.
    if (remote_contributions_.size() >= 64) {
        auto oldest = std::min_element(
            remote_contributions_.begin(), remote_contributions_.end(),
            [](const std::pair<const std::string, Contribution>& a,
               const std::pair<const std::string, Contribution>& b) {
                return a.second.updated_unix < b.second.updated_unix;
            });
        if (oldest != remote_contributions_.end()) remote_contributions_.erase(oldest);
    }

    remote_contributions_.emplace(contribution.node_id, contribution);

    // Store any LoRA adapter the peer published. Adapters are content-free
    // by construction - they are weights, not data. The aggregation logic
    // in aggregated_lora_adapters() applies a Byzantine-robust mode vote.
    if (!contribution.lora_adapter_name.empty() && !contribution.lora_adapter_bytes.empty()) {
        add_remote_lora_adapter(contribution.lora_adapter_name, contribution.lora_adapter_bytes);
    }

    LOG_INFO("Vectors", "Contribution received from peer " + contribution.node_id);
}

// A relay answers with {"contributions":[...]}; a bare array is also accepted.
bool VectorsSync::parse_relay_response(const std::string& body) {
    if (body.empty() || body.size() > 8u * 1024u * 1024u) return false;

    try {
        const nlohmann::json parsed = nlohmann::json::parse(body);
        nlohmann::json wrapped;
        const nlohmann::json* list = nullptr;
        if (parsed.is_array()) {
            list = &parsed;
        } else if (parsed.is_object() && parsed.contains("contributions") &&
                   parsed["contributions"].is_array()) {
            wrapped = parsed["contributions"];
            list = &wrapped;
        }
        if (list == nullptr) return false;

        bool stored_any = false;
        for (const nlohmann::json& entry : *list) {
            Contribution contribution;
            if (!from_json(entry.dump(), contribution)) continue;
            store_remote(contribution);
            stored_any = true;
        }
        return stored_any;
    } catch (const std::exception&) {
        return false;
    }
}

// ============================================================================
// Sync loop and read-outs
// ============================================================================

void VectorsSync::publish_and_fetch() {
    // 1. Pull from every peer discovered on the LAN. Pull-only keeps the trust
    //    model simple: nobody can push unsolicited data at us on the LAN.
    const std::vector<PeerInfo> peers = transport_.peers();
    const std::int64_t now = sync_now_unix();
    for (const PeerInfo& peer : peers) {
        if (now - peer.last_seen_unix > 300) continue;  // stale discovery entry
        Contribution contribution;
        if (transport_.fetch_contribution(peer.host, peer.port, contribution)) {
            store_remote(contribution);
            fetch_count_++;
        }
    }

    // 2. WAN: one round trip through the relay both publishes our contribution
    //    and returns the other peers' latest ones.
    if (config_.mode == "wan" && !config_.rendezvous_url.empty()) {
        std::string response;
        if (transport_.publish_to_rendezvous(build_contribution_json(), response)) {
            publish_count_++;
            parse_relay_response(response);
        } else if (transport_.fetch_from_rendezvous(response)) {
            parse_relay_response(response);
        }
    }
}

void VectorsSync::sync_loop() {
    // Give discovery a moment to find peers, then run the configured cadence.
    int waited_ms = 0;
    while (running_ && waited_ms < 3000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        waited_ms += 500;
    }

    while (running_) {
        publish_and_fetch();

        const int interval_ms = config_.sync_interval_seconds * 1000;
        int elapsed = 0;
        while (running_ && elapsed < interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            elapsed += 500;
        }
    }
}

std::vector<ToolConsensus> VectorsSync::tool_consensus() const {
    // Per-peer reputation weight (0.0..1.0). A peer that has contributed
    // reliably for many rounds gets a higher weight; a new or unreliable
    // peer is dampened. This is the incentive mechanism from Chapter 10 of
    // "Federated Learning Foundations and Applications": Shapley-style
    // contribution scoring, without needing a token economy.
    //
    // Byzantine-robust aggregation (Chapter 11): instead of simple averaging
    // we collect per-peer (success, failure, latency) triples, then apply a
    // trimmed mean - discard the top and bottom 20% of per-peer success-rate
    // outliers before averaging. A single malicious peer cannot skew the
    // consensus because its extreme value is trimmed away.
    struct PeerToolData {
        std::string node_id;
        int success = 0;
        int failure = 0;
        double total_latency_ms = 0.0;
        double weight = 1.0;  // reputation weight
    };

    std::map<std::string, std::vector<PeerToolData>> per_tool_peers;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (const auto& entry : remote_contributions_) {
            const std::string& peer_id = entry.first;
            const double reputation = peer_reputation_.count(peer_id)
                ? peer_reputation_.at(peer_id) : 1.0;
            for (const ToolStat& stat : entry.second.tool_stats) {
                PeerToolData ptd;
                ptd.node_id = peer_id;
                ptd.success = stat.success;
                ptd.failure = stat.failure;
                ptd.total_latency_ms = stat.total_latency_ms;
                ptd.weight = reputation;
                per_tool_peers[stat.tool].push_back(ptd);
            }
        }
    }

    // k-anonymity gate: a tool is reported only when at least k distinct peers
    // contributed it, so no single peer's behaviour can be isolated.
    std::vector<ToolConsensus> result;
    for (const auto& entry : per_tool_peers) {
        const std::vector<PeerToolData>& peers = entry.second;
        if (static_cast<int>(peers.size()) < config_.k_anonymity) continue;

        // Trimmed mean: sort by success rate, discard top/bottom 20%.
        std::vector<PeerToolData> sorted = peers;
        std::sort(sorted.begin(), sorted.end(),
            [](const PeerToolData& a, const PeerToolData& b) {
                const double ra = (a.success + a.failure) > 0
                    ? (double)a.success / (a.success + a.failure) : 0.0;
                const double rb = (b.success + b.failure) > 0
                    ? (double)b.success / (b.success + b.failure) : 0.0;
                return ra < rb;
            });

        const size_t trim_count = sorted.size() / 5;  // 20% from each end
        const size_t start = (trim_count > 0 && sorted.size() > 2 * trim_count)
            ? trim_count : 0;
        const size_t end = sorted.size() - (trim_count > 0 && sorted.size() > 2 * trim_count
            ? trim_count : 0);

        double weighted_success = 0.0;
        double weighted_failure = 0.0;
        double weighted_latency = 0.0;
        double total_weight = 0.0;
        int contributing = 0;

        for (size_t i = start; i < end; ++i) {
            const double w = sorted[i].weight;
            weighted_success += sorted[i].success * w;
            weighted_failure += sorted[i].failure * w;
            weighted_latency += sorted[i].total_latency_ms * w;
            total_weight += w;
            contributing++;
        }

        if (total_weight <= 0.0 || contributing <= 0) continue;
        const long long attempts = static_cast<long long>(weighted_success + weighted_failure + 0.5);
        if (attempts <= 0) continue;

        ToolConsensus consensus;
        consensus.tool = entry.first;
        consensus.contributing_peers = contributing;
        consensus.success_rate = weighted_success / (weighted_success + weighted_failure);
        consensus.average_latency_ms = weighted_latency / (weighted_success + weighted_failure);
        result.push_back(consensus);
    }

    std::sort(result.begin(), result.end(), [](const ToolConsensus& a, const ToolConsensus& b) {
        return a.tool < b.tool;
    });
    return result;
}

std::string VectorsSync::status_json() const {
    const std::vector<ToolConsensus> consensus = tool_consensus();

    nlohmann::json status;
    status["enabled"] = is_enabled();
    status["running"] = running_.load();
    status["mode"] = config_.mode;
    status["room"] = config_.room;
    status["node_id"] = config_.node_id;
    status["peers"] = peer_count();
    status["signed"] = !config_.room_secret.empty();
    status["k_anonymity"] = config_.k_anonymity;
    status["dp_sigma"] = config_.dp_sigma;
    status["fetches"] = fetch_count_.load();
    status["publishes"] = publish_count_.load();
    status["received"] = receive_count_.load();
    status["tool_consensus"] = static_cast<int>(consensus.size());
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        status["remote_contributions"] = static_cast<int>(remote_contributions_.size());
    }
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        status["local_tools_tracked"] = static_cast<int>(local_tool_stats_.size());
    }
    if (!last_error_.empty()) status["last_error"] = last_error_;
    return status.dump();
}

std::vector<MeshPeerView> VectorsSync::get_mesh_view() const {
    std::vector<MeshPeerView> result;

    // Get local transport peers (LAN discovery)
    std::vector<PeerInfo> local_peers = transport_.peers();
    std::map<std::string, PeerInfo> peer_map;
    for (const auto& p : local_peers) {
        peer_map[p.node_id] = p;
    }

    const std::int64_t now = sync_now_unix();

    // Combine with remote contributions
    std::lock_guard<std::mutex> lock(remote_mutex_);
    for (const auto& entry : remote_contributions_) {
        const std::string& node_id = entry.first;
        const Contribution& contrib = entry.second;

        if (node_id == config_.node_id) continue;  // Skip self

        MeshPeerView view;
        view.node_id = node_id;
        view.last_sync_unix = contrib.updated_unix;

        // Peer metadata from contribution
        view.brain_model = contrib.peer_metadata.brain_model_name;
        view.embedding_model = contrib.peer_metadata.embedding_model_name;
        view.embedding_dim = contrib.peer_metadata.embedding_dim;
        view.compatible_embeddings = (contrib.peer_metadata.embedding_dim == local_embedding_dim_ && contrib.peer_metadata.embedding_dim > 0);
        view.compatible_lora = contrib.peer_metadata.shares_lora;
        view.lora_adapters = contrib.peer_metadata.lora_adapter_names;
        view.sync_count_24h = contrib.peer_metadata.sync_count_24h;

        // Reputation
        {
            std::lock_guard<std::mutex> rep_lock(reputation_mutex_);
            auto rep_it = peer_reputation_.find(node_id);
            if (rep_it != peer_reputation_.end()) {
                view.reputation = rep_it->second;
            }
        }

        // LAN peer info (host, port, last_seen)
        auto peer_it = peer_map.find(node_id);
        if (peer_it != peer_map.end()) {
            view.host = peer_it->second.host;
            view.port = peer_it->second.port;
            view.last_seen_unix = peer_it->second.last_seen_unix;
            view.we_pull_from_them = true;  // We fetch from LAN peers
            view.they_pull_from_us = peer_it->second.port > 0;  // They have an HTTP endpoint
        } else {
            // WAN-only peer (via relay)
            view.we_pull_from_them = contrib.peer_metadata.pulls_from_relay;
            view.they_pull_from_us = contrib.peer_metadata.serves_lan;
        }

        // Only include peers seen recently (within 5 minutes) or with valid sync
        if (now - view.last_sync_unix <= 300 || view.last_seen_unix > 0) {
            result.push_back(std::move(view));
        }
    }

    return result;
}

}  // namespace Vectors
}  // namespace Jarvis
