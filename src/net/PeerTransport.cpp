#include "PeerTransport.h"
#include "VectorsCrypto.h"
#include "HttpClient.h"
#include "Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace Jarvis {
namespace Vectors {

namespace {

constexpr unsigned long long kInvalidSocket = static_cast<unsigned long long>(~0ull);

void ensure_winsock() {
    static bool initialized = false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) return;
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    initialized = true;
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string peer_host_string(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) return "";
    return std::string(buffer);
}

// Waits until a socket is readable or the timeout expires, so the accept loop
// can notice a shutdown request instead of blocking forever.
bool wait_readable(SOCKET socket_handle, int timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_handle, &read_set);
    timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
}

std::string http_response(const std::string& status, const std::string& content_type,
                          const std::string& body) {
    std::string response = "HTTP/1.1 " + status + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Cache-Control: no-store\r\n\r\n";
    response += body;
    return response;
}

}  // namespace

PeerTransport::PeerTransport() = default;

PeerTransport::~PeerTransport() {
    stop();
}

void PeerTransport::set_last_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
}

std::string PeerTransport::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void PeerTransport::note_peer(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (PeerInfo& existing : peers_) {
        if (existing.node_id == peer.node_id) {
            existing = peer;  // refresh: a laptop may change address
            return;
        }
    }
    peers_.push_back(peer);
    LOG_INFO("Vectors", "Peer joined: " + peer.node_id + " @ " + peer.host + ":" +
                                  std::to_string(peer.port) + " (room " + peer.room + ")");
}

std::vector<PeerInfo> PeerTransport::peers() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_;
}

bool PeerTransport::start(const VectorsConfig& config) {
    stop();
    config_ = config;
    ensure_winsock();

    if (config_.node_id.empty()) {
        set_last_error("node id missing");
        return false;
    }

    // ---- UDP discovery -----------------------------------------------------
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
        set_last_error("UDP socket creation failed");
        return false;
    }
    BOOL reuse = TRUE;
    BOOL broadcast = TRUE;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
               sizeof(broadcast));

    sockaddr_in udp_address{};
    udp_address.sin_family = AF_INET;
    udp_address.sin_addr.s_addr = htonl(INADDR_ANY);
    udp_address.sin_port = htons(static_cast<u_short>(config_.discovery_port));
    if (bind(udp, reinterpret_cast<const sockaddr*>(&udp_address), sizeof(udp_address)) ==
        SOCKET_ERROR) {
        closesocket(udp);
        set_last_error("UDP port " + std::to_string(config_.discovery_port) +
                       " is unavailable (another Sonny instance already uses it?)");
        return false;
    }
    DWORD receive_timeout = 500;  // lets the receive loop observe stop()
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout),
               sizeof(receive_timeout));
    udp_socket_ = static_cast<unsigned long long>(udp);

    // ---- TCP endpoint ------------------------------------------------------
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp == INVALID_SOCKET) {
        closesocket(udp);
        udp_socket_ = kInvalidSocket;
        set_last_error("TCP socket creation failed");
        return false;
    }
    setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in tcp_address{};
    tcp_address.sin_family = AF_INET;
    tcp_address.sin_addr.s_addr = htonl(INADDR_ANY);
    tcp_address.sin_port = htons(static_cast<u_short>(config_.listen_port));
    if (bind(tcp, reinterpret_cast<const sockaddr*>(&tcp_address), sizeof(tcp_address)) ==
            SOCKET_ERROR ||
        listen(tcp, 8) == SOCKET_ERROR) {
        closesocket(tcp);
        closesocket(udp);
        udp_socket_ = kInvalidSocket;
        set_last_error("TCP port " + std::to_string(config_.listen_port) + " is unavailable");
        return false;
    }
    tcp_socket_ = static_cast<unsigned long long>(tcp);

    running_ = true;
    udp_thread_ = std::thread(&PeerTransport::udp_listen_loop, this);
    beacon_thread_ = std::thread(&PeerTransport::beacon_loop, this);
    accept_thread_ = std::thread(&PeerTransport::accept_loop, this);

    LOG_INFO("Vectors", "Transport listening - UDP discovery " +
                                  std::to_string(config_.discovery_port) + ", TCP " +
                                  std::to_string(config_.listen_port) + ", room '" +
                                  config_.room + "'");
    return true;
}

