// ChromiumLauncher.cpp - see ChromiumLauncher.h for the design notes.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>

#include "ChromiumLauncher.h"
#include "Logger.h"
#include "BrowserUtil.h"

#include "../resources/avatars/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Jarvis {
namespace Browser {

// ------------------------------------------------------- discovery ---------

std::string ChromiumLauncher::detect_executable(const std::string& channel,
                                                std::string* detected_name) {
    struct Candidate {
        const char* label;
        std::string path;
    };

    const std::string pf = env_path("ProgramFiles");
    const std::string pf86 = env_path("ProgramFiles(x86)");
    const std::string local = env_path("LOCALAPPDATA");

    std::vector<Candidate> chrome_candidates;
    std::vector<Candidate> edge_candidates;

    // Registry first: it tracks non-default install locations.
    std::string chrome_reg = read_registry_string(
        HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe", "");
    if (chrome_reg.empty()) {
        chrome_reg = read_registry_string(
            HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe", "");
    }
    if (!chrome_reg.empty()) chrome_candidates.push_back({"registry", chrome_reg});

    std::string edge_reg = read_registry_string(
        HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", "");
    if (edge_reg.empty()) {
        edge_reg = read_registry_string(
            HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", "");
    }
    if (!edge_reg.empty()) edge_candidates.push_back({"registry", edge_reg});

    if (!pf.empty()) {
        chrome_candidates.push_back(
            {"Program Files", pf + "\\Google\\Chrome\\Application\\chrome.exe"});
        edge_candidates.push_back(
            {"Program Files", pf + "\\Microsoft\\Edge\\Application\\msedge.exe"});
    }
    if (!pf86.empty()) {
        chrome_candidates.push_back(
            {"Program Files (x86)", pf86 + "\\Google\\Chrome\\Application\\chrome.exe"});
        edge_candidates.push_back(
            {"Program Files (x86)", pf86 + "\\Microsoft\\Edge\\Application\\msedge.exe"});
    }
    if (!local.empty()) {
        chrome_candidates.push_back(
            {"LocalAppData", local + "\\Google\\Chrome\\Application\\chrome.exe"});
        edge_candidates.push_back(
            {"LocalAppData", local + "\\Microsoft\\Edge\\Application\\msedge.exe"});
    }

    auto first_existing = [&](std::vector<Candidate>& candidates) -> std::string {
        for (const auto& candidate : candidates) {
            if (file_exists(candidate.path)) {
                if (detected_name) {
                    *detected_name = browser_display_name(candidate.path) + " (" +
                                     std::string(candidate.label) + ")";
                }
                return candidate.path;
            }
        }
        return "";
    };

    const std::string requested = to_lower(channel.empty() ? "auto" : channel);
    if (requested == "chrome") return first_existing(chrome_candidates);
    if (requested == "msedge" || requested == "edge") return first_existing(edge_candidates);

    // auto: prefer Chrome (best CDP coverage), fall back to Edge (ships with
    // every supported Windows build).
    std::string found = first_existing(chrome_candidates);
    if (!found.empty()) return found;
    return first_existing(edge_candidates);
}

std::string ChromiumLauncher::browser_display_name(const std::string& exe_path) {
    std::string lower = to_lower(exe_path);
    if (lower.find("msedge") != std::string::npos) return "Microsoft Edge";
    if (lower.find("chrome") != std::string::npos) return "Google Chrome";
    if (lower.find("brave") != std::string::npos) return "Brave";
    if (lower.find("chromium") != std::string::npos) return "Chromium";
    return "Chromium browser";
}

std::string ChromiumLauncher::default_user_data_dir() {
    return appdata_dir() + "\\Sonny\\browser-profile";
}

std::string ChromiumLauncher::default_download_dir() {
    std::string profile = env_path("USERPROFILE");
    if (profile.empty()) return appdata_dir() + "\\Sonny\\downloads";
    return profile + "\\Downloads\\Sonny";
}

std::string ChromiumLauncher::default_screenshot_dir() {
    return appdata_dir() + "\\Sonny\\screenshots";
}
int ChromiumLauncher::find_free_port(int preferred) {
    ensure_winsock();
    int start = preferred > 0 ? preferred : 9333;
    for (int candidate = start; candidate < start + 64; ++candidate) {
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe == INVALID_SOCKET) return 0;
        sockaddr_in addr;
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(candidate));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        bool available = bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        closesocket(probe);
        if (available) return candidate;
    }
    return 0;
}

bool ChromiumLauncher::http_get(int port, const std::string& path, int timeout_ms,
                                std::string& body, std::string* error) {
    ensure_winsock();
    body.clear();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        if (error) *error = "socket() failed";
        return false;
    }

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool ok = false;
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
        request += "Connection: close\r\n\r\n";
        if (send(sock, request.data(), static_cast<int>(request.size()), 0) > 0) {
            std::string raw;
            char buffer[8192];
            int64_t deadline = now_ms() + timeout_ms;
            while (now_ms() < deadline) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(sock, &read_set);
                timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 100 * 1000;
                if (select(0, &read_set, nullptr, nullptr, &tv) <= 0) continue;
                int received = recv(sock, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                raw.append(buffer, static_cast<size_t>(received));
            }

            size_t header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string low = to_lower(raw);
                std::string status_line = raw.substr(0, raw.find("\r\n"));
                body = raw.substr(header_end + 4);

                // Stay defensive: decode chunked bodies if the endpoint ever
                // decides to use them.
                if (low.find("transfer-encoding: chunked") != std::string::npos) {
                    std::string decoded;
                    size_t cursor = 0;
                    while (cursor < body.size()) {
                        size_t line_end = body.find("\r\n", cursor);
                        if (line_end == std::string::npos) break;
                        int chunk_size = 0;
                        try {
                            chunk_size = std::stoi(body.substr(cursor, line_end - cursor), nullptr, 16);
                        } catch (...) {
                            break;
                        }
                        if (chunk_size <= 0) break;
                        size_t data_start = line_end + 2;
                        decoded += body.substr(data_start, static_cast<size_t>(chunk_size));
                        cursor = data_start + static_cast<size_t>(chunk_size) + 2;
                    }
                    body = decoded;
                }
                ok = status_line.find(" 200") != std::string::npos ||
                     status_line.find(" 204") != std::string::npos;
                if (!ok && error) *error = "HTTP " + status_line;
            } else if (error) {
                *error = "Malformed HTTP response from browser endpoint";
            }
        } else if (error) {
            *error = "Failed to send HTTP request to browser endpoint";
        }
    } else if (error) {
        *error = "Cannot connect to browser endpoint on port " + std::to_string(port);
    }

