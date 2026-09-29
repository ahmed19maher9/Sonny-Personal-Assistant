#pragma once

// ---------------------------------------------------------------------------
// BrowserServiceImpl â€” the "Playwright-grade" orchestration layer for Sonny's
// Chromium automation, implemented natively in C++ on top of the Chrome DevTools
// Protocol (CDP).
//
// Playwright itself is a Node.js stack (server + ws + JS eval); Sonny's model is
// a single C++ executable, so the equivalent capability set is provided by:
//   * ChromiumLauncher    â€” launches/attaches Chromium with a Sonny-owned profile
//                           and --remote-debugging-port (CDP endpoint).
//   * ChromiumCdpSession  â€” per-page CDP websocket session: request/response
//                           multiplexing + event subscription.
//   * BrowserPageScripts  â€” injected JS runtime (locator engine, form model,
//                           trusted-input emulation helpers).
//   * BrowserServiceImpl  â€” THIS FILE: turns those primitives into Playwright
//                           style operations (auto-waiting navigation, batched
//                           actions, full-page screenshots, PDF printing, input
//                           emulation, dialogs, tabs) with crash backstops.
//
// All methods are thread-safe (one mutex per BrowserService; concurrent tool
// calls serialize instead of corrupting CDP state) and every failure throws
// std::runtime_error with a Playwright-style message, which BrowserTool converts
// into the JSON the LLM consumes.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <initializer_list>

#include <json.hpp>  // nlohmann (vendored in resources/avatars/, on the include path)

#include "Tool.h"              // ToolResult
#include "ChromiumLauncher.h"  // Browser::ChromiumLauncher
#include "ChromiumCdpSession.h"// Browser::CdpSession

namespace Jarvis {

using json = nlohmann::json;  // nlohmann alias used across the browser layer

class BrowserServiceImpl {
public:
    // `profile_dir_override` lets tests isolate state (prod passes "" and the
    // launcher default -- %APPDATA%\Sonny\browser-profile -- is used).
    explicit BrowserServiceImpl(const std::string& profile_dir_override = "");
    ~BrowserServiceImpl();

    BrowserServiceImpl(const BrowserServiceImpl&) = delete;
    BrowserServiceImpl& operator=(const BrowserServiceImpl&) = delete;

    // -- lifecycle ------------------------------------------------------------
    void startup();          // launches/attaches Chromium; throws on failure
    // Tears down the active page session; the browser process is left running
    // (the profile keeps cookies so "log in once" keeps working) unless
    // `close_browser` is true. Safe to call twice / never started.
    void shutdown(bool close_browser = false);

    // -- pages (tabs) ----------------------------------------------------------
    json list_pages();
    json open_page(const std::string& url, bool new_tab);
    json switch_page(const std::string& query, int index);
    json new_tab();
    json close_page(const std::string& target_id);

    // -- read ------------------------------------------------------------------
    json inspect_page(const std::string& kind, const std::string& selector, int limit);
    json find_elements(const std::string& target, int index, const std::string& filter,
                       bool get_all);
    json read_element(const std::string& target, int index);
    json summarize();
    json screenshot(const std::string& path, bool full_page);
    json print_pdf(const std::string& path);
    json eval_js(const std::string& expression);
    json form_fields(const std::string& scope);
    json wait_for(const std::string& target, int index, int timeout_ms);

    // -- act -------------------------------------------------------------------
    json trusted_click(const std::string& target, int index);
    json hover_element(const std::string& target, int index);
    json click_element(const std::string& target, int index, const std::string& filter);
    json fill_element(const std::string& target, const std::string& value, int index);
    json clear_field(const std::string& target, int index);
    json scroll_page(const std::string& direction, int amount);
    json batch_actions(const json& actions);
    json history_nav(const std::string& action);
    // Single trusted key press on the focused element (Enter submits, etc).
    json press_key_action(const std::string& key);

    // -- autofill --------------------------------------------------------------
    // Fills form controls from the user's profile store (UserProfileStore):
    // field classification runs in-page against the synonym table, values come
    // from the store (missing keys are reported back instead of guessed).
    json autofill();

