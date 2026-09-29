// BrowserWebSocket.cpp - see BrowserWebSocket.h for the design notes.
//
// Winsock must be pulled in before <windows.h>, so the include order here is
// intentional and must not be "tidied".
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "BrowserWebSocket.h"
#include "BrowserCrypto.h"
#include "BrowserUtil.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <chrono>

namespace Jarvis {
namespace Browser {

constexpr size_t kMaxMessageBytes = 96u * 1024u * 1024u;  // hard cap (screenshots are big)
constexpr int kRecvChunk = 64 * 1024;

// ------------------------------------------------------------ lifecycle ----

WebSocketClient::WebSocketClient() : socket_(-1) {
    ensure_winsock();
}

WebSocketClient::~WebSocketClient() {
    close();
}

void WebSocketClient::close() {
    if (socket_ >= 0) {
        // Best effort courtesy close frame, then hard shutdown.
        write_frame(0x8, "");
        shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = -1;
    }
    read_buffer_.clear();
}

int WebSocketClient::wait_readable(int timeout_ms) const {
    if (socket_ < 0) return -1;
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(static_cast<SOCKET>(socket_), &read_set);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(0, &read_set, nullptr, nullptr, &tv);
    if (rc > 0) return 1;
    if (rc == 0) return 0;
    return -1;
}

int WebSocketClient::wait_writable(int timeout_ms) const {
    if (socket_ < 0) return -1;
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(static_cast<SOCKET>(socket_), &write_set);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(0, nullptr, &write_set, nullptr, &tv);
    if (rc > 0) return 1;
    if (rc == 0) return 0;
    return -1;
}

bool WebSocketClient::send_all(const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int ready = wait_writable(5000);
        if (ready != 1) return false;
        int chunk = static_cast<int>((len - sent) > 65536 ? 65536 : (len - sent));
        int rc = send(static_cast<SOCKET>(socket_), data + sent, chunk, 0);
        if (rc <= 0) return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool WebSocketClient::write_frame(unsigned char opcode, const std::string& payload) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (socket_ < 0) return false;

    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode

    const size_t len = payload.size();
    if (len < 126) {
        // Client -> server frames are always masked (RFC 6455 section 5.3).
        frame.push_back(static_cast<char>(0x80 | static_cast<unsigned char>(len)));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> shift) & 0xFF));
        }
    }

    unsigned char mask[4];
    for (size_t i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(rand() & 0xFF);
    frame.append(reinterpret_cast<char*>(mask), 4);

    size_t payload_start = frame.size();
    frame.append(payload);
    for (size_t i = 0; i < len; ++i) {
        frame[payload_start + i] = static_cast<char>(
            static_cast<unsigned char>(frame[payload_start + i]) ^ mask[i % 4]);
    }
    return send_all(frame.data(), frame.size());
}

bool WebSocketClient::send_text(const std::string& payload, std::string* error) {
    if (!write_frame(0x1, payload)) {
        if (error) *error = "WebSocket send failed (connection closed?)";
        return false;
    }
    return true;
}
// ------------------------------------------------ connect + handshake ------

bool WebSocketClient::connect(const std::string& host, int port, const std::string& path,
                              int timeout_ms, std::string* error) {
    close();
    ensure_winsock();

    ADDRINFOA hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port_text = std::to_string(port);
    PADDRINFOA result = nullptr;
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || !result) {
        if (error) *error = "Cannot resolve host: " + host;
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (PADDRINFOA entry = result; entry != nullptr; entry = entry->ai_next) {
        sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        u_long non_blocking = 1;
        ioctlsocket(sock, FIONBIO, &non_blocking);
        int rc = ::connect(sock, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
        if (rc == SOCKET_ERROR) {
            int last = WSAGetLastError();
            if (last != WSAEWOULDBLOCK) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(sock, &write_set);
            timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            if (select(0, nullptr, &write_set, nullptr, &tv) <= 0) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }
            int so_error = 0;
            int so_len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len);
            if (so_error != 0) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }
        }
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        break;
    }
    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        if (error) *error = "TCP connect failed to " + host + ":" + std::to_string(port);
        return false;
    }

    // Small latency wins for a loopback control channel.
    BOOL no_delay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
               sizeof(no_delay));

    socket_ = static_cast<long long>(sock);
    read_buffer_.clear();

    if (!handshake(host, port, path, timeout_ms, error)) {
        close();
        return false;
    }
    return true;
}