void PeerTransport::udp_listen_loop() {
    const SOCKET udp = static_cast<SOCKET>(udp_socket_);
    char buffer[2048];

    while (running_) {
        sockaddr_in sender{};
        int sender_length = sizeof(sender);
        const int received = recvfrom(udp, buffer, static_cast<int>(sizeof(buffer)) - 1, 0,
                                      reinterpret_cast<sockaddr*>(&sender), &sender_length);
        if (received <= 0) continue;  // receive timeout or shutdown: re-check running_
        register_peer(std::string(buffer, static_cast<size_t>(received)), peer_host_string(sender));
    }
}

void PeerTransport::beacon_loop() {
    const SOCKET udp = static_cast<SOCKET>(udp_socket_);

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<u_short>(config_.discovery_port));
    target.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    nlohmann::json beacon;
    beacon["v"] = kWireVersion;
    beacon["node_id"] = config_.node_id;
    beacon["port"] = config_.listen_port;
    beacon["room"] = config_.room;
    const std::string payload = beacon.dump();

    while (running_) {
        sendto(udp, payload.c_str(), static_cast<int>(payload.size()), 0,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target));

        // Sleep in slices so a shutdown does not wait out the whole interval.
        const int interval_ms = std::max(5, config_.discovery_interval_seconds) * 1000;
        int waited_ms = 0;
        while (running_ && waited_ms < interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            waited_ms += 250;
        }
    }
}

void PeerTransport::accept_loop() {
    const SOCKET listener = static_cast<SOCKET>(tcp_socket_);

    while (running_) {
        if (!wait_readable(listener, 500)) continue;

        sockaddr_in client_address{};
        int client_length = sizeof(client_address);
        SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&client_address),
                               &client_length);
        if (client == INVALID_SOCKET) continue;

        // Handled inline: peers poll every few minutes, and the socket timeouts
        // inside handle_client() bound how long a stalled peer can hold the loop.
        handle_client(static_cast<unsigned long long>(client));
    }
}

void PeerTransport::handle_client(unsigned long long client_socket) {
    const SOCKET client = static_cast<SOCKET>(client_socket);

    DWORD timeout = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));

    std::string request;
    char buffer[2048];
    size_t header_end = std::string::npos;
    const size_t header_limit = 8192;
    while (header_end == std::string::npos && request.size() < header_limit) {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) {
            closesocket(client);
            return;
        }
        request.append(buffer, static_cast<size_t>(received));
        header_end = request.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) {
        closesocket(client);
        return;
    }

    std::string method;
    std::string path;
    const size_t line_end = request.find("\r\n");
    if (line_end != std::string::npos) {
        std::istringstream line(request.substr(0, line_end));
        std::string version;
        line >> method >> path >> version;
    }
    const size_t query_position = path.find('?');
    if (query_position != std::string::npos) path.resize(query_position);

    size_t content_length = 0;
    {
        std::string lower = request.substr(0, header_end);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const size_t position = lower.find("content-length:");
        if (position != std::string::npos) {
            content_length =
                static_cast<size_t>(strtoull(request.c_str() + position + 15, nullptr, 10));
        }
    }
    if (content_length > static_cast<size_t>(config_.max_body_bytes)) {
        const std::string response = http_response("413 Payload Too Large", "text/plain", "too large");
        send(client, response.c_str(), static_cast<int>(response.size()), 0);
        closesocket(client);
        return;
    }

    std::string body = request.substr(header_end + 4);
    while (body.size() < content_length) {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) break;
        body.append(buffer, static_cast<size_t>(received));
    }
    if (body.size() > content_length) body.resize(content_length);

    const std::string prefix = "/sonny/vectors/v1/";
    std::string response;

    if (method == "GET" && path == prefix + "ping") {
        nlohmann::json info;
        info["v"] = kWireVersion;
        info["node_id"] = config_.node_id;
        info["room"] = config_.room;
        response = http_response("200 OK", "application/json", info.dump());
    } else if (method == "GET" && path == prefix + "contribution") {
        const std::string payload = provider_ ? provider_() : std::string();
        response = payload.empty()
                       ? http_response("503 Service Unavailable", "text/plain", "not ready")
                       : http_response("200 OK", "application/json", payload);
    } else if (method == "POST" && path == prefix + "contribution") {
        Contribution contribution;
        if (!from_json(body, contribution)) {
            response = http_response("400 Bad Request", "text/plain", "bad payload");
        } else if (contribution.room != config_.room) {
            response = http_response("403 Forbidden", "text/plain", "wrong room");
        } else if (!verify_signature(contribution)) {
            response = http_response("401 Unauthorized", "text/plain", "bad signature");
        } else if (contribution.node_id == config_.node_id) {
            response = http_response("200 OK", "application/json", "{\"accepted\":false}");
        } else {
            if (handler_) handler_(contribution);
            response = http_response("200 OK", "application/json", "{\"accepted\":true}");
        }
    } else {
        response = http_response("404 Not Found", "text/plain", "unknown endpoint");
    }

    send(client, response.c_str(), static_cast<int>(response.size()), 0);
    closesocket(client);
}

