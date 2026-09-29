#pragma once
// ============================================================================
// PeerTransport.h - how vectors nodes find and talk to each other.
//
// LAN  : UDP broadcast beacons + a small HTTP endpoint on each node. No server,
//        no internet, no cost.
// WAN  : the same endpoint plus an optional rendezvous/relay address, which can
//        be a relay you run yourself (tools/relay/SonnyRelay.cpp) or any address
//        inside an overlay VPN. The app never depends on a paid service.
//
// What arrives over the wire is treated strictly as data: it is parsed into a
// Contribution, size-capped, version-checked, optionally signature-checked, and
// handed to a callback. Nothing from a peer is ever executed, and no peer data
// can steer a tool, a prompt or the filesystem.
// ============================================================================

#include "VectorsConfig.h"
#include "VectorsTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Jarvis {
namespace Vectors {

struct PeerInfo {
    std::string node_id;
    std::string host;
    int port = 0;
    std::string room;
    std::int64_t last_seen_unix = 0;
};

class PeerTransport {
public:
    PeerTransport();
    ~PeerTransport();

    PeerTransport(const PeerTransport&) = delete;
    PeerTransport& operator=(const PeerTransport&) = delete;

    // Supplies our own contribution as JSON when a peer asks for it.
    void set_contribution_provider(std::function<std::string()> provider) {
        provider_ = std::move(provider);
    }
    // Invoked for every accepted, verified contribution pushed to us.
    void set_contribution_handler(std::function<void(const Contribution&)> handler) {
        handler_ = std::move(handler);
    }

    bool start(const VectorsConfig& config);
    void stop();
    bool is_running() const { return running_.load(); }

    std::string last_error() const;
    void set_last_error(const std::string& error);

    std::vector<PeerInfo> peers() const;
    void note_peer(const PeerInfo& peer);

    // Fetches and verifies a peer's contribution over HTTP.
    bool fetch_contribution(const std::string& host, int port, Contribution& out);

    // Optional WAN path: publish/collect through a rendezvous relay.
    bool publish_to_rendezvous(const std::string& body, std::string& response_body);
    bool fetch_from_rendezvous(std::string& out_body);

private:
    void udp_listen_loop();
    void beacon_loop();
    void accept_loop();
    void handle_client(unsigned long long client_socket);
    bool verify_signature(const Contribution& contribution) const;
    void register_peer(const std::string& json_text, const std::string& sender_host);

    VectorsConfig config_;
    std::function<std::string()> provider_;
    std::function<void(const Contribution&)> handler_;

    std::atomic<bool> running_{false};
    std::thread udp_thread_;
    std::thread beacon_thread_;
    std::thread accept_thread_;

    unsigned long long udp_socket_ = ~0ull;  // INVALID_SOCKET
    unsigned long long tcp_socket_ = ~0ull;

    mutable std::mutex peers_mutex_;
    std::vector<PeerInfo> peers_;

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

// Blocking GET against a plain-HTTP peer endpoint. Shared by the transport and
// the sync loop. Returns false on any transport or parse failure.
bool http_get_text(const std::string& url, std::string& out_body, int timeout_ms);

}  // namespace Vectors
}  // namespace Jarvis