    closesocket(sock);
    return ok;
}

bool ChromiumLauncher::debug_endpoint_alive(int port) {
    if (port <= 0) return false;
    std::string body, error;
    if (!http_get(port, "/json/version", 1500, body, &error)) return false;
    return body.find("webSocketDebuggerUrl") != std::string::npos;
}

// ------------------------------------------- endpoint discovery ------------

std::string ChromiumLauncher::endpoint_record_path(const std::string& user_data_dir) {
    return user_data_dir + "\\sonny-browser-endpoint.json";
}

bool ChromiumLauncher::read_devtools_active_port(const std::string& user_data_dir,
                                                 int* port, std::string* ws_path) {
    if (port) *port = 0;
    if (user_data_dir.empty()) return false;

    std::ifstream file(user_data_dir + "\\DevToolsActivePort");
    if (!file.is_open()) return false;

    std::string first_line, second_line;
    std::getline(file, first_line);
    std::getline(file, second_line);
    first_line.erase(std::remove(first_line.begin(), first_line.end(), '\r'), first_line.end());
    second_line.erase(std::remove(second_line.begin(), second_line.end(), '\r'), second_line.end());
    if (first_line.empty()) return false;

    int discovered = 0;
    try {
        discovered = std::stoi(first_line);
    } catch (...) {
        return false;
    }
    if (discovered <= 0 || discovered > 65535) return false;
    if (port) *port = discovered;
    if (ws_path) *ws_path = second_line;
    return true;
}

