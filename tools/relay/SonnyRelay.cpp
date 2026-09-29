// ============================================================================
// SonnyRelay.cpp - the optional rendezvous relay for WAN vectorss.
//
// This exists so that "WAN mode" never requires a paid service. Build it with
// the project (target: sonny_relay) and run it on any always-on Windows machine
// you own - a home server, an unused laptop, a NAS - or on an address inside an
// overlay VPN. No account, no subscription, no container platform.
//
// What it does: keeps the latest contribution per (room, node_id) in memory and
// hands each caller the other nodes' newest contributions in one round trip.
//
// What it deliberately does NOT do:
//   * it never parses a payload beyond a structural sanity check, so a relay
//     operator cannot mine the contents
//   * it never writes anything to disk
//   * it never authenticates anyone - payloads are signed end-to-end with the
//     room secret, so a relay cannot forge or tamper undetected
//   * it never executes anything from a request
//
// Usage:  sonny_relay.exe [port]            (default 47830)
// Then on every Sonny instance:
//   vectors_mode=wan
//   vectors_rendezvous_url=http://<relay-host>:47830/sonny/vectors/v1/publish
// ============================================================================

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define closesocket ::close
  #define DWORD unsigned long
  #define BOOL int
  #define TRUE 1
  #define FALSE 0
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxBodyBytes = 131072;  // 128 KB per contribution
constexpr size_t kMaxHeaderBytes = 8192;
constexpr size_t kMaxNodesPerRoom = 256;
constexpr size_t kMaxRooms = 64;
constexpr int kEntryTtlSeconds = 24 * 60 * 60;

struct Entry {
    std::string node_id;
    std::string body;
    std::int64_t updated_unix = 0;
};

std::mutex g_store_mutex;
std::map<std::string, std::vector<Entry>> g_rooms;

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A payload is stored only if it looks like the contribution object the clients
// expect. The relay stays ignorant of the contents, but a malformed body must
// never end up inside the JSON array handed to clients.
bool looks_like_contribution(const std::string& body) {
    if (body.size() < 16 || body.size() > kMaxBodyBytes) return false;
    if (body.front() != '{' || body.back() != '}') return false;
    return body.find("\"wire_version\"") != std::string::npos;
}

std::string url_decode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const long code = strtol(hex.c_str(), &end, 16);
            if (end != nullptr && *end == '\0') {
                decoded.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::string query_value(const std::string& query, const std::string& key) {
    size_t position = 0;
    while (position < query.size()) {
        const size_t next = query.find('&', position);
        const std::string pair =
            query.substr(position, next == std::string::npos ? std::string::npos : next - position);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos && pair.substr(0, equals) == key) {
            return url_decode(pair.substr(equals + 1));
        }
        if (next == std::string::npos) break;
        position = next + 1;
    }
    return std::string();
}

std::string sanitize_token(const std::string& value, const char* fallback) {
    std::string cleaned;
    for (char c : value) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (allowed) cleaned.push_back(c);
        if (cleaned.size() >= 64) break;
    }
    return cleaned.empty() ? std::string(fallback) : cleaned;
}

std::string http_reply(const char* status, const std::string& body) {
    std::string reply = "HTTP/1.1 ";
    reply += status;
    reply += "\r\nContent-Type: application/json\r\n";
    reply += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    reply += "Connection: close\r\nCache-Control: no-store\r\n\r\n";
    reply += body;
    return reply;
}

void expire_old_entries() {
    const std::int64_t cutoff = now_unix() - kEntryTtlSeconds;
    for (auto room = g_rooms.begin(); room != g_rooms.end();) {
        std::vector<Entry>& entries = room->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [cutoff](const Entry& entry) {
                                         return entry.updated_unix < cutoff;
                                     }),
                      entries.end());
        if (entries.empty()) {
            room = g_rooms.erase(room);
        } else {
            ++room;
        }
    }
}

// Returns the contributions of every node in the room except `excluded_node`.
std::string collect_room_json(const std::string& room, const std::string& excluded_node) {
    std::string list;
    {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        expire_old_entries();
        const auto found = g_rooms.find(room);
        if (found != g_rooms.end()) {
            for (const Entry& entry : found->second) {
                if (!excluded_node.empty() && entry.node_id == excluded_node) continue;
                if (!list.empty()) list += ",";
                list += entry.body;
            }
        }
    }
    return std::string("{\"v\":1,\"contributions\":[") + list + "]}";
}

void store_contribution(const std::string& room, const std::string& node_id,
                        const std::string& body) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    expire_old_entries();

    if (g_rooms.size() >= kMaxRooms && g_rooms.find(room) == g_rooms.end()) return;

    std::vector<Entry>& entries = g_rooms[room];
    for (Entry& entry : entries) {
        if (entry.node_id == node_id) {
            entry.body = body;
            entry.updated_unix = now_unix();
            return;
        }
    }
    if (entries.size() >= kMaxNodesPerRoom) return;

    Entry entry;
    entry.node_id = node_id;
    entry.body = body;
    entry.updated_unix = now_unix();
    entries.push_back(entry);
}

