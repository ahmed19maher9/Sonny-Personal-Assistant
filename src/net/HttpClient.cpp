#include "HttpClient.h"
#include "Logger.h"

#include <windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>

#pragma comment(lib, "winhttp.lib")

namespace Jarvis {
namespace net {

namespace {

std::wstring http_widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], needed);
    return out;
}

std::string http_last_error(const char* stage) {
    DWORD code = GetLastError();
    std::string message(stage);
    message += " failed (win32=" + std::to_string(code) + ")";
    LPSTR buffer = nullptr;
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, (LPSTR)&buffer, 0, nullptr);
    if (length && buffer) {
        std::string detail(buffer, length);
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
            detail.pop_back();
        }
        if (!detail.empty()) message += ": " + detail;
        LocalFree(buffer);
    }
    return message;
}

// Minimal URL split. WinHttpCrackUrl handles the parsing; this only wraps the
// fixed-size buffers it needs.
bool http_crack_url(const std::string& url, std::wstring& host, std::wstring& path_and_query,
                    INTERNET_PORT& port, bool& secure) {
    std::wstring wide_url = http_widen(url);
    if (wide_url.empty()) return false;

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host_buffer[256] = {0};
    wchar_t path_buffer[4096] = {0};
    wchar_t extra_buffer[2048] = {0};
    parts.lpszHostName = host_buffer;
    parts.dwHostNameLength = (DWORD)(sizeof(host_buffer) / sizeof(host_buffer[0]));
    parts.lpszUrlPath = path_buffer;
    parts.dwUrlPathLength = (DWORD)(sizeof(path_buffer) / sizeof(path_buffer[0]));
    parts.lpszExtraInfo = extra_buffer;
    parts.dwExtraInfoLength = (DWORD)(sizeof(extra_buffer) / sizeof(extra_buffer[0]));

    if (!WinHttpCrackUrl(wide_url.c_str(), (DWORD)wide_url.size(), 0, &parts)) return false;

    host.assign(host_buffer);
    path_and_query.assign(path_buffer);
    path_and_query.append(extra_buffer);
    if (path_and_query.empty()) path_and_query = L"/";
    port = parts.nPort;
    secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

// Turns a possibly relative Location header into an absolute URL.
std::string http_absolutize(const std::string& location, const std::string& base_url) {
    if (location.empty()) return "";
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return location;

    // Copy scheme://host[:port] from the base URL.
    size_t scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos) return location;
    size_t authority_end = base_url.find('/', scheme_end + 3);
    std::string authority = authority_end == std::string::npos
                                ? base_url
                                : base_url.substr(0, authority_end);
    if (location[0] == '/') return authority + location;
    return authority + "/" + location;
}

bool http_is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string http_read_header(HINTERNET request, DWORD header_index) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, header_index, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return "";
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, header_index, WINHTTP_HEADER_NAME_BY_INDEX, &buffer[0], &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return "";
    }
    buffer.resize(wcslen(buffer.c_str()));
    if (buffer.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), (int)buffer.size(), nullptr, 0,
                                     nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), (int)buffer.size(), &out[0], needed, nullptr,
                        nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// One round trip. Returns false only on a transport failure (DNS, connect,
// TLS, send/receive). An HTTP 4xx/5xx is a *successful* round trip and is
// reported through `status`, not as an error.
//
// `sink` receives the response body; when it is null the body is streamed but
// discarded. Redirect responses are reported to the caller (status + location)
// instead of being followed here, so the caller can decide whether a body is
// still valid.
// ---------------------------------------------------------------------------
bool http_perform_once(HINTERNET session,
                       const std::string& method,
                       const std::string& url,
                       const std::string& body,
                       const std::string& content_type,
                       const std::vector<std::string>& extra_headers,
                       int timeout_ms,
                       std::ostream* sink,
                       uint64_t* bytes_written,
                       uint64_t* total_bytes,
                       const std::function<void(uint64_t, uint64_t)>* progress,
                       int& status,
                       std::string& location,
                       std::string& error) {
    status = 0;
    location.clear();
    error.clear();
    if (bytes_written) *bytes_written = 0;
    if (total_bytes) *total_bytes = 0;
    if (!session) {
        error = "no WinHTTP session";
        return false;
    }

    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!http_crack_url(url, host, path, port, secure)) {
        error = "invalid URL: " + url;
        return false;
    }

    // unique_ptr keeps the two handles balanced on every early return.
    using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
    HttpHandle connection(WinHttpConnect(session, host.c_str(), port, 0), &WinHttpCloseHandle);
    if (!connection) {
        error = http_last_error("WinHttpConnect");
        return false;
    }

    const std::wstring method_w = http_widen(method);
    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request(WinHttpOpenRequest(connection.get(), method_w.c_str(), path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags),
                       &WinHttpCloseHandle);
    if (!request) {
        error = http_last_error("WinHttpOpenRequest");
        return false;
    }

    if (timeout_ms > 0) {
        const int resolve_ms = timeout_ms < 5000 ? timeout_ms : 5000;
        WinHttpSetTimeouts(session, resolve_ms, timeout_ms, timeout_ms, timeout_ms);
    }

    std::wstring headers;
    if (!content_type.empty()) headers += L"Content-Type: " + http_widen(content_type) + L"\r\n";
    headers += L"Accept: */*\r\n";
    for (const std::string& extra : extra_headers) headers += http_widen(extra) + L"\r\n";
    WinHttpAddRequestHeaders(request.get(), headers.c_str(), (DWORD)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    const DWORD body_length = (DWORD)body.size();
    void* body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA
                                  : const_cast<void*>(static_cast<const void*>(body.data()));
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, body_ptr, body_length,
                            body_length, 0)) {
        error = http_last_error("WinHttpSendRequest");
        return false;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        error = http_last_error("WinHttpReceiveResponse");
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = http_last_error("WinHttpQueryHeaders");
        return false;
    }
    status = (int)status_code;

    if (http_is_redirect(status)) {
        location = http_read_header(request.get(), WINHTTP_QUERY_LOCATION);
        return true;
    }

    DWORD64 content_length = 0;
    DWORD length_size = sizeof(content_length);
    if (total_bytes &&
        WinHttpQueryHeaders(request.get(),
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        *total_bytes = (uint64_t)content_length;
    }

    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer, (DWORD)sizeof(buffer), &read)) {
            error = http_last_error("WinHttpReadData");
            return false;
        }
        if (read == 0) break;
        if (sink) sink->write(buffer, (std::streamsize)read);
        if (bytes_written) *bytes_written += read;
        if (progress && total_bytes) (*progress)(*bytes_written, *total_bytes);
    }
    return true;
}

}  // namespace