void PeerTransport::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Closing the sockets unblocks the receive/accept loops immediately.
    if (udp_socket_ != kInvalidSocket) {
        closesocket(static_cast<SOCKET>(udp_socket_));
        udp_socket_ = kInvalidSocket;
    }
    if (tcp_socket_ != kInvalidSocket) {
        closesocket(static_cast<SOCKET>(tcp_socket_));
        tcp_socket_ = kInvalidSocket;
    }
    if (udp_thread_.joinable()) udp_thread_.join();
    if (beacon_thread_.joinable()) beacon_thread_.join();
    if (accept_thread_.joinable()) accept_thread_.join();
}

// A beacon is broadcast every few seconds, so a peer that moved (DHCP, Wi-Fi
// roaming, VPN) is simply re-registered instead of going stale.
void PeerTransport::register_peer(const std::string& json_text, const std::string& sender_host) {
    if (json_text.empty() || json_text.size() > 2048) return;

    try {
        const nlohmann::json object = nlohmann::json::parse(json_text);
        if (!object.is_object()) return;
        if (object.value("v", 0) != kWireVersion) return;

        PeerInfo peer;
        peer.node_id = object.value("node_id", std::string());
        peer.room = object.value("room", std::string());
        peer.port = object.value("port", 0);
        peer.host = sender_host;
        peer.last_seen_unix = now_unix();

        if (peer.node_id.empty() || peer.node_id == config_.node_id) return;
        if (peer.room != config_.room) return;  // cohort isolation
        if (peer.port <= 0 || peer.port > 65535) return;

        note_peer(peer);
    } catch (const std::exception&) {
        // Garbage on the discovery port is expected on a shared network.
    }
}

bool PeerTransport::verify_signature(const Contribution& contribution) const {
    if (config_.room_secret.empty()) return true;  // unsigned mode (trusted LAN)

    const std::string expected = hmac_sha256_hex(config_.room_secret, signing_payload(contribution));
    if (expected.empty()) return false;
    return secure_equals(expected, contribution.signature);
}

bool PeerTransport::fetch_contribution(const std::string& host, int port, Contribution& out) {
    if (host.empty() || port <= 0 || port > 65535) return false;

    const std::string url =
        "http://" + host + ":" + std::to_string(port) + "/sonny/vectors/v1/contribution";
    std::string body;
    if (!http_get_text(url, body, config_.http_timeout_ms)) return false;
    if (body.empty() || body.size() > static_cast<size_t>(config_.max_body_bytes)) return false;

    Contribution contribution;
    if (!from_json(body, contribution)) return false;
    if (contribution.room != config_.room) return false;
    if (contribution.node_id == config_.node_id) return false;
    if (!verify_signature(contribution)) return false;

    out = std::move(contribution);
    return true;
}

bool PeerTransport::publish_to_rendezvous(const std::string& body, std::string& response_body) {
    if (config_.rendezvous_url.empty()) return false;

    // The room is a query parameter so one relay can serve several cohorts
    // without any per-room configuration.
    const bool has_query = config_.rendezvous_url.find('?') != std::string::npos;
    const std::string url = config_.rendezvous_url + (has_query ? "&" : "?") +
                            "room=" + config_.room + "&node_id=" + config_.node_id;

    net::HttpClient client;
    net::HttpResponse response;
    if (!client.post_json(url, body, response, config_.http_timeout_ms)) return false;
    response_body = response.body;
    return response.success;
}

bool PeerTransport::fetch_from_rendezvous(std::string& out_body) {
    if (config_.rendezvous_url.empty()) return false;

    const bool has_query = config_.rendezvous_url.find('?') != std::string::npos;
    const std::string url = config_.rendezvous_url + (has_query ? "&" : "?") +
                            "room=" + config_.room + "&node_id=" + config_.node_id +
                            "&other=1";
    return http_get_text(url, out_body, config_.http_timeout_ms);
}

bool http_get_text(const std::string& url, std::string& out_body, int timeout_ms) {
    net::HttpClient client;
    net::HttpResponse response;
    if (!client.get(url, response, timeout_ms)) {
        out_body.clear();
        return false;
    }
    out_body = response.body;
    return response.success;
}

}  // namespace Vectors
}  // namespace Jarvis