    // -- shared string helper (used by anon-namespace error builders too) ------
    static const std::string& first_string_field(const json& obj,
                                                 std::initializer_list<const char*> keys,
                                                 const std::string& fallback = "");

private:
    // Page-scoped operation context: a connected CDP session + its target id.
    // shared_ptr so a mid-operation session swap (open_page new-tab) cannot
    // leave the lambda holding a dangling raw pointer. All page work must go
    // through with_page().
    struct LockedPage { std::shared_ptr<Browser::CdpSession> session; std::string target_id; };

    template <typename Fn>
    auto with_page(Fn fn) -> decltype(fn(std::declval<LockedPage&>()));

    // CDP wrappers.
    json cdp_eval(LockedPage& page, const std::string& expression);
    json cdp_eval_wrapped(LockedPage& page, const std::string& body_js);
    json cdp_call(LockedPage& page, const std::string& method,
                  const json& params, int timeout_ms = 15000);
    // CdpSession::call() returns the CDP *payload*, i.e. {"result": RemoteObject}
    // plus optional "exceptionDetails". This turns a page-side throw into a
    // C++ exception and returns the RemoteObject for further unwrapping.
    static json eval_remote_object(const json& payload);
    // Runs a wrapped Browser::Scripts body (an IIFE returning JSON) and raises
    // a Playwright-style error when the page-side `ok:false`.
    json call_runtime(LockedPage& page, const std::string& body_js);
    void dispatch_mouse(LockedPage& page, const std::string& type,
                        int x, int y, const std::string& button, int click_count);
    void dispatch_key(LockedPage& page, const std::string& type,
                      const std::string& code, int vk,
                      const std::string& text,
                      const std::vector<std::string>& modifiers);
    // Trusted keyboard input: resolves the KeySpec table, taps the focused
    // element (used by batch_actions steps and plain key presses).
    void press_key(LockedPage& page, const std::string& key,
                   const std::vector<std::string>& modifiers);

    // Runtime binding (the injected JS helper bundle).
    void bind_runtime(LockedPage& page);
    // Byte-stable synonym fingerprint -> marker attribute on <html> skips the
    // (sizeable) re-injection after every navigation.
    bool runtime_marker_present_locked(LockedPage& page);
    // readyState + network quiescence wait after navigations / actions.
    void wait_dom_settled(LockedPage& page, int timeout_ms);
    // Chrome only paints and lays out its *visible*, active tab: a minimized or
    // occluded window reports zero-size element rects (trusted clicks miss) and
    // stalls the compositor (screenshots time out). This activates the target
    // tab and restores a normal window state, i.e. Playwright's
    // page.bringToFront() with a window-state repair on top.
    void ensure_window_ready_locked(LockedPage& page);
    void navigate_page(LockedPage& page, const std::string& url,
                       const std::string& referrer = "");
    // Compact post-action digest (what the LLM sees after open/click/...).
    json digest_result(const std::string& url, const std::string& verb);

    // JS-dialog janitor (own thread: a call() from the reader thread would
    // deadlock on the reply).
    void dialog_loop();

    // -- small helpers ----------------------------------------------------------
    static std::string json_to_string(const json& value);
    static std::string arg_string(const json& args, const char* key,
                                  const std::string& fallback = "");
    static int arg_int(const json& args, const char* key, int fallback = 0);
    static bool arg_bool(const json& args, const char* key, bool fallback = false);

    // Attachment bookkeeping.
    bool ensure_attached_locked();           // call with mutex_ held
    void invalidate_page_locked(const std::string& reason);
    bool attach_to_locked(const std::string& target_id, std::string* error);

    // Element-action plumbing shared by trusted_click / batch step clicks.
    json trusted_click_core(LockedPage& page, const std::string& target, int index);

    // ------------------------------------------------------------------ state --
    std::recursive_mutex mutex_;             // serializes ALL public entry points
    Browser::ChromiumLauncher launcher_;
    std::string profile_dir_;                // %APPDATA%\Sonny\browser-profile (or override)
    std::string attached_target_id_;         // page currently bound to session_
    std::unique_ptr<Browser::CdpSession> session_;
    std::thread dialog_thread_;              // JS-dialog janitor (own thread: a
                                             // call() from the reader thread
                                             // would deadlock on the reply)
    std::atomic<bool> dialog_thread_running_{false};
    std::string runtime_signature_;          // synonym-table fingerprint
    std::string runtime_js_cache_;           // injected bundle for the signature
    bool startup_attempted_ = false;
};

}  // namespace Jarvis
