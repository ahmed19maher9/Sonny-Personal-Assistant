#include "ChromiumCdpSession.h"

#include "Logger.h"

#include <chrono>
#include <vector>

namespace Jarvis {
namespace Browser {

namespace {

// ws://127.0.0.1:9222/devtools/page/ABC123 -> host/port/path
bool split_ws_url(const std::string& url, std::string& host, int& port, std::string& path) {
    const std::string prefix = "ws://";
    if (url.rfind(prefix, 0) != 0) return false;
    std::string rest = url.substr(prefix.size());

    size_t path_start = rest.find('/');
    std::string authority = path_start == std::string::npos ? rest : rest.substr(0, path_start);
    path = path_start == std::string::npos ? "/" : rest.substr(path_start);

    size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        host = authority;
        port = 80;
    } else {
        host = authority.substr(0, colon);
        try {
            port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return false;
        }
    }
    if (host.empty()) return false;
    if (host == "localhost") host = "127.0.0.1";
    return true;
}

}  // anonymous namespace

CdpSession::CdpSession() = default;

CdpSession::~CdpSession() {
    close();
}

bool CdpSession::connect(const std::string& ws_url, int timeout_ms, std::string* error) {
    close();

    std::string host, path;
    int port = 0;
    if (!split_ws_url(ws_url, host, port, path)) {
        if (error) *error = "Malformed CDP websocket URL: " + ws_url;
        return false;
    }

    if (!socket_.connect(host, port, path, timeout_ms, error)) {
        return false;
    }

    running_ = true;
    reader_ = std::thread(&CdpSession::reader_loop, this);
    return true;
}

void CdpSession::close() {
    running_ = false;
    socket_.close();  // unblocks the reader thread's recv()

    if (reader_.joinable() && std::this_thread::get_id() != reader_.get_id()) {
        reader_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    handler_ = nullptr;
    cv_.notify_all();
}

void CdpSession::fail_all_pending(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : pending_) {
        if (entry.second && !entry.second->done) {
            entry.second->done = true;
            entry.second->error = reason;
        }
    }
    cv_.notify_all();
}

void CdpSession::reader_loop() {
    std::string message;
    while (running_) {
        int rc = socket_.receive(message, 250);
        if (rc == 0) continue;
        if (rc < 0) {
            last_error_ = "CDP websocket closed";
            fail_all_pending(last_error_);
            running_ = false;
            break;
        }

        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(message, nullptr, false);
        } catch (...) {
            continue;  // ignore anything that is not valid JSON
        }
        if (payload.is_discarded() || !payload.is_object()) continue;

        // Replies carry an id; events carry a method.
        if (payload.contains("id") && payload["id"].is_number_integer()) {
            const int id = payload["id"].get<int>();
            std::shared_ptr<PendingCall> target;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = pending_.find(id);
                if (it != pending_.end()) target = it->second;
            }
            if (target) {
                if (payload.contains("error")) {
                    target->error = payload["error"].dump();
                } else if (payload.contains("result")) {
                    target->result = payload["result"];
                } else {
                    target->result = nlohmann::json::object();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    target->done = true;
                }
                cv_.notify_all();
            }
            continue;
        }

        if (payload.contains("method") && payload["method"].is_string()) {
            const std::string method = payload["method"].get<std::string>();
            // Messages from flattened sessions name their session; ignore
            // events belonging to a different one.
            if (payload.contains("sessionId") && !session_id_.empty() &&
                payload["sessionId"].is_string() &&
                payload["sessionId"].get<std::string>() != session_id_) {
                continue;
            }
            nlohmann::json params = payload.contains("params") ? payload["params"]
                                                               : nlohmann::json::object();
            EventHandler handler_copy;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                event_counts_[method]++;
                last_events_[method] = params;
                handler_copy = handler_;
            }
            cv_.notify_all();
            if (handler_copy) {
                handler_copy(method, params);
            }
        }
    }
}
nlohmann::json CdpSession::call(const std::string& method, const nlohmann::json& params,
                                int timeout_ms) {
    nlohmann::json error_reply = {{"__error", std::string()}};

    if (!socket_.is_connected()) {
        error_reply["__error"] = "CDP session is not connected";
        return error_reply;
    }

    const int id = ++next_id_;
    auto pending = std::make_shared<PendingCall>();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id] = pending;
    }

    nlohmann::json envelope = {{"id", id}, {"method", method}, {"params", params}};
    if (!session_id_.empty()) {
        envelope["sessionId"] = session_id_;
    }

    std::string send_error;
    if (!socket_.send_text(envelope.dump(), &send_error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
        error_reply["__error"] = send_error.empty() ? "CDP send failed" : send_error;
        return error_reply;
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        cv_.wait_until(lock, deadline, [&] { return pending->done; });
        pending_.erase(id);

        if (!pending->done) {
            LOG_DEBUG_COMPONENT("Browser", "CDP call timed out: " + method);
            error_reply["__error"] = "CDP call timed out: " + method;
            return error_reply;
        }
    }

    if (!pending->error.is_null() && !pending->error.empty()) {
        error_reply["__error"] = "CDP " + method + " failed: " + pending->error.dump();
        return error_reply;
    }
    return pending->result;
}

bool CdpSession::wait_for_event(const std::string& method, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    long long baseline = event_counts_[method];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    return cv_.wait_until(lock, deadline, [&] {
        return event_counts_[method] > baseline || !running_;
    }) && event_counts_[method] > baseline;
}

long long CdpSession::event_count(const std::string& method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = event_counts_.find(method);
    return it == event_counts_.end() ? 0 : it->second;
}

bool CdpSession::has_event(const std::string& method) const {
    return event_count(method) > 0;
}

nlohmann::json CdpSession::last_event_params(const std::string& method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_events_.find(method);
    return it == last_events_.end() ? nlohmann::json::object() : it->second;
}

void CdpSession::set_event_handler(EventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

bool CdpSession::is_error(const nlohmann::json& reply) {
    return reply.is_object() && reply.contains("__error");
}

std::string CdpSession::error_text(const nlohmann::json& reply) {
    if (!is_error(reply)) return "";
    if (reply["__error"].is_string()) return reply["__error"].get<std::string>();
    return reply["__error"].dump();
}

}  // namespace Browser
}  // namespace Jarvis