bool ChromiumLauncher::read_endpoint_record(const std::string& user_data_dir, int* port) {
    if (port) *port = 0;
    if (user_data_dir.empty()) return false;

    std::ifstream file(endpoint_record_path(user_data_dir));
    if (!file.is_open()) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    const nlohmann::json record = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (record.is_discarded() || !record.is_object()) return false;

    const int recorded_port = record.value("port", 0);
    if (recorded_port <= 0 || recorded_port > 65535) return false;
    if (port) *port = recorded_port;
    return true;
}

void ChromiumLauncher::write_endpoint_record(const std::string& user_data_dir, int port,
                                             unsigned long pid, const std::string& browser) {
    if (user_data_dir.empty()) return;
    ensure_directory(user_data_dir);
    nlohmann::json record;
    record["port"] = port;
    record["pid"] = static_cast<unsigned long long>(pid);
    record["browser"] = browser;
    record["written_at"] = static_cast<long long>(now_ms());
    std::ofstream file(endpoint_record_path(user_data_dir), std::ios::trunc);
    if (file.is_open()) file << record.dump(2);
}

void ChromiumLauncher::remove_endpoint_record(const std::string& user_data_dir) {
    if (user_data_dir.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(endpoint_record_path(user_data_dir), ignored);
}

bool ChromiumLauncher::process_alive(unsigned long pid) {
    if (pid == 0) return false;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(pid));
    if (!handle) return false;
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(handle, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
}
// ------------------------------------------------------- lifetime ----------

bool ChromiumLauncher::start(const LaunchOptions& options, std::string* error) {
    stop(true);

    options_ = options;
    if (options_.user_data_dir.empty()) options_.user_data_dir = default_user_data_dir();
    if (options_.download_dir.empty()) options_.download_dir = default_download_dir();
    ensure_directory(options_.user_data_dir);
    ensure_directory(options_.download_dir);

    if (!options_.executable.empty() && file_exists(options_.executable)) {
        executable_ = options_.executable;
    } else {
        std::string detected_name;
        executable_ = detect_executable(options_.channel, &detected_name);
        if (executable_.empty()) {
            if (error) {
                *error = "No Chromium browser found. Install Google Chrome or Microsoft Edge, "
                         "or point browser_executable at chrome.exe / msedge.exe.";
            }
            return false;
        }
        LOG_INFO("Browser", "Detected " + detected_name + " at " + executable_);
    }

    // --- Attach path 1: a debugging endpoint is already listening. ---
    if (options_.debug_port > 0 && debug_endpoint_alive(options_.debug_port)) {
        return attach_to(options_.debug_port, error);
    }

    // --- Attach path 2: a previous Sonny run (possibly crashed) left a browser
    // holding this profile. Chrome refuses to open a profile twice, so instead
    // of failing the launch we adopt that browser - login state and tabs intact.
    if (options_.debug_port == 0) {
        int recorded = 0;
        const bool recorded_from_file =
            read_devtools_active_port(options_.user_data_dir, &recorded);
        const bool recorded_from_json =
            !recorded_from_file && read_endpoint_record(options_.user_data_dir, &recorded);
        if ((recorded_from_file || recorded_from_json) && debug_endpoint_alive(recorded)) {
            LOG_INFO("Browser", "Re-attaching to the browser already using this profile (port " +
                                    std::to_string(recorded) + ")");
            return attach_to(recorded, error);
        }
        if (recorded_from_json) {
            // Stale record from a browser that is gone: clear it so the launch
            // below starts from a clean slate.
            remove_endpoint_record(options_.user_data_dir);
        }
    }

    // Chrome publishes the port it picked in <user_data_dir>/DevToolsActivePort,
    // so ask for "any free port" (0) instead of probing for one ourselves: no
    // race with other processes grabbing the port first, and no retry ladder.
    port_ = options_.debug_port > 0 ? find_free_port(options_.debug_port) : 0;
    if (options_.debug_port > 0 && port_ == 0) {
        if (error) *error = "No free local port available for the DevTools endpoint";
        return false;
    }

    if (!launch_process(error)) return false;
    if (!wait_until_ready(30000, error)) return false;
    return true;
}

bool ChromiumLauncher::attach_to(int port, std::string* error) {
    if (port <= 0) {
        if (error) *error = "Invalid DevTools port " + std::to_string(port);
        return false;
    }

    port_ = port;
    attached_ = true;
    launched_ = false;
    ready_ = false;
    ZeroMemory(&process_, sizeof(process_));

    std::string body;
    http_get(port_, "/json/version", 3000, body, nullptr);
    const nlohmann::json version = nlohmann::json::parse(body, nullptr, false);
    if (!version.is_discarded() && version.is_object()) {
        browser_version_ = version.value("Browser", std::string());
        browser_ws_ = version.value("webSocketDebuggerUrl", std::string());
    }
    if (browser_version_.empty() && !debug_endpoint_alive(port_)) {
        port_ = 0;
        attached_ = false;
        if (error) *error = "No DevTools endpoint on port " + std::to_string(port);
        return false;
    }

    ready_ = true;
    // Keep the record current so the *next* Sonny run finds this browser again.
    if (!options_.user_data_dir.empty()) {
        write_endpoint_record(options_.user_data_dir, port_, 0, browser_version_);
    }
    LOG_INFO("Browser", "Attached to existing browser on port " + std::to_string(port_) +
                            (browser_version_.empty() ? "" : " (" + browser_version_ + ")"));
    return true;
}

bool ChromiumLauncher::launch_process(std::string* error) {
    std::vector<std::string> args;
    args.push_back(quote_arg(executable_));
    args.push_back("--remote-debugging-port=" + std::to_string(port_));
    // Required by Chrome >= 111 when a client sends an Origin header; harmless
    // otherwise and keeps the endpoint usable for other tooling.
    args.push_back("--remote-allow-origins=*");
    args.push_back("--user-data-dir=" + quote_arg(options_.user_data_dir));
    args.push_back("--no-first-run");
    args.push_back("--no-default-browser-check");
    args.push_back("--disable-session-crashed-bubble");
    args.push_back("--hide-crash-restore-bubble");
    args.push_back("--disable-popup-blocking");
    args.push_back("--disable-features=Translate,OptimizationHints,MediaRouter,ChromeWhatsNewUI");
    // Playwright's stability set: without these, background tabs get throttled
    // and long automation runs stall on timers.
    args.push_back("--disable-background-timer-throttling");
    args.push_back("--disable-backgrounding-occluded-windows");
    args.push_back("--disable-renderer-backgrounding");
    args.push_back("--disable-ipc-flooding-protection");
    args.push_back("--autoplay-policy=no-user-gesture-required");

    if (options_.headless) {
        args.push_back("--headless=new");
        args.push_back("--disable-gpu");
    } else {
        args.push_back("--window-size=" + std::to_string(options_.window_width) + "," +
                       std::to_string(options_.window_height));
        if (options_.start_maximized) args.push_back("--start-maximized");
    }

    for (const auto& extra : options_.extra_args) {
        args.push_back(extra);
    }
    args.push_back("about:blank");

    std::string command_line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) command_line += " ";
        command_line += args[i];
    }

    LOG_INFO("Browser", "Launching: " + command_line);

    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = options_.headless ? SW_HIDE : SW_SHOWNORMAL;

    ZeroMemory(&process_, sizeof(process_));
    if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr,
                        options_.user_data_dir.empty() ? nullptr : options_.user_data_dir.c_str(),
                        &startup, &process_)) {
        if (error) {
            *error = "Failed to launch " + browser_display_name(executable_) + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    launched_ = true;
    attached_ = false;
    return true;
}
bool ChromiumLauncher::wait_until_ready(int timeout_ms, std::string* error) {
    int64_t deadline = now_ms() + timeout_ms;
    bool port_resolved = port_ > 0;
    while (now_ms() < deadline) {
        // --remote-debugging-port=0: Chrome writes the chosen port to
        // DevToolsActivePort as soon as the endpoint is listening.
        if (!port_resolved) {
            int discovered = 0;
            if (read_devtools_active_port(options_.user_data_dir, &discovered) &&
                debug_endpoint_alive(discovered)) {
                port_ = discovered;
                port_resolved = true;
                LOG_DEBUG("Browser", "Chrome published DevTools port " + std::to_string(port_));
            }
        }

        if (port_resolved && debug_endpoint_alive(port_)) {
            std::string body;
            http_get(port_, "/json/version", 3000, body, nullptr);
            nlohmann::json version = nlohmann::json::parse(body, nullptr, false);
            if (!version.is_discarded() && version.is_object()) {
                browser_version_ = version.value("Browser", std::string());
                browser_ws_ = version.value("webSocketDebuggerUrl", std::string());
            }
            ready_ = true;
            write_endpoint_record(options_.user_data_dir, port_,
                                  process_.dwProcessId, browser_version_);
            LOG_INFO("Browser", "DevTools endpoint ready on port " + std::to_string(port_) +
                                    (browser_version_.empty() ? "" : " (" + browser_version_ + ")"));
            return true;
        }

        // Did the browser hand the launch off to an already running instance
        // that has no debugging port? That is the most common failure mode and
        // it deserves an actionable message.
        if (launched_ && process_.hProcess) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(process_.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                if (error) {
                    *error = browser_display_name(executable_) +
                             " exited immediately. It is probably already running with the same "
                             "profile without remote debugging enabled - close it, or give Sonny "
                             "its own browser_user_data_dir.";
                }
                return false;
            }
        }
        Sleep(250);
    }
    if (error) {
        *error = port_resolved
                     ? "Timed out waiting for the browser DevTools endpoint on port " +
                           std::to_string(port_)
                     : "Timed out waiting for the browser to publish its DevTools port";
    }
    return false;
}