bool is_http_url(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

HttpClient::HttpClient() : session_(nullptr) {
    HINTERNET session = WinHttpOpen(L"Sonny/1.0 (vectors)",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // AUTOMATIC_PROXY needs Win8.1+; fall back to the pre-Win8 default.
        session = WinHttpOpen(L"Sonny/1.0 (vectors)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    session_ = session;
    if (session_) {
        WinHttpSetTimeouts(session_, 5000, 10000, 15000, 30000);
    }
}

HttpClient::~HttpClient() {
    if (session_) {
        WinHttpCloseHandle((HINTERNET)session_);
        session_ = nullptr;
    }
}

bool HttpClient::get(const std::string& url, HttpResponse& out, int timeout_ms) {
    return request("GET", url, "", "", {}, out, timeout_ms);
}

bool HttpClient::post_json(const std::string& url, const std::string& json_body, HttpResponse& out,
                           int timeout_ms) {
    return request("POST", url, json_body, "application/json", {}, out, timeout_ms);
}

bool HttpClient::request(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& content_type,
                         const std::vector<std::string>& extra_headers, HttpResponse& out,
                         int timeout_ms) {
    out = HttpResponse{};

    std::string current_url = url;
    std::string current_method = method;
    std::string current_body = body;

    // Manual redirect handling (capped): WinHTTP's default policy does not
    // follow redirects, and HuggingFace's /resolve/ endpoints always 302 to a
    // CDN host, so this path is load-bearing for the model fetch.
    for (int hop = 0; hop <= 4; ++hop) {
        int status = 0;
        std::string location;
        std::string error;
        std::ostringstream sink;
        if (!http_perform_once((HINTERNET)session_, current_method, current_url, current_body,
                               content_type, extra_headers, timeout_ms, &sink, nullptr, nullptr,
                               nullptr, status, location, error)) {
            out.error = error;
            return false;
        }

        if (http_is_redirect(status)) {
            std::string next = http_absolutize(location, current_url);
            if (next.empty()) {
                out.status_code = status;
                out.error = "redirect without Location header";
                return false;
            }
            if (status == 303) {  // 303 See Other: always continue with GET
                current_method = "GET";
                current_body.clear();
            }
            LOG_DEBUG_COMPONENT("Http",
                                "redirect " + std::to_string(status) + " -> " + next);
            current_url = next;
            continue;
        }

        out.status_code = status;
        out.body = sink.str();
        out.success = (status >= 200 && status < 300);
        if (!out.success) out.error = "HTTP " + std::to_string(status);
        return true;
    }

    out.error = "too many redirects";
    return false;
}

bool HttpClient::download_to_file(const std::string& url, const std::string& dest_path,
                                  const std::function<void(uint64_t, uint64_t)>& progress,
                                  int timeout_ms) {
    if (!is_http_url(url) || dest_path.empty()) return false;

    std::error_code ec;
    const std::filesystem::path dest(dest_path);
    if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path(), ec);

    std::filesystem::path temp = dest;
    temp += ".part";

    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOG_WARN("Vectors", "Cannot open download target for writing: " + temp.string());
        return false;
    }

    std::string current_url = url;
    bool written_ok = false;
    for (int hop = 0; hop <= 4; ++hop) {
        // A redirect may have been preceded by a partial body: restart the file
        // so a followed Location can never leave mixed content behind.
        file.seekp(0);
        std::filesystem::resize_file(temp, 0, ec);

        int status = 0;
        std::string location;
        std::string error;
        uint64_t written = 0;
        uint64_t total = 0;
        const std::function<void(uint64_t, uint64_t)>* callback = progress ? &progress : nullptr;

        if (!http_perform_once((HINTERNET)session_, "GET", current_url, "", "", {}, timeout_ms,
                               &file, &written, &total, callback, status, location, error)) {
            LOG_WARN("Vectors", "Download failed: " + error);
            file.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        if (http_is_redirect(status)) {
            std::string next = http_absolutize(location, current_url);
            if (next.empty()) {
                LOG_WARN("Vectors", "Download redirect had no Location header");
                file.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
            current_url = next;
            continue;
        }

        if (status < 200 || status >= 300) {
            LOG_WARN("Vectors", "Download returned HTTP " + std::to_string(status));
            file.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        written_ok = true;
        LOG_INFO("Vectors", "Downloaded " + std::to_string(written) + " bytes to " +
                                      temp.string());
        break;
    }

    file.close();
    if (!written_ok) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    if (!MoveFileExA(temp.string().c_str(), dest.string().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_WARN("Vectors", "Could not move downloaded file into place: " + dest.string());
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace net
}  // namespace Jarvis
