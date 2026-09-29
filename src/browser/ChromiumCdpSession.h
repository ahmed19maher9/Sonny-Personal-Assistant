#pragma once

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

// nlohmann/json is vendored in the project (see AvatarOverlay.cpp) and is used
// here for the Chrome DevTools Protocol payloads.
#include "../resources/avatars/json.hpp"

#include "BrowserWebSocket.h"

namespace Jarvis {
namespace Browser {

// ---------------------------------------------------------------------------
// A Chrome DevTools Protocol session.
//
// CDP is a request/response protocol multiplexed with an asynchronous event
// stream over a single WebSocket: every command carries an incrementing "id"
// and every reply echoes it, while events arrive unsolicited with a "method".
// This class hides that detail behind:
//
//   call(method, params)        -> blocking, id-correlated, timeout guarded
//   wait_for_event(method, ms)  -> waits for navigation/network events
//   set_event_handler(...)      -> optional streaming hook
//
// The socket is also used for "flattened" sessions (Target.attachToTarget with
// flatten:true), in which case session_id() is attached to every outgoing
// message exactly like Playwright's CDPSession does.
// ---------------------------------------------------------------------------
class CdpSession {
public:
    using EventHandler = std::function<void(const std::string& method,
                                            const nlohmann::json& params)>;

    CdpSession();
    ~CdpSession();

    CdpSession(const CdpSession&) = delete;
    CdpSession& operator=(const CdpSession&) = delete;

    // ws_url looks like ws://127.0.0.1:9222/devtools/page/<targetId>
    bool connect(const std::string& ws_url, int timeout_ms = 10000,
                 std::string* error = nullptr);
    void close();
    bool is_connected() const { return socket_.is_connected(); }

    void set_session_id(const std::string& id) { session_id_ = id; }
    const std::string& session_id() const { return session_id_; }

    // Send a command and block until the matching reply arrives.
    // On failure the returned object has {"__error": "<message>"}.
    nlohmann::json call(const std::string& method,
                        const nlohmann::json& params = nlohmann::json::object(),
                        int timeout_ms = 20000);

    // Wait until `method` fires at least once more (used for load events etc).
    bool wait_for_event(const std::string& method, int timeout_ms);
    long long event_count(const std::string& method) const;
    bool has_event(const std::string& method) const;
    nlohmann::json last_event_params(const std::string& method) const;

    void set_event_handler(EventHandler handler);
    const std::string& last_error() const { return last_error_; }

    // True when a CDP call failed and the returned JSON carries "__error".
    static bool is_error(const nlohmann::json& reply);
    static std::string error_text(const nlohmann::json& reply);

private:
    struct PendingCall {
        bool done = false;
        nlohmann::json result;
        nlohmann::json error;
    };

    void reader_loop();
    void fail_all_pending(const std::string& reason);

    WebSocketClient socket_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<int> next_id_{0};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<int, std::shared_ptr<PendingCall>> pending_;
    std::map<std::string, long long> event_counts_;
    std::map<std::string, nlohmann::json> last_events_;
    EventHandler handler_;
    std::string session_id_;
    std::string last_error_;
};

}  // namespace Browser
}  // namespace Jarvis