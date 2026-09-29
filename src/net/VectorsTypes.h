#pragma once
// ============================================================================
// VectorsTypes.h - the payload the peers exchange.
//
// Privacy contract (enforced by ContributionEncoder/VectorsSync):
//   * No user text, file names, queries, titles or vectors are ever included.
//   * Counters are bucketed/rounded, and numeric aggregates may be noised.
//   * A peer only ever receives DATA. Nothing in a payload can influence tool
//     execution, prompts or file access.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Jarvis {
namespace Vectors {

// Wire version. Receivers reject unknown versions so a partial rollout cannot
// corrupt a peer's aggregates.
constexpr int kWireVersion = 1;

namespace {

// Minimal base64 encoder/decoder. Implemented inline so both
// VectorsTypes.cpp and VectorsSync.cpp can use it without an
// anonymous-namespace ODR conflict in the unity build.
constexpr const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode_vec(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int triple = 0;
        int pad = 0;
        for (int j = 0; j < 3; ++j) {
            triple <<= 8;
            if (i + j < len) {
                triple |= data[i + j];
            } else {
                triple |= 0;
                ++pad;
            }
        }
        for (int j = 0; j < 4 - pad; ++j) {
            out += kBase64Chars[(triple >> (6 * (3 - j))) & 0x3F];
        }
        for (int j = 0; j < pad; ++j) out += '=';
    }
    return out;
}

inline std::string base64_decode_vec(const std::string& in) {
    if (in.empty()) return {};
    std::string out;
    out.reserve((in.size() / 4) * 3);
    unsigned int buffer = 0;
    int bits = 0;
    int pad = 0;
    for (char c : in) {
        if (c == '=') { ++pad; break; }
        int val = -1;
        for (int i = 0; kBase64Chars[i]; ++i) {
            if (kBase64Chars[i] == c) { val = i; break; }
        }
        if (val < 0) continue;
        buffer = (buffer << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
            buffer &= (1 << bits) - 1;
        }
    }
    return out;
}

}  // namespace

// Base64-encode a float32 array (little-endian) for transport.
inline std::string encode_vectors_base64(const std::vector<float>& floats) {
    if (floats.empty()) return std::string();
    return base64_encode_vec(
        reinterpret_cast<const unsigned char*>(floats.data()),
        floats.size() * sizeof(float));
}

// Base64-decode into a float32 array. Returns an empty vector on failure.
inline std::vector<float> decode_vectors_base64(const std::string& b64) {
    if (b64.empty()) return {};
    const std::string decoded = base64_decode_vec(b64);
    if (decoded.empty() || decoded.size() % sizeof(float) != 0) return {};
    const size_t count = decoded.size() / sizeof(float);
    std::vector<float> floats(count);
    std::memcpy(floats.data(), decoded.data(), decoded.size());
    return floats;
}

// Per-tool outcome counters. A tool name is not user data: the set of tool
// names is fixed at build time.
struct ToolStat {
    std::string tool;
    int success = 0;
    int failure = 0;
    double total_latency_ms = 0.0;
    double max_latency_ms = 0.0;
};

// Mesh metadata: describes a discovered peer in the mesh network.
// Content-free by construction: only topology (node count, edges) and
// aggregate latency statistics are shared, never identities or data.
struct MeshInfo {
    int node_count = 0;          // number of nodes in this peer's view
    int edge_count = 0;          // number of edges in this peer's view
    double avg_latency_ms = 0.0; // average round-trip latency to this peer
    std::string region;          // optional region label (e.g. "us-east", "eu-west")
};

// Peer capability metadata for mesh visualization.
// Content-free: only model names, versions, and aggregate counters.
// No user text, queries, vectors, or file names are included.
struct PeerMetadata {
    std::string brain_model_name;      // e.g., "llama-3-8b-q4_k_m"
    std::string embedding_model_name;  // e.g., "all-MiniLM-L6-v2"
    int embedding_dim = 0;
    bool shares_embeddings = false;
    bool shares_lora = false;
    std::vector<std::string> lora_adapter_names;  // what this node publishes
    // Telemetry (bucketed/rounded, no raw counts):
    int64_t last_sync_unix = 0;
    int sync_count_24h = 0;
    int bytes_sent_24h = 0;
    int bytes_recv_24h = 0;
    // Direction flags (for edge rendering):
    bool pulls_from_relay = true;   // we fetch from relay
    bool serves_lan = true;         // we accept LAN HTTP requests
};

struct Contribution {
    std::string node_id;
    std::string room;
    int wire_version = kWireVersion;
    std::int64_t updated_unix = 0;

    // Corpus scale, rounded to the nearest 10 so an exact document count cannot
    // be fingerprinted across snapshots.
    int document_chunks_rounded = 0;
    int user_facts_rounded = 0;
    bool has_embeddings = false;

    std::vector<ToolStat> tool_stats;

    // Mesh metadata: describes a discovered peer in the mesh network.
    // Content-free by construction: only topology (node count, edges) and
    // aggregate latency statistics are shared, never identities or data.
    MeshInfo mesh_info;

    // Peer capability metadata for mesh visualization.
    // Content-free: only model names, versions, and aggregate counters.
    // No user text, queries, vectors, or file names are included.
    PeerMetadata peer_metadata;

    // Term -> rounded document frequency. Only populated when the operator opts
    // in; the encoder drops rare terms before this is ever filled.
    std::map<std::string, int> term_document_frequency;

    // Optional embedding vectors for cross-node semantic search. Each vector
    // is client-side DP-protected (clipped + Gaussian noise) before
    // publication, and the relay only ever sees the aggregated sum - never
    // individual vectors. This is the federated vector approach from Chapter 12
    // of "Federated Learning Foundations and Applications": DP + secure
    // aggregation is the gold standard for privacy-preserving vector sharing.
    //
    // Vectors are stored as base64-encoded float arrays (little-endian).
    // The wire format includes the dimension count so a peer can validate
    // compatibility before attempting to use the vectors.
    std::vector<std::string> embedding_vectors;  // base64(float32 LE), each with known dim
    int embedding_dim = 0;

    // HMAC-SHA256 (hex) over the payload with `signature` emptied. Present only
    // when a room secret is configured.
    std::string signature;

    // Optional LoRA adapter for cross-node personalization (Chapter 15 of
    // "Federated Learning Foundations and Applications"). A LoRA adapter is a
    // small GGUF file of diff tensors (~100 KB-2 MB) that, when applied to the
    // base model, steers it toward a peer's local domain. The vectors
    // shares adapter bytes between peers; each node applies the received
    // adapters to its local model via llama_set_adapters_lora().
    //
    // Privacy: adapters are content-free by construction - they are weights,
    // not data. A peer never sees the user's conversations, only the
    // mathematical delta that makes the model more helpful for that peer's
    // domain. The wire format caps adapter size to prevent DoS.
    //
    // Stored as base64-encoded GGUF bytes. The name is a free-form label
    // (e.g. "medical", "legal", "code-review") used for deduplication.
    std::string lora_adapter_name;
    std::string lora_adapter_bytes;  // base64(GGUF), capped at 2 MB decoded
};

// Serialization. Deterministic: object keys are emitted in sorted order, which
// is what makes sign-after-serialize / verify-before-parse reliable.
std::string to_json(const Contribution& contribution);

// Strict parse: returns false on malformed JSON, a missing/unknown wire
// version, or a payload larger than the caller's cap. Never throws.
bool from_json(const std::string& text, Contribution& out);

// The exact bytes that are signed: the payload with an empty signature field.
std::string signing_payload(const Contribution& contribution);

}  // namespace Vectors
}  // namespace Jarvis
