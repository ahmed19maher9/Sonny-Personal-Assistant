#include "VectorsTypes.h"

#include <json.hpp>

namespace Jarvis {
namespace Vectors {

namespace {

using VectorsJson = nlohmann::json;

// Builds the payload object. `blank_signature` supports the sign/verify dance:
// the signature covers the payload with an empty signature field, so the JSON
// can be dumped exactly twice without any canonicalization rules.
VectorsJson to_json_object(const Contribution& contribution, bool blank_signature) {
    VectorsJson object;
    object["wire_version"] = contribution.wire_version;
    object["node_id"] = contribution.node_id;
    object["room"] = contribution.room;
    object["updated_unix"] = contribution.updated_unix;
    object["document_chunks_rounded"] = contribution.document_chunks_rounded;
    object["user_facts_rounded"] = contribution.user_facts_rounded;
    object["has_embeddings"] = contribution.has_embeddings;

    VectorsJson tools = VectorsJson::array();
    for (const ToolStat& stat : contribution.tool_stats) {
        VectorsJson entry;
        entry["tool"] = stat.tool;
        entry["success"] = stat.success;
        entry["failure"] = stat.failure;
        entry["total_latency_ms"] = stat.total_latency_ms;
        entry["max_latency_ms"] = stat.max_latency_ms;
        tools.push_back(entry);
    }
    object["tool_stats"] = tools;

    // Mesh metadata: topology + aggregate latency. Content-free by
    // construction - only node/edge counts and a latency average are
    // published, never identities or user data.
    if (contribution.mesh_info.node_count > 0 || contribution.mesh_info.edge_count > 0) {
        VectorsJson mesh;
        mesh["node_count"] = contribution.mesh_info.node_count;
        mesh["edge_count"] = contribution.mesh_info.edge_count;
        mesh["avg_latency_ms"] = contribution.mesh_info.avg_latency_ms;
        if (!contribution.mesh_info.region.empty()) {
            mesh["region"] = contribution.mesh_info.region;
        }
        object["mesh_info"] = mesh;
    }

    VectorsJson terms = VectorsJson::object();
    for (const auto& pair : contribution.term_document_frequency) {
        terms[pair.first] = pair.second;
    }
    object["term_document_frequency"] = terms;

    // Embedding vectors: base64 strings, each a float32 LE array of
    // embedding_dim elements. Only published when the operator opts in;
    // each vector is already DP-protected (clipped + noised) on the
    // publishing side.
    if (!contribution.embedding_vectors.empty()) {
        VectorsJson vectors = VectorsJson::array();
        for (const std::string& vec : contribution.embedding_vectors) {
            vectors.push_back(vec);
        }
        object["embedding_vectors"] = vectors;
        object["embedding_dim"] = contribution.embedding_dim;
    }

    object["signature"] = blank_signature ? std::string() : contribution.signature;

    // Peer metadata for mesh visualization.
    if (!contribution.peer_metadata.brain_model_name.empty() ||
        !contribution.peer_metadata.embedding_model_name.empty() ||
        contribution.peer_metadata.embedding_dim > 0 ||
        contribution.peer_metadata.shares_embeddings ||
        contribution.peer_metadata.shares_lora ||
        !contribution.peer_metadata.lora_adapter_names.empty() ||
        contribution.peer_metadata.last_sync_unix > 0 ||
        contribution.peer_metadata.sync_count_24h > 0 ||
        contribution.peer_metadata.bytes_sent_24h > 0 ||
        contribution.peer_metadata.bytes_recv_24h > 0 ||
        !contribution.peer_metadata.pulls_from_relay ||
        !contribution.peer_metadata.serves_lan) {
        VectorsJson meta;
        meta["brain_model_name"] = contribution.peer_metadata.brain_model_name;
        meta["embedding_model_name"] = contribution.peer_metadata.embedding_model_name;
        meta["embedding_dim"] = contribution.peer_metadata.embedding_dim;
        meta["shares_embeddings"] = contribution.peer_metadata.shares_embeddings;
        meta["shares_lora"] = contribution.peer_metadata.shares_lora;
        VectorsJson lora_names = VectorsJson::array();
        for (const std::string& name : contribution.peer_metadata.lora_adapter_names) {
            lora_names.push_back(name);
        }
        meta["lora_adapter_names"] = lora_names;
        meta["last_sync_unix"] = contribution.peer_metadata.last_sync_unix;
        meta["sync_count_24h"] = contribution.peer_metadata.sync_count_24h;
        meta["bytes_sent_24h"] = contribution.peer_metadata.bytes_sent_24h;
        meta["bytes_recv_24h"] = contribution.peer_metadata.bytes_recv_24h;
        meta["pulls_from_relay"] = contribution.peer_metadata.pulls_from_relay;
        meta["serves_lan"] = contribution.peer_metadata.serves_lan;
        object["peer_metadata"] = meta;
    }

    // LoRA adapter for cross-node personalization. Only published when the
    // operator opts in; the wire format caps adapter size to prevent DoS.
    if (!contribution.lora_adapter_name.empty()) {
        object["lora_adapter_name"] = contribution.lora_adapter_name;
    }
    if (!contribution.lora_adapter_bytes.empty()) {
        object["lora_adapter_bytes"] = contribution.lora_adapter_bytes;
    }
    return object;
}

}  // namespace

std::string to_json(const Contribution& contribution) {
    return to_json_object(contribution, false).dump();
}

std::string signing_payload(const Contribution& contribution) {
    return to_json_object(contribution, true).dump();
}

bool from_json(const std::string& text, Contribution& out) {
    if (text.empty()) return false;

    try {
        const VectorsJson object = VectorsJson::parse(text);
        if (!object.is_object()) return false;
        if (!object.contains("wire_version")) return false;

        const int version = object.value("wire_version", 0);
        if (version != kWireVersion) return false;

        Contribution parsed;
        parsed.wire_version = version;
        parsed.node_id = object.value("node_id", std::string());
        parsed.room = object.value("room", std::string());
        parsed.updated_unix = object.value("updated_unix", static_cast<std::int64_t>(0));
        parsed.document_chunks_rounded = object.value("document_chunks_rounded", 0);
        parsed.user_facts_rounded = object.value("user_facts_rounded", 0);
        parsed.has_embeddings = object.value("has_embeddings", false);
        parsed.signature = object.value("signature", std::string());

        // LoRA adapter for cross-node personalization. Cap the decoded size
        // to prevent a malicious peer from exhausting memory.
        if (object.contains("lora_adapter_name") &&
            object["lora_adapter_name"].is_string()) {
            std::string name = object["lora_adapter_name"].get<std::string>();
            if (!name.empty() && name.size() <= 128) {
                parsed.lora_adapter_name = name;
            }
        }
        if (object.contains("lora_adapter_bytes") &&
            object["lora_adapter_bytes"].is_string()) {
            std::string bytes = object["lora_adapter_bytes"].get<std::string>();
            // Cap at 2 MB decoded: a LoRA adapter for a 3B model is ~100 KB-2 MB.
            // A larger payload is rejected as malformed.
            if (!bytes.empty() && bytes.size() <= 4 * 1024 * 1024) {
                parsed.lora_adapter_bytes = bytes;
            }
        }

        // Mesh metadata: topology + aggregate latency. Content-free by
        // construction - only node/edge counts and a latency average are
        // published, never identities or user data.
        if (object.contains("mesh_info") && object["mesh_info"].is_object()) {
            const VectorsJson& mesh = object["mesh_info"];
            MeshInfo info;
            info.node_count = mesh.value("node_count", 0);
            info.edge_count = mesh.value("edge_count", 0);
            info.avg_latency_ms = mesh.value("avg_latency_ms", 0.0);
            if (mesh.contains("region") && mesh["region"].is_string()) {
                info.region = mesh["region"].get<std::string>();
            }
            if (info.node_count < 0 || info.node_count > 100000) info.node_count = 0;
            if (info.edge_count < 0 || info.edge_count > 1000000) info.edge_count = 0;
            if (info.avg_latency_ms < 0.0 || info.avg_latency_ms > 1e12) info.avg_latency_ms = 0.0;
            if (info.region.size() > 64) info.region.clear();
            parsed.mesh_info = info;
        }

        if (parsed.node_id.empty() || parsed.node_id.size() > 128) return false;
        if (parsed.room.size() > 128) return false;

        const VectorsJson& tools =
            object.contains("tool_stats") ? object["tool_stats"] : VectorsJson::array();
        if (tools.is_array()) {
            for (const VectorsJson& entry : tools) {
                if (!entry.is_object()) continue;
                ToolStat stat;
                stat.tool = entry.value("tool", std::string());
                if (stat.tool.empty() || stat.tool.size() > 64) continue;
                stat.success = entry.value("success", 0);
                stat.failure = entry.value("failure", 0);
                stat.total_latency_ms = entry.value("total_latency_ms", 0.0);
                stat.max_latency_ms = entry.value("max_latency_ms", 0.0);
                if (stat.success < 0 || stat.failure < 0) continue;
                if (stat.total_latency_ms < 0.0 || stat.total_latency_ms > 1e12) continue;
                parsed.tool_stats.push_back(stat);
                if (parsed.tool_stats.size() >= 256) break;
            }
        }

        if (object.contains("term_document_frequency") &&
            object["term_document_frequency"].is_object()) {
            size_t accepted = 0;
            for (auto it = object["term_document_frequency"].begin();
                 it != object["term_document_frequency"].end(); ++it) {
                if (accepted >= 4096) break;
                if (!it.value().is_number_integer()) continue;
                const int frequency = it.value().get<int>();
                if (frequency <= 0 || frequency > 1000000000) continue;
                const std::string term = it.key();
                if (term.empty() || term.size() > 48) continue;
                parsed.term_document_frequency[term] = frequency;
                ++accepted;
            }
        }

        // Embedding vectors: base64 float32 LE arrays. Each vector must match
        // the published embedding_dim; mismatched vectors are rejected.
        if (object.contains("embedding_dim") && object["embedding_dim"].is_number_integer()) {
            parsed.embedding_dim = object["embedding_dim"].get<int>();
            if (parsed.embedding_dim <= 0 || parsed.embedding_dim > 4096) {
                parsed.embedding_dim = 0;
            }
        }
        if (object.contains("embedding_vectors") && object["embedding_vectors"].is_array()) {
            size_t accepted = 0;
            for (const VectorsJson& entry : object["embedding_vectors"]) {
                if (accepted >= 256) break;
                if (!entry.is_string()) continue;
                const std::vector<float> vec = decode_vectors_base64(entry.get<std::string>());
                if (vec.empty()) continue;
                if (parsed.embedding_dim > 0 &&
                    static_cast<int>(vec.size()) != parsed.embedding_dim) continue;
                parsed.embedding_vectors.push_back(entry.get<std::string>());
                ++accepted;
            }
        }

        // Peer metadata for mesh visualization.
        if (object.contains("peer_metadata") && object["peer_metadata"].is_object()) {
            const VectorsJson& meta = object["peer_metadata"];
            parsed.peer_metadata.brain_model_name = meta.value("brain_model_name", std::string());
            parsed.peer_metadata.embedding_model_name = meta.value("embedding_model_name", std::string());
            parsed.peer_metadata.embedding_dim = meta.value("embedding_dim", 0);
            if (parsed.peer_metadata.embedding_dim < 0 || parsed.peer_metadata.embedding_dim > 4096) {
                parsed.peer_metadata.embedding_dim = 0;
            }
            parsed.peer_metadata.shares_embeddings = meta.value("shares_embeddings", false);
            parsed.peer_metadata.shares_lora = meta.value("shares_lora", false);
            if (meta.contains("lora_adapter_names") && meta["lora_adapter_names"].is_array()) {
                for (const VectorsJson& entry : meta["lora_adapter_names"]) {
                    if (entry.is_string()) {
                        std::string name = entry.get<std::string>();
                        if (!name.empty() && name.size() <= 128) {
                            parsed.peer_metadata.lora_adapter_names.push_back(name);
                        }
                    }
                }
            }
            parsed.peer_metadata.last_sync_unix = meta.value("last_sync_unix", static_cast<std::int64_t>(0));
            if (parsed.peer_metadata.last_sync_unix < 0) parsed.peer_metadata.last_sync_unix = 0;
            parsed.peer_metadata.sync_count_24h = meta.value("sync_count_24h", 0);
            if (parsed.peer_metadata.sync_count_24h < 0) parsed.peer_metadata.sync_count_24h = 0;
            parsed.peer_metadata.bytes_sent_24h = meta.value("bytes_sent_24h", 0);
            if (parsed.peer_metadata.bytes_sent_24h < 0) parsed.peer_metadata.bytes_sent_24h = 0;
            parsed.peer_metadata.bytes_recv_24h = meta.value("bytes_recv_24h", 0);
            if (parsed.peer_metadata.bytes_recv_24h < 0) parsed.peer_metadata.bytes_recv_24h = 0;
            parsed.peer_metadata.pulls_from_relay = meta.value("pulls_from_relay", true);
            parsed.peer_metadata.serves_lan = meta.value("serves_lan", true);
        }

        out = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace Vectors
}  // namespace Jarvis
