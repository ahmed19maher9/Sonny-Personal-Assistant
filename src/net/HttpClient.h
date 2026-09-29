#pragma once
// ============================================================================
// HttpClient.h - Minimal WinHTTP client used by the vectors network.
//
// Why WinHTTP and not a new dependency: the target already links winhttp.lib
// and wininet.lib (see CMakeLists target_link_libraries), and the existing
// browser/text-to-speech code already speaks HTTP by hand. Keeping the shapes
// here to a single GET/POST/download surface means the peer protocol and the
// optional embedding-model fetch share one implementation.
//
// All strings are UTF-8. Non-2xx responses are reported through
// HttpResponse::status_code with success == false; transport failures set
// HttpResponse::error instead (never thrown).
// ============================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Jarvis {
namespace net {

struct HttpResponse {
    bool success = false;      // true only for a 2xx response
    int status_code = 0;       // 0 when the request never reached a server
    std::string body;          // UTF-8 response payload
    std::string error;         // Win32 error description on transport failure
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // GET / POST convenience wrappers. Return false on transport failure.
    bool get(const std::string& url, HttpResponse& out, int timeout_ms = 8000);
    bool post_json(const std::string& url, const std::string& json_body,
                   HttpResponse& out, int timeout_ms = 8000);

    // Generic request. `extra_headers` are appended verbatim ("Name: value").
    bool request(const std::string& method,
                 const std::string& url,
                 const std::string& body,
                 const std::string& content_type,
                 const std::vector<std::string>& extra_headers,
                 HttpResponse& out,
                 int timeout_ms = 8000);

    // Stream a URL straight to disk (used to fetch the optional embedding
    // model). Writes to `<dest_path>.part` first and renames on success so an
    // interrupted download never leaves a truncated model behind.
    // progress: optional (bytes_received, total_bytes_or_0) callback.
    bool download_to_file(const std::string& url,
                          const std::string& dest_path,
                          const std::function<void(uint64_t, uint64_t)>& progress = nullptr,
                          int timeout_ms = 60000);

private:
    void* session_;  // HINTERNET (WinHTTP session), opaque here
};

// True when `url` starts with http:// or https://.
bool is_http_url(const std::string& url);

}  // namespace net
}  // namespace Jarvis
