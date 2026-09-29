#pragma once

#include <string>
#include <mutex>

namespace Jarvis {
namespace Browser {

// ---------------------------------------------------------------------------
// Minimal, dependency-free RFC 6455 WebSocket client (client side only).
//
// Sonny drives Chromium through the Chrome DevTools Protocol, which speaks
// WebSocket. Rather than pulling in a third-party WebSocket library (or a
// Node/Python sidecar), this class implements exactly the subset of the
// protocol CDP needs:
//
//   * HTTP/1.1 Upgrade handshake with Sec-WebSocket-Accept verification
//   * Client-side masking (mandatory for clients)
//   * Text frames, fragmented message reassembly, 64-bit payload lengths
//   * Transparent Ping/Pong handling and Close frame detection
//
// The header is deliberately free of <windows.h>/<winsock2.h>: the code base
// includes <windows.h> very widely, and <winsock2.h> must be included before
// it, so the socket handle is kept as an opaque integer here and the Winsock
// headers live only in the .cpp.
// ---------------------------------------------------------------------------
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Blocking TCP connect + HTTP Upgrade handshake.
    // Returns false on failure and fills *error with a human readable reason.
    bool connect(const std::string& host, int port, const std::string& path,
                 int timeout_ms = 10000, std::string* error = nullptr);

    void close();
    bool is_connected() const { return socket_ >= 0; }

    // Send one text frame. Thread safe (serialised internally).
    bool send_text(const std::string& payload, std::string* error = nullptr);

    // Receive one complete text message.
    //    1 = message received (out holds the UTF-8 payload)
    //    0 = timeout expired (no message)
    //   -1 = connection closed / protocol error
    int receive(std::string& out, int timeout_ms);

private:
    // 1 = ok, 0 = timeout, -1 = error/closed
    int ensure_buffered(size_t needed, int timeout_ms);
    int wait_readable(int timeout_ms) const;
    int wait_writable(int timeout_ms) const;
    bool send_all(const char* data, size_t len);
    bool write_frame(unsigned char opcode, const std::string& payload);
    bool handshake(const std::string& host, int port, const std::string& path,
                   int timeout_ms, std::string* error);

    long long socket_;
    std::string read_buffer_;  // bytes received but not yet consumed
    std::mutex write_mutex_;
};

}  // namespace Browser
}  // namespace Jarvis