bool WebSocketClient::handshake(const std::string& host, int port, const std::string& path,
                                int timeout_ms, std::string* error) {
    const std::string key = Crypto::random_base64_key();

    std::string request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    // Deliberately no Origin header: Chrome >= 111 rejects DevTools websocket
    // upgrades that carry an Origin unless --remote-allow-origins is set, and
    // omitting it also lets Sonny attach to a browser it did not launch.
    request += "\r\n";

    if (!send_all(request.data(), request.size())) {
        if (error) *error = "Failed to send WebSocket handshake";
        return false;
    }

    // Read until the end of the response headers, keeping any extra bytes
    // (the first frame can arrive in the same TCP segment as the 101 reply).
    const std::string marker = "\r\n\r\n";
    size_t header_end = std::string::npos;
    int64_t deadline = now_ms() + timeout_ms;
    while (header_end == std::string::npos) {
        int remaining = static_cast<int>(deadline - now_ms());
        if (remaining <= 0) {
            if (error) *error = "Timed out waiting for WebSocket handshake response";
            return false;
        }
        int ready = wait_readable(remaining);
        if (ready != 1) {
            if (error) *error = "Timed out waiting for WebSocket handshake response";
            return false;
        }
        char chunk[kRecvChunk];
        int received = recv(static_cast<SOCKET>(socket_), chunk, sizeof(chunk), 0);
        if (received <= 0) {
            if (error) *error = "Connection closed during WebSocket handshake";
            return false;
        }
        read_buffer_.append(static_cast<const char*>(chunk), static_cast<size_t>(received));
        header_end = read_buffer_.find(marker);
    }

    const std::string header_block = read_buffer_.substr(0, header_end + marker.size());
    read_buffer_.erase(0, header_end + marker.size());

    if (header_block.rfind("HTTP/1.1 101", 0) != 0) {
        // Surface the first status line so failures are diagnosable.
        size_t line_end = header_block.find("\r\n");
        if (error) *error = "WebSocket upgrade rejected: " + header_block.substr(0, line_end);
        return false;
    }

    const std::string expected_accept =
        Crypto::sha1_base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    std::string lower_headers = to_lower(header_block);
    size_t accept_pos = lower_headers.find("sec-websocket-accept:");
    if (accept_pos != std::string::npos) {
        size_t value_start = accept_pos + strlen("sec-websocket-accept:");
        size_t value_end = header_block.find("\r\n", value_start);
        std::string accept_value = trim(header_block.substr(value_start, value_end - value_start));
        if (accept_value != expected_accept) {
            if (error) *error = "WebSocket handshake key mismatch";
            return false;
        }
    }
    return true;
}

// Fill the internal buffer with at least `needed` bytes.
// 1 = ok, 0 = timeout, -1 = closed/error.
int WebSocketClient::ensure_buffered(size_t needed, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    while (read_buffer_.size() < needed) {
        int remaining = static_cast<int>(deadline - now_ms());
        if (remaining <= 0) return read_buffer_.size() >= needed ? 1 : 0;
        int ready = wait_readable(remaining);
        if (ready < 0) return -1;
        if (ready == 0) return 0;
        char chunk[kRecvChunk];
        int received = recv(static_cast<SOCKET>(socket_), chunk, sizeof(chunk), 0);
        if (received <= 0) {
            close();
            return -1;
        }
        read_buffer_.append(static_cast<const char*>(chunk), static_cast<size_t>(received));
    }
    return 1;
}

int WebSocketClient::receive(std::string& out, int timeout_ms) {
    out.clear();
    if (socket_ < 0) return -1;

    std::string fragmented_message;
    bool fragmented = false;
    int64_t deadline = now_ms() + timeout_ms;

    for (;;) {
        int remaining = static_cast<int>(deadline - now_ms());
        if (remaining <= 0) return 0;

        // ---- frame header (2 bytes, optionally extended) ----
        if (ensure_buffered(2, remaining) != 1) {
            return socket_ < 0 ? -1 : 0;
        }
        const unsigned char b0 = static_cast<unsigned char>(read_buffer_[0]);
        const unsigned char b1 = static_cast<unsigned char>(read_buffer_[1]);
        const bool fin = (b0 & 0x80) != 0;
        const unsigned char opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;

        size_t extended_bytes = 0;
        if (payload_len == 126) {
            extended_bytes = 2;
        } else if (payload_len == 127) {
            extended_bytes = 8;
        }
        size_t total_header = 2 + extended_bytes + (masked ? 4 : 0);

        if (ensure_buffered(total_header, remaining) != 1) {
            return socket_ < 0 ? -1 : 0;
        }
        size_t cursor = 2;
        if (extended_bytes == 2) {
            payload_len = (static_cast<uint64_t>(static_cast<unsigned char>(read_buffer_[2])) << 8) |
                          static_cast<unsigned char>(read_buffer_[3]);
            cursor = 4;
        } else if (extended_bytes == 8) {
            payload_len = 0;
            for (size_t i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) |
                              static_cast<unsigned char>(read_buffer_[2 + i]);
            }
            cursor = 10;
        }
        unsigned char mask_key[4] = {0, 0, 0, 0};
        if (masked) {
            for (size_t i = 0; i < 4; ++i) {
                mask_key[i] = static_cast<unsigned char>(read_buffer_[cursor + i]);
            }
            cursor += 4;
        }

        if (payload_len > kMaxMessageBytes) {
            // Refuse absurd frames instead of exhausting memory.
            close();
            return -1;
        }

        if (ensure_buffered(cursor + static_cast<size_t>(payload_len), remaining) != 1) {
            return socket_ < 0 ? -1 : 0;
        }

        std::string payload = read_buffer_.substr(cursor, static_cast<size_t>(payload_len));
        read_buffer_.erase(0, cursor + static_cast<size_t>(payload_len));
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                               mask_key[i % 4]);
            }
        }

        switch (opcode) {
            case 0x1:  // text
            case 0x2:  // binary (CDP only uses text, but treat it uniformly)
                if (fin) {
                    out = payload;
                    return 1;
                }
                fragmented_message = payload;
                fragmented = true;
                break;
            case 0x0:  // continuation
                if (!fragmented) {
                    close();
                    return -1;
                }
                fragmented_message += payload;
                if (fin) {
                    out = fragmented_message;
                    return 1;
                }
                break;
            case 0x8:  // close
                close();
                return -1;
            case 0x9:  // ping -> pong
                write_frame(0xA, payload);
                break;
            case 0xA:  // pong (ignore)
                break;
            default:
                break;
        }
    }
}

}  // namespace Browser
}  // namespace Jarvis