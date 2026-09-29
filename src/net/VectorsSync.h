#pragma once
// ============================================================================
// VectorsSync.h - the local brain of the peer network.
//
// Responsibilities:
//   * records content-free local metrics (tool name, success, latency)
//   * builds the contribution that may leave this machine, applying the privacy
//     gates (bucketing, Gaussian noise, opt-in term statistics)
//   * pulls contributions from discovered peers and from an optional relay
//   * derives consensus values, but only above the k-anonymity threshold, so a
//     single peer can never be singled out
//   * persists the node identity (a random token, never a hardware id)
//
// It never reads conversations, documents, vectors or file names. The only data
// it has access to is what the assistant explicitly hands it.
// ============================================================================

#include "VectorsConfig.h"
#include "VectorsTypes.h"
#include "PeerTransport.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Jarvis {
namespace Vectors {

// Aggregated peer statistics for one tool. Only produced when at least
// `k_anonymity` distinct peers reported that tool.
struct ToolConsensus {
    std::string tool;
    int contributing_peers = 0;
    double success_rate = 0.0;       // 0..1 across all reported invocations
    double average_latency_ms = 0.0;
};

// Peer view for mesh visualization: one entry per discovered peer.
struct MeshPeerView {
    std::string node_id;
    std::string host;
    int port = 0;
    std::string brain_model;
    std::string embedding_model;
    int embedding_dim = 0;
    bool compatible_embeddings = false;
    bool compatible_lora = false;
    std::vector<std::string> lora_adapters;  // names this peer publishes
    std::int64_t last_seen_unix = 0;
    std::int64_t last_sync_unix = 0;
    int sync_count_24h = 0;
    double reputation = 0.0;  // existing peer_reputation_
    // Direction:
    bool we_pull_from_them = true;   // we fetch their contribution
    bool they_pull_from_us = false;  // would need relay/peer to report
};

class VectorsSync {
public:
    VectorsSync();
    ~VectorsSync();

    VectorsSync(const VectorsSync&) = delete;
    VectorsSync& operator=(const VectorsSync&) = delete;

    // Validates the configuration, sanitizes the room name and loads (or
    // creates) the persistent node id. Safe to call with enabled == false.
    bool initialize(const VectorsConfig& config);

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    bool is_enabled() const { return config_.enabled && config_.mode != "off"; }

    // ---- Local recording (called by the assistant) -------------------------
    // Content-free by construction: no query text, no parameters, no results.
    void record_tool_outcome(const std::string& tool, bool success, double latency_ms);
    void set_corpus_scale(int document_chunks, int user_facts, bool has_embeddings);

    // ---- Consensus read-outs ----------------------------------------------
    std::vector<ToolConsensus> tool_consensus() const;
    std::string status_json() const;

    // Optional lexical statistics: the assistant can feed the RAG engine's
    // term frequencies here. They are bucketed and noised before publication
    // (see build_contribution_json), so no query text ever leaves the machine.
    void set_term_frequencies(const std::map<std::string, int>& frequencies);

    // Optional embedding vectors for cross-node semantic search. Each vector
    // is client-side DP-protected (clipped + Gaussian noise) before
    // publication; the relay only ever sees the aggregated sum. The assistant
    // populates this from the RAG engine's local corpus.
    void set_embedding_vectors(const std::vector<std::vector<float>>& vectors,
                               int embedding_dim);

    // Optional LoRA adapter for cross-node personalization. The assistant
    // publishes its local adapter (if any); peers aggregate by name and
    // apply the most-received adapters to their local model.
void set_lora_adapter(const std::string& name, const std::string& base64_bytes);

    // Optional mesh metadata for cross-node topology visualization. The
    // assistant populates this from the peer transport's discovery
    // results; the mesh view is aggregated across peers.
    void set_mesh_info(const MeshInfo& mesh_info);

    // Add a remote peer's LoRA adapter to the aggregation pool. Called
    // internally by store_remote() when a contribution carries an adapter.
    void add_remote_lora_adapter(const std::string& name, const std::string& base64_bytes);