std::string health_json() {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    size_t nodes = 0;
    for (const auto& room : g_rooms) nodes += room.second.size();
    return "{\"status\":\"ok\",\"rooms\":" + std::to_string(g_rooms.size()) +
           ",\"nodes\":" + std::to_string(nodes) + "}";
}

// Returns the list of active node_ids in a room (for mesh visualization).
std::string peers_json(const std::string& room) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    expire_old_entries();
    std::string list;
    const auto found = g_rooms.find(room);
    if (found != g_rooms.end()) {
        for (const Entry& entry : found->second) {
            if (!list.empty()) list += ",";
            list += "{\"node_id\":\"" + entry.node_id + "\"," +
                    "\"updated_unix\":" + std::to_string(entry.updated_unix) + "}";
        }
    }
    return "{\"v\":1,\"peers\":[" + list + "]}";
}

void handle_client(SOCKET client) {
#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
               sizeof(tv));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
               sizeof(tv));
#endif

    std::string request;
    char buffer[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos && request.size() < kMaxHeaderBytes) {
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
    std::string target;
    const size_t first_space = request.find(' ');
    if (first_space != std::string::npos) {
        method = request.substr(0, first_space);
        const size_t second_space = request.find(' ', first_space + 1);
        target = request.substr(first_space + 1,
                                second_space == std::string::npos
                                    ? std::string::npos
                                    : second_space - first_space - 1);
    }

    std::string path = target;
    std::string query;
    const size_t question = target.find('?');
    if (question != std::string::npos) {
        path = target.substr(0, question);
        query = target.substr(question + 1);
    }

    size_t content_length = 0;
    {
        std::string headers = request.substr(0, header_end);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const size_t position = headers.find("content-length:");
        if (position != std::string::npos) {
            content_length =
                static_cast<size_t>(strtoull(headers.c_str() + position + 15, nullptr, 10));
        }
    }

    const std::string room = sanitize_token(query_value(query, "room"), "default");
    const std::string node_id = sanitize_token(query_value(query, "node_id"), "");

    std::string reply;
    if (path == "/health") {
        reply = http_reply("200 OK", health_json());
    } else if (path == "/sonny/vectors/v1/peers") {
        reply = http_reply("200 OK", peers_json(room));
    } else if (path == "/sonny/vectors/v1/latest") {
        reply = http_reply("200 OK", collect_room_json(room, node_id));
    } else if (path == "/sonny/vectors/v1/publish" && method == "POST") {
        if (node_id.empty()) {
            reply = http_reply("400 Bad Request", "{\"error\":\"node_id required\"}");
        } else if (content_length > kMaxBodyBytes) {
            reply = http_reply("413 Payload Too Large", "{\"error\":\"too large\"}");
        } else {
            std::string body = request.substr(header_end + 4);
            while (body.size() < content_length) {
                const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
                if (received <= 0) break;
                body.append(buffer, static_cast<size_t>(received));
            }
            if (body.size() > content_length) body.resize(content_length);

            if (!looks_like_contribution(body)) {
                reply = http_reply("400 Bad Request", "{\"error\":\"bad payload\"}");
            } else {
                store_contribution(room, node_id, body);
                reply = http_reply("200 OK", collect_room_json(room, node_id));
            }
        }
    } else {
        reply = http_reply("404 Not Found", "{\"error\":\"unknown endpoint\"}");
    }

    send(client, reply.c_str(), static_cast<int>(reply.size()), 0);
    closesocket(client);
}

}  // namespace

int main(int argc, char* argv[]) {
    const int port = argc > 1 ? atoi(argv[1]) : 47830;

#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "[relay] Winsock initialization failed" << std::endl;
        return 1;
    }
#endif

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "[relay] socket() failed" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<u_short>(port));

    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR ||
        listen(listener, 16) == SOCKET_ERROR) {
        std::cerr << "[relay] cannot listen on port " << port << std::endl;
        closesocket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "[relay] Sonny vectors relay listening on port " << port << std::endl;
    std::cout << "[relay] publish : http://<host>:" << port << "/sonny/vectors/v1/publish"
              << std::endl;
    std::cout << "[relay] latest  : http://<host>:" << port << "/sonny/vectors/v1/latest"
              << std::endl;
    std::cout << "[relay] peers   : http://<host>:" << port << "/sonny/vectors/v1/peers"
              << std::endl;
    std::cout << "[relay] health  : http://<host>:" << port << "/health" << std::endl;
    std::cout << "[relay] payloads stay in memory and are never parsed" << std::endl;
    std::cout << "[relay] press Ctrl+C to stop" << std::endl;

    // Runs until the process is terminated: connections are handled one at a
    // time, which is ample for a handful of nodes exchanging a few kilobytes
    // every few minutes.
    for (;;) {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        SOCKET client =
            accept(listener, reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (client == INVALID_SOCKET) continue;
        handle_client(client);
    }
}