void ChromiumLauncher::stop(bool force) {
    const bool was_attached = attached_;
    if (launched_ && process_.hProcess) {
        DWORD wait = WaitForSingleObject(process_.hProcess, force ? 1000 : 4000);
        if (wait == WAIT_TIMEOUT) {
            LOG_WARN("Browser", "Browser did not exit gracefully - terminating");
            TerminateProcess(process_.hProcess, 0);
            WaitForSingleObject(process_.hProcess, 2000);
        }
        CloseHandle(process_.hProcess);
        if (process_.hThread) CloseHandle(process_.hThread);
        // The browser we started is gone: drop the record so the next run
        // launches instead of probing a dead port.
        if (!options_.user_data_dir.empty()) {
            remove_endpoint_record(options_.user_data_dir);
        }
    }
    if (was_attached) {
        LOG_INFO("Browser", "Detached from external browser (left running)");
    }
    ZeroMemory(&process_, sizeof(process_));
    launched_ = false;
    attached_ = false;
    ready_ = false;
    port_ = 0;
    browser_version_.clear();
    browser_ws_.clear();
}

// ------------------------------------------------------- queries -----------

std::vector<TargetInfo> ChromiumLauncher::list_targets() const {
    std::vector<TargetInfo> targets;
    if (port_ <= 0) return targets;

    std::string body, error;
    if (!http_get(port_, "/json/list", 4000, body, &error)) {
        LOG_DEBUG_COMPONENT("Browser", "list_targets failed: " + error);
        return targets;
    }

    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return targets;

    for (const auto& item : parsed) {
        if (!item.is_object()) continue;
        TargetInfo target;
        target.id = item.value("id", std::string());
        target.type = item.value("type", std::string());
        target.title = item.value("title", std::string());
        target.url = item.value("url", std::string());
        target.ws_url = item.value("webSocketDebuggerUrl", std::string());
        targets.push_back(std::move(target));
    }

    // Deterministic order: pages first, then by title for stable references.
    std::sort(targets.begin(), targets.end(), [](const TargetInfo& a, const TargetInfo& b) {
        if (a.is_page() != b.is_page()) return a.is_page();
        return a.id < b.id;
    });
    return targets;
}

std::string ChromiumLauncher::debug_info() const {
    nlohmann::json info;
    info["ready"] = ready_;
    info["attached"] = attached_;
    info["launched"] = launched_;
    info["port"] = port_;
    info["executable"] = executable_;
    info["browser"] = browser_version_;
    info["user_data_dir"] = options_.user_data_dir;
    info["download_dir"] = options_.download_dir;
    info["headless"] = options_.headless;
    return info.dump();
}

}  // namespace Browser
}  // namespace Jarvis