    // Returns the aggregated cross-node mesh view (Byzantine-robust trimmed
    // mean over peer contributions). Empty when no peers have shared mesh
    // metadata or k-anonymity is not met.
    std::vector<std::pair<std::string, MeshInfo>> mesh_view() const;

    // Returns the aggregated cross-node embedding vectors (Byzantine-robust
    // trimmed mean over peer contributions). Empty when no peers have shared
    // vectors or k-anonymity is not met.
    std::vector<std::vector<float>> aggregated_embeddings() const;

    // Returns the aggregated LoRA adapters (Byzantine-robust mode vote over
    // peer contributions). Empty when no peers have shared adapters or
    // k-anonymity is not met.
    std::vector<std::pair<std::string, std::string>> aggregated_lora_adapters() const;

    // Returns a detailed per-peer mesh view for visualization.
    // Combines remote contributions with local transport peer info.
    std::vector<MeshPeerView> get_mesh_view() const;

    const VectorsConfig& config() const { return config_; }
    std::string node_id() const { return config_.node_id; }
    int peer_count() const;

private:
    void sync_loop();
    void publish_and_fetch();
    void store_remote(const Contribution& contribution);
    bool parse_relay_response(const std::string& body);
    std::string build_contribution_json() const;
    void sanitize_room();
    void load_state();
    void save_state() const;

    VectorsConfig config_;
    PeerTransport transport_;

    mutable std::mutex metrics_mutex_;
    std::map<std::string, ToolStat> local_tool_stats_;
    int local_document_chunks_ = 0;
    int local_user_facts_ = 0;
    bool local_has_embeddings_ = false;

    // Optional opt-in lexical statistics (term -> document frequency).
    // The assistant populates this from the RAG engine; the encoder drops
    // rare terms and noises the counts before publication.
    mutable std::mutex term_mutex_;
    std::map<std::string, int> local_term_frequencies_;

    // Optional embedding vectors for cross-node semantic search. Each vector
    // is already DP-protected (clipped + noised) before publication.
    mutable std::mutex embedding_mutex_;
    std::vector<std::vector<float>> local_embedding_vectors_;
    int local_embedding_dim_ = 0;

    // Optional LoRA adapter for cross-node personalization. The assistant
    // publishes its local adapter (if any); peers aggregate by name and
    // apply the most-received adapters to their local model.
    mutable std::mutex lora_local_mutex_;
    std::string local_lora_adapter_name_;
    std::string local_lora_adapter_bytes_;

    // Optional mesh metadata for cross-node topology visualization.
    // The assistant populates this from the peer transport's discovery
    // results; the mesh view is aggregated across peers.
    mutable std::mutex mesh_local_mutex_;
    MeshInfo local_mesh_info_;

    mutable std::mutex remote_mutex_;
    std::map<std::string, Contribution> remote_contributions_;

    // Per-peer reputation weight (0.0..1.0), updated from each contribution.
    // Implements the incentive mechanism from Chapter 10 of "Federated
    // Learning Foundations and Applications": peers that consistently
    // contribute reliable tool stats are weighted more heavily in consensus,
    // while unreliable or new peers are dampened. This is a Shapley-style
    // contribution score without requiring a token economy.
    mutable std::mutex reputation_mutex_;
    std::map<std::string, double> peer_reputation_;

    // LoRA adapter state for cross-node personalization. Each peer may
    // publish one adapter; we aggregate by name and pick the most-received
    // adapters above the k-anonymity threshold.
    mutable std::mutex lora_mutex_;
    std::map<std::string, std::vector<std::string>> remote_lora_adapters_;  // name -> [base64 bytes...]

    std::atomic<bool> running_{false};
    std::thread sync_thread_;

    std::string state_dir_;
    std::string last_error_;

    std::atomic<int> fetch_count_{0};
    std::atomic<int> publish_count_{0};
    std::atomic<int> receive_count_{0};
};

}  // namespace Vectors
}  // namespace Jarvis
