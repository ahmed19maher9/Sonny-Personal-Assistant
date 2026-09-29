#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace Jarvis {
namespace Browser {

// A DevTools target (tab / page / service worker / extension) as reported by
// the HTTP endpoint http://127.0.0.1:<port>/json/list.
struct TargetInfo {
    std::string id;
    std::string type;
    std::string title;
    std::string url;
    std::string ws_url;
    bool is_page() const { return type == "page"; }
};

// Everything needed to bring a Chromium instance up (or attach to one).
struct LaunchOptions {
    std::string executable;        // explicit chrome.exe/msedge.exe path ("" = auto-detect)
    std::string channel = "auto";  // "auto" | "chrome" | "msedge"
    std::string user_data_dir;     // persistent profile (logins survive restarts)
    std::string download_dir;      // where downloads land
    int debug_port = 0;            // 0 = pick a free port automatically
    bool headless = false;
    int window_width = 1440;
    int window_height = 900;
    bool start_maximized = true;
    std::vector<std::string> extra_args;
};

// ---------------------------------------------------------------------------
// Chromium process + endpoint management (the equivalent of Playwright's
// browserType.launch() and connectOverCDP()).
//
// The browser is *always* started with a Sonny-owned user-data-dir: Chrome
// 136+ refuses to enable --remote-debugging-port on the user's default profile,
// and a dedicated profile is what makes "log in once, stay logged in" work.
//
// Communication with the browser uses two channels:
//   * HTTP  (/json/version, /json/list)  -> discovery, version, target list
//   * WebSocket (CdpSession)             -> everything else
// Both are loopback-only; the HTTP helper talks raw sockets on purpose so a
// system proxy configuration can never break automation.
// ---------------------------------------------------------------------------
class ChromiumLauncher {
public:
    // ----- helpers (also used by the settings UI / diagnostics) -----
    static std::string detect_executable(const std::string& channel,
                                         std::string* detected_name = nullptr);
    static std::string browser_display_name(const std::string& exe_path);
    static int find_free_port(int preferred);
    static bool http_get(int port, const std::string& path, int timeout_ms,
                         std::string& body, std::string* error = nullptr);
    static bool debug_endpoint_alive(int port);

    static std::string default_user_data_dir();
    static std::string default_download_dir();
    static std::string default_screenshot_dir();

    // ----- endpoint discovery -------------------------------------------------
    // Chrome only writes <user_data_dir>/DevToolsActivePort when it picks the
    // debugging port itself (--remote-debugging-port=0). Sonny therefore asks
    // for port 0 and reads the port back from that file: no port-collision
    // retries, and no guessing which port a browser is listening on.
    // Line 1 is the port, line 2 the browser-level websocket path.
    static bool read_devtools_active_port(const std::string& user_data_dir, int* port,
                                          std::string* ws_path = nullptr);

    // Sonny's own record of the browser instance it started with this profile
    // (port + pid). Chrome refuses to open a profile twice, so when a previous
    // Sonny run left a browser behind, this record is what lets a new process
    // re-attach to it instead of failing to launch - the equivalent of
    // Playwright's chromium.connectOverCDP().
    static std::string endpoint_record_path(const std::string& user_data_dir);
    static bool read_endpoint_record(const std::string& user_data_dir, int* port);
    static void write_endpoint_record(const std::string& user_data_dir, int port,
                                      unsigned long pid, const std::string& browser);
    static void remove_endpoint_record(const std::string& user_data_dir);
    static bool process_alive(unsigned long pid);

    // ----- lifetime -----
    // Starts (or attaches to) the browser. Returns false with *error set on
    // failure; never throws.
    bool start(const LaunchOptions& options, std::string* error = nullptr);
    // Attaches to a browser whose DevTools endpoint is already listening; no
    // process is started and `stop()` will not kill it (it is not ours).
    bool attach_to(int port, std::string* error = nullptr);
    void stop(bool force = false);
    bool running() const { return ready_; }
    bool attached() const { return attached_; }
    int port() const { return port_; }
    const std::string& executable() const { return executable_; }
    const LaunchOptions& options() const { return options_; }

    // ----- queries -----
    std::string browser_version() const { return browser_version_; }
    std::string browser_ws_url() const { return browser_ws_; }
    std::vector<TargetInfo> list_targets() const;
    std::string debug_info() const;

private:
    bool launch_process(std::string* error);
    bool wait_until_ready(int timeout_ms, std::string* error);

    LaunchOptions options_;
    std::string executable_;
    std::string browser_version_;
    std::string browser_ws_;
    PROCESS_INFORMATION process_{};
    bool launched_ = false;
    bool attached_ = false;
    bool ready_ = false;
    int port_ = 0;
};

}  // namespace Browser
}  // namespace Jarvis