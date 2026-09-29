#pragma once
// ============================================================================
// VectorsConfig.h - configuration for the peer metrics network.
//
// Every field is opt-in and content-free by construction: the vectors
// exchanges counters and statistics, never conversations, documents or vectors.
//
// Modes:
//   "off" - nothing is started.
//   "lan" - UDP broadcast discovery + direct HTTP between machines on the same
//           network. No internet access, no server, no cost.
//   "wan" - same as "lan" plus peers reachable through `rendezvous_url`, which
//           may be a relay you host yourself (tools/relay/SonnyRelay.cpp) or an
//           address inside an overlay VPN (Tailscale/ZeroTier/WireGuard). The
//           app itself never requires a paid service.
// ============================================================================

#include <string>

namespace Jarvis {
namespace Vectors {

struct VectorsConfig {
    bool enabled = false;
    std::string mode = "off";        // "off" | "lan" | "wan"
    std::string room = "default";    // cohort name: only same-room peers exchange data
    std::string room_secret;         // optional shared secret -> HMAC-signed payloads
    std::string rendezvous_url;      // optional relay/overlay address for "wan"

    int listen_port = 47821;         // TCP: serves our contribution to peers
    int discovery_port = 47820;      // UDP: broadcast discovery beacons
    int sync_interval_seconds = 300; // publish/fetch cadence
    int discovery_interval_seconds = 20;
    int http_timeout_ms = 4000;
    int max_body_bytes = 131072;     // hard cap on any accepted payload

    // What leaves this machine.
    bool share_tool_metrics = true;      // tool name + success/failure + latency
    bool share_corpus_scale = true;      // bucketed chunk/fact counts

    // Privacy gates applied before anything is published.
    int k_anonymity = 3;             // consensus needs >= k distinct peers
    float dp_sigma = 1.0f;           // Gaussian noise for numeric aggregates (0 = off)
    float max_remote_weight = 0.15f; // cap on remote influence over local ranking

    // Identity. Generated once and persisted; never derived from hardware ids.
    std::string node_id;
};

}  // namespace Vectors
}  // namespace Jarvis
