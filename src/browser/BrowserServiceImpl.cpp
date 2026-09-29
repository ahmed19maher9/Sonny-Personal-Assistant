#include "BrowserServiceImpl.h"

#include "BrowserCrypto.h"
#include "BrowserPageScripts.h"
#include "Logger.h"
#include "UserProfileStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

namespace Jarvis {

namespace Browser {
// Short alias: the service layer refers to the script builders as
// Browser::Scripts::* (the canonical namespace is BrowserPageScripts).
namespace Scripts = BrowserPageScripts;
}

// The browser layer's names (CdpSession, ChromiumLauncher, UserProfileStore,
// Scripts) are used pervasively below.
using namespace Browser;
using Browser::Scripts::json_string;

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using CdpSession = Browser::CdpSession;

constexpr int kDomSettleTimeoutMs = 8000;   // readyState wait after navigation
constexpr int kNavTimeoutMs = 30000;        // Page.navigate / load-event budget
constexpr int kCdpTimeoutMs = 15000;        // default CDP call budget
constexpr int kWaitPollMs = 250;            // wait_for poll interval

// <html> attribute stamped by install_runtime; proves the injected bundle is
// live and byte-current (see runtime_marker_present_locked).
constexpr char kMarkerAttr[] = "data-sonny-runtime";

// LockedPage holds a shared_ptr, but the service owns its session as a
// unique_ptr; alias without transferring ownership (the alias never outlives
// with_page's critical section).
std::shared_ptr<Browser::CdpSession> alias_session(
    const std::unique_ptr<Browser::CdpSession>& session) {
    return std::shared_ptr<Browser::CdpSession>(session.get(),
                                                [](Browser::CdpSession*) {});
}

// ---------------------------------------------------------------------------
// FNV-1a — fingerprints the synonym table so the page marker attribute can
// prove the injected runtime matches the current table without re-sending it.
// ---------------------------------------------------------------------------
std::string fnv1a_hex(const std::string& text) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

// JS string literal content (caller adds quotes). Escapes what would otherwise
// break out of a Runtime.evaluate expression.
std::string sanitize_js_string(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 16);
    char buffer[8];
    for (unsigned char c : raw) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string timestamp_for_file() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
    return buffer;
}

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A failed locator result carries `nearest`; surface the accessible names so
// the LLM can immediately retry with a label that actually exists.
[[noreturn]] void throw_action_error(const json& parsed) {
    std::string message = BrowserServiceImpl::first_string_field(parsed, {"error"},
                                                                "browser action failed");
    auto nearest = parsed.find("nearest");
    if (nearest != parsed.end() && nearest->is_array()) {
        std::string alternatives;
        for (const auto& element : *nearest) {
            if (!element.is_object()) continue;
            auto label = element.find("label");
            if (label == element.end() || !label->is_string()) continue;
            if (!alternatives.empty()) alternatives += ", ";
            alternatives += label->get<std::string>();
        }
        if (!alternatives.empty()) {
            if (alternatives.size() > 220) alternatives = alternatives.substr(0, 220);
            message += " | closest matches: " + alternatives;
        }
    }
    throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// Key metadata for Input.dispatchKeyEvent: (DOM code, virtual key code, text).
// Printable characters get text so the browser builds real keypress input.
// ---------------------------------------------------------------------------
struct KeySpec { std::string code; int vk; std::string text; };

bool lookup_special_key(const std::string& key, KeySpec& spec) {
    static const std::map<std::string, KeySpec> special = {
        {"enter", {"Enter", 13, "\r"}},      {"backspace", {"Backspace", 8, "\b"}},
        {"delete", {"Delete", 46, ""}},      {"escape", {"Escape", 27, ""}},
        {"esc", {"Escape", 27, ""}},         {"tab", {"Tab", 9, ""}},
        {"space", {" ", 32, " "}},           {"arrowup", {"ArrowUp", 38, ""}},
        {"arrowdown", {"ArrowDown", 40, ""}},{"arrowleft", {"ArrowLeft", 37, ""}},
        {"arrowright", {"ArrowRight", 39, ""}},{"pageup", {"PageUp", 33, ""}},
        {"pagedown", {"PageDown", 34, ""}},  {"home", {"Home", 36, ""}},
        {"end", {"End", 35, ""}},            {"insert", {"Insert", 45, ""}},
        {"f5", {"F5", 116, ""}},
    };
    auto it = special.find(lower_copy(key));
    if (it == special.end()) return false;
    spec = it->second;
    return true;
}

bool lookup_char_key(const std::string& key, KeySpec& spec) {
    if (key.size() != 1) return false;
    const char c = key[0];
    if (c >= 'a' && c <= 'z') {
        spec = {std::string("Key") + static_cast<char>(std::toupper(c)), c - 'a' + 65, key};
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        spec = {std::string("Key") + c, c - 'A' + 65, std::string(1, static_cast<char>(std::tolower(c)))};
        return true;
    }
    if (c >= '0' && c <= '9') {
        spec = {std::string("Digit") + c, c - '0' + 48, std::string(1, c)};
        return true;
    }
    return false;
}

// Caps summary payloads so tool results stay inside the LLM's comfortable
// context window (a raw Playwright snapshot is hundreds of KB; this is KBs).
void trim_summary(json& summary) {
    if (!summary.is_object()) return;
    auto cap_array = [&summary](const char* key, size_t limit) {
        auto it = summary.find(key);
        if (it != summary.end() && it->is_array() && it->size() > limit) {
            it->erase(it->begin() + static_cast<long>(limit), it->end());
        }
    };
    auto trim_text = [&summary](const char* key, size_t limit) {
        auto it = summary.find(key);
        if (it != summary.end() && it->is_string() && it->get<std::string>().size() > limit) {
            *it = it->get<std::string>().substr(0, limit);
        }
    };
    cap_array("headings", 10);
    cap_array("buttons", 10);
    cap_array("links", 12);
    cap_array("submitControls", 6);
    cap_array("fields", 14);
    trim_text("text", 1400);
    if (summary.contains("info") && summary["info"].is_object()) {
        summary["info"].erase("inputs");
        summary["info"].erase("images");
    }
}

void trim_form_fields(json& fields) {
    if (!fields.is_array()) return;
    if (fields.size() > 40) fields.erase(fields.begin(), fields.begin() + 40);
    for (auto& field : fields) {
        if (!field.is_object()) continue;
        auto options = field.find("options");
        if (options != field.end() && options->is_array() && options->size() > 8) {
            options->erase(options->begin(), options->begin() + 8);
        }
        if (field.contains("selector") && field["selector"].is_string() &&
            field["selector"].get<std::string>().size() > 160) {
            field["selector"] = field["selector"].get<std::string>().substr(0, 160);
        }
    }
}

// CDP targets -> compact refs used by switch_page matching.
struct PageRef { std::string id; std::string title; std::string url; };

std::vector<PageRef> page_refs(const std::vector<Browser::TargetInfo>& targets) {
    std::vector<PageRef> refs;
    for (const auto& target : targets) {
        if (!target.is_page()) continue;
        if (target.url.rfind("devtools://", 0) == 0) continue;
        refs.push_back({target.id, target.title, target.url});
    }
    return refs;
}

// The tabs payload shared by list_pages / new_tab: 1-based indices so the LLM
// can say "switch to tab 2" straight from this output.
json target_list_payload_impl(const std::vector<Browser::TargetInfo>& targets,
                              const std::string& attached_id) {
    json pages = json::array();
    int active = 0;
    int index = 0;
    for (const auto& ref : page_refs(targets)) {
        ++index;
        if (ref.id == attached_id) active = index;
        pages.push_back(json{{"index", index},
                             {"id", ref.id},
                             {"title", ref.title},
                             {"url", ref.url}});
    }
    json out{{"ok", true}, {"count", pages.size()}, {"pages", pages}};
    if (active > 0) out["active"] = active;
    return out;
}

}  // anonymous namespace
// ------------------------------------------------------------- statics -----
const std::string& BrowserServiceImpl::first_string_field(
    const json& obj, std::initializer_list<const char*> keys, const std::string& fallback) {
    if (obj.is_object()) {
        for (const char* key : keys) {
            auto it = obj.find(key);
            if (it != obj.end() && it->is_string()) return it->get_ref<const std::string&>();
        }
    }
    return fallback;
}

std::string BrowserServiceImpl::json_to_string(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_float()) return value.dump();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_null()) return "";
    return value.dump();
}

std::string BrowserServiceImpl::arg_string(const json& args, const char* key,
                                           const std::string& fallback) {
    if (!args.is_object()) return fallback;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    return json_to_string(*it);
}

int BrowserServiceImpl::arg_int(const json& args, const char* key, int fallback) {
    if (!args.is_object()) return fallback;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (it->is_number_integer()) return static_cast<int>(it->get<long long>());
    if (it->is_string()) {
        try { return std::stoi(it->get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

bool BrowserServiceImpl::arg_bool(const json& args, const char* key, bool fallback) {
    if (!args.is_object()) return fallback;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        const std::string text = lower_copy(it->get<std::string>());
        if (text == "true" || text == "yes" || text == "1") return true;
        if (text == "false" || text == "no" || text == "0") return false;
    }
    return fallback;
}

// ------------------------------------------------------------- lifecycle ---
BrowserServiceImpl::BrowserServiceImpl(const std::string& profile_dir_override)
    : profile_dir_(profile_dir_override.empty()
                       ? Browser::ChromiumLauncher::default_user_data_dir()
                       : profile_dir_override) {
    // Fingerprints the *entire* injected bundle (page script + synonym table),
    // not just the table: after a Sonny upgrade the tabs that still hold the
    // previous revision of the page API must be re-injected, and the <html>
    // marker stamped by install_runtime is what makes that decision.
    const std::string& synonyms = UserProfileStore::synonyms_json();
    runtime_signature_ = fnv1a_hex(synonyms + Browser::Scripts::runtime_js(synonyms));
}

BrowserServiceImpl::~BrowserServiceImpl() {
    try {
        shutdown(false);
    } catch (...) {
        // Never let a teardown hiccup escape a destructor.
    }
}

void BrowserServiceImpl::startup() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (launcher_.running()) return;

    Browser::LaunchOptions options;
    options.user_data_dir = profile_dir_;
    options.download_dir = Browser::ChromiumLauncher::default_download_dir();
    options.headless = false;
    // launch_process() already passes the prompt-free / anti-throttling set
    // (the Playwright defaults); anything added here is Sonny-specific.
    options.extra_args = {};
    std::string error;
    if (!launcher_.start(options, &error)) {
        throw std::runtime_error("Chromium launch failed: " + error);
    }
    LOG_INFO("Browser", "Chromium ready on CDP port " + std::to_string(launcher_.port()));
}

void BrowserServiceImpl::shutdown(bool close_browser) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    invalidate_page_locked("service shutdown");
    if (close_browser && launcher_.running()) {
        launcher_.stop(true);
        LOG_INFO("Browser", "Chromium closed");
    }
}
// ---------------------------------------------------------- attachment -----
void BrowserServiceImpl::invalidate_page_locked(const std::string& reason) {
    LOG_DEBUG("Browser", "Page session invalidated: " + reason);
    dialog_thread_running_ = false;
    if (dialog_thread_.joinable() &&
        dialog_thread_.get_id() != std::this_thread::get_id()) {
        dialog_thread_.join();
    }
    if (session_) session_->close();
    session_.reset();
    attached_target_id_.clear();
}

// The janitor runs on its own thread because a CdpSession::call() issued from
// the reader thread would deadlock: the reader must stay free to deliver the
// reply. Chrome never resumes JS execution until the dialog is answered, so
// this loop is what keeps alert()/confirm()/beforeunload from hanging every
// operation (the Playwright "auto-dismiss dialogs" behaviour).
void BrowserServiceImpl::dialog_loop() {
    CdpSession* session = session_.get();
    long long seen_dialogs = 0;
    int enable_heartbeat = 0;
    while (dialog_thread_running_.load() && session && session->is_connected()) {
        if (++enable_heartbeat % 40 == 0) {  // ~5 s: re-arm after edge cases
            session->call("Page.enable", json::object(), 1500);
        }
        const long long have = session->event_count("Page.javascriptDialogOpening");
        if (have > seen_dialogs) {
            seen_dialogs = have;
            json params = session->last_event_params("Page.javascriptDialogOpening");
            const std::string type = first_string_field(params, {"type"}, "alert");
            json response = json::object();
            // beforeunload must be accepted to actually leave the page;
            // alert/confirm are dismissed so the page behaves "as if no dialog".
            response["type"] = (type == "beforeunload") ? "accept" : "dismiss";
            const json reply = session->call("Page.handleJavaScriptDialog", response, 3000);
            if (CdpSession::is_error(reply)) {
                LOG_WARN("Browser", "Dialog dismiss failed: " + CdpSession::error_text(reply));
            } else {
                LOG_DEBUG("Browser", "Auto-dismissed JS dialog (" + type + ")");
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
}

bool BrowserServiceImpl::attach_to_locked(const std::string& target_id, std::string* error) {
    std::string ws_url;
    for (const auto& target : launcher_.list_targets()) {
        if (target.id == target_id) {
            ws_url = target.ws_url;
            break;
        }
    }
    if (ws_url.empty()) {
        if (error) *error = "browser target " + target_id + " no longer exists";
        return false;
    }

    auto fresh = std::make_unique<CdpSession>();
    if (!fresh->connect(ws_url, 8000, error)) return false;

    // Crash backstop: log loudly so a wedged tab is diagnosable from the log.
    fresh->set_event_handler([](const std::string& method, const json& params) {
        if (method == "Inspector.targetCrashed") {
            LOG_ERROR("Browser", "Page crashed: " + params.dump());
        } else if (method == "Inspector.detached") {
            LOG_WARN("Browser", "Inspector detached: " + params.dump());
        }
    });

    invalidate_page_locked("re-attaching to " + target_id);  // stops janitor + closes old
    session_ = std::move(fresh);
    attached_target_id_ = target_id;

    // Page domain: load events + JS dialog events. Runtime: script contexts.
    session_->call("Page.enable", json::object(), 4000);
    session_->call("Runtime.enable", json::object(), 4000);

    // Hide automation flags that YouTube uses to detect bot browsers.
    // navigator.webdriver is set to `true` by Chromium under CDP; YouTube's
    // player shows "Something went wrong, refresh or try again later" after a
    // delay when it detects automation. Override it before any page script runs.
    session_->call("Page.addScriptToEvaluateOnNewDocument",
                   json{{"source",
                         "try{Object.defineProperty(navigator,'webdriver',{get:function(){return undefined},configurable:true});"
                         "Object.defineProperty(navigator,'languages',{get:function(){return ['en-US','en']},configurable:true});"
                         "window.chrome={runtime:{}},"
                         "navigator.plugins=[];"
                         "navigator.mimeTypes=[];}catch(e){}"}}, 4000);

    dialog_thread_running_ = true;
    dialog_thread_ = std::thread([this]() { dialog_loop(); });

    // A reused browser may be minimized/backgrounded; make sure the tab we just
    // attached to is the one that is actually painted.
    {
        LockedPage page{alias_session(session_), attached_target_id_};
        ensure_window_ready_locked(page);
    }
    return true;
}

bool BrowserServiceImpl::ensure_attached_locked() {
    if (session_ && session_->is_connected() && !attached_target_id_.empty()) {
        // Cheap liveness probe: closed tabs stop answering evaluate.
        const json probe = session_->call(
            "Runtime.evaluate",
            json{{"expression", "1"}, {"returnByValue", true}}, 3000);
        if (!CdpSession::is_error(probe)) return true;
        invalidate_page_locked("page session is unresponsive");
    }

    if (!launcher_.running()) {
        try {
            startup();  // recursive mutex: safe from inside the lock
        } catch (const std::exception& error) {
            LOG_WARN("Browser", std::string("Browser auto-start failed: ") + error.what());
            return false;
        }
    }

    const auto refs = page_refs(launcher_.list_targets());
    const PageRef* chosen = nullptr;
    for (const auto& ref : refs) {
        if (ref.id == attached_target_id_) continue;  // we just proved it dead
        if (!chosen || ref.url == "about:blank") {
            chosen = &ref;
            if (chosen->url == "about:blank") break;  // a pristine tab wins
        }
    }
    if (!chosen) return false;  // no pages at all (rare: window closed)

    std::string error;
    if (!attach_to_locked(chosen->id, &error)) {
        LOG_WARN("Browser", "Attach failed: " + error);
        return false;
    }
    return true;
}
template <typename Fn>
auto BrowserServiceImpl::with_page(Fn fn) -> decltype(fn(std::declval<LockedPage&>())) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ensure_attached_locked()) {
        throw std::runtime_error(
            "no browser page is available (start failed or all tabs closed)");
    }
    LockedPage page{alias_session(session_), attached_target_id_};
    return fn(page);
}

// ------------------------------------------------------------ CDP helpers --
json BrowserServiceImpl::cdp_call(LockedPage& page, const std::string& method,
                                  const json& params, int timeout_ms) {
    const json reply = page.session->call(method, params, timeout_ms);
    if (CdpSession::is_error(reply)) {
        throw std::runtime_error(method + ": " + CdpSession::error_text(reply));
    }
    return reply;
}

json BrowserServiceImpl::cdp_eval(LockedPage& page, const std::string& expression) {
    const json reply = cdp_call(page, "Runtime.evaluate",
                                json{{"expression", expression},
                                     {"returnByValue", true},
                                     {"awaitPromise", true}},
                                20000);
    // Returns the RemoteObject ({type, value}); callers read .value("value").
    return eval_remote_object(reply);
}

// CdpSession::call() unwraps only the transport envelope, so `payload` is the
// CDP result object: {"result": RemoteObject, "exceptionDetails": {...}}.
json BrowserServiceImpl::eval_remote_object(const json& payload) {
    auto details = payload.find("exceptionDetails");
    if (details != payload.end() && !details->is_null() && !details->is_discarded()) {
        std::string message = first_string_field(*details, {"text"}, "JavaScript exception");
        if (details->contains("exception") && (*details)["exception"].is_object()) {
            const std::string description =
                first_string_field((*details)["exception"], {"description", "value"}, "");
            if (!description.empty()) message += ": " + description;
        }
        throw std::runtime_error(message);
    }
    if (!payload.is_object()) {
        throw std::runtime_error("unexpected DevTools payload for Runtime.evaluate");
    }
    return payload.value("result", json::object());
}

// Runs `body_js` (statements; must return a JSON string) inside the injected
// runtime wrapper and parses the envelope. See BrowserPageScripts::wrap().
json BrowserServiceImpl::cdp_eval_wrapped(LockedPage& page, const std::string& body_js) {
    bind_runtime(page);
    const std::string script =
        Browser::Scripts::wrap(body_js, UserProfileStore::synonyms_json());
    const json reply = cdp_call(page, "Runtime.evaluate",
                                json{{"expression", script}, {"returnByValue", true}},
                                30000);
    // RemoteObject: {type:"string", value:"<json envelope>"}
    const json remote = eval_remote_object(reply);
    const json value = remote.value("value", json());
    if (!value.is_string()) {
        LOG_WARN("Browser", "wrapped eval returned non-string: " + remote.dump().substr(0, 400));
        throw std::runtime_error("browser script returned no data (page may be unavailable)");
    }
    json parsed = json::parse(value.get<std::string>(), nullptr, false);
    if (parsed.is_discarded()) {
        throw std::runtime_error("browser script returned malformed JSON");
    }
    return parsed;
}

// Parses a wrapped result and unwraps `.data`, turning {ok:false} failures into
// exceptions that include the locator's nearest-match suggestions.
json BrowserServiceImpl::call_runtime(LockedPage& page, const std::string& body_js) {
    const json parsed = cdp_eval_wrapped(page, body_js);
    if (!parsed.is_object() || !parsed.value("ok", false)) {
        throw_action_error(parsed.is_object() ? parsed : json::object());
    }
    return parsed.value("data", json::object());
}
// ------------------------------------------------------------ injection ----
bool BrowserServiceImpl::runtime_marker_present_locked(LockedPage& page) {
    const json remote = cdp_eval(
        page,
        "(function(){var el=document.documentElement;"
        "return el&&el.getAttribute('" + std::string(kMarkerAttr) + "')||'';})()");
    if (remote.is_object()) {
        auto value = remote.find("value");
        if (value != remote.end() && value->is_string()) {
            return value->get<std::string>() == runtime_signature_;
        }
    }
    return false;
}

void BrowserServiceImpl::bind_runtime(LockedPage& page) {
    if (runtime_marker_present_locked(page)) return;
    cdp_eval(page, Browser::Scripts::install_runtime(UserProfileStore::synonyms_json(),
                                                     runtime_signature_));
    LOG_DEBUG("Browser", "Injected page runtime (signature " + runtime_signature_ + ")");
}

void BrowserServiceImpl::wait_dom_settled(LockedPage& page, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("page did not finish loading within " +
                                     std::to_string(timeout_ms / 1000) + "s");
        }
        try {
            const json remote = cdp_eval(page, "document.readyState");
            if (remote.is_object() && remote.value("value", std::string()) == "complete") {
                return;
            }
        } catch (const std::exception& error) {
            // Execution context destroyed mid-navigation (real page change);
            // keep polling until the new document is ready.
            LOG_DEBUG("Browser", std::string("settle poll: ") + error.what());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitPollMs));
    }
}

void BrowserServiceImpl::ensure_window_ready_locked(LockedPage& page) {
    // 1) Make this tab the active one (Playwright's page.bringToFront()).
    try {
        cdp_call(page, "Target.activateTarget", json{{"targetId", page.target_id}}, 5000);
    } catch (const std::exception& error) {
        LOG_DEBUG("Browser", std::string("activateTarget skipped: ") + error.what());
    }
    // 2) A minimized/maximized-to-nothing window is never painted, so element
    //    rects come back as 0x0 and the compositor refuses captures. Ask the
    //    browser to hand the window back in a normal state.
    try {
        const json window = cdp_call(page, "Browser.getWindowForTarget",
                                     json{{"targetId", page.target_id}}, 5000);
        const int window_id = window.value("windowId", 0);
        const std::string state =
            window.value("bounds", json::object()).value("windowState", std::string("normal"));
        if (window_id > 0 && state != "normal" && state != "fullscreen") {
            cdp_call(page, "Browser.setWindowBounds",
                     json{{"windowId", window_id},
                          {"bounds", json{{"windowState", "normal"}}}},
                     5000);
            LOG_DEBUG("Browser", "Restored window state to normal (was " + state + ")");
        }
    } catch (const std::exception& error) {
        LOG_DEBUG("Browser", std::string("window state check skipped: ") + error.what());
    }
}

void BrowserServiceImpl::navigate_page(LockedPage& page, const std::string& url,
                                       const std::string& referrer) {
    json params{{"url", url}, {"transitionType", "link"}};
    if (!referrer.empty()) params["referrer"] = referrer;
    cdp_call(page, "Page.navigate", params, kNavTimeoutMs);
    wait_dom_settled(page, kDomSettleTimeoutMs);
}

// -------------------------------------------------------------- digest -----
json BrowserServiceImpl::digest_result(const std::string& url, const std::string& verb) {
    json summary;
    with_page([&](LockedPage& page) {
        summary = call_runtime(page, Browser::Scripts::summary_body());
    });
    trim_summary(summary);
    return json{{"ok", true}, {"action", verb}, {"url", url}, {"page", summary}};
}

// -------------------------------------------------------------- open -------
json BrowserServiceImpl::open_page(const std::string& raw_url, bool new_tab) {
    if (raw_url.empty()) {
        throw std::runtime_error("no url given - say which site to open");
    }
    std::string url = raw_url;
    if (url.find("://") == std::string::npos) {
        url = "https://" + url;
    }

    if (!launcher_.running()) startup();

    std::string navigated_to;
    with_page([&](LockedPage& page) {
        if (!new_tab) {
            navigate_page(page, url);
            navigated_to = url;
            return;
        }
        // New-tab mode: create a blank target, attach to it, then navigate it.
        const json created = cdp_call(page, "Target.createTarget",
                                      json{{"url", "about:blank"}}, 10000);
        const std::string new_id = created.value("targetId", std::string());
        if (new_id.empty()) {
            throw std::runtime_error("failed to open a new tab");
        }
        std::string attach_error;
        if (!attach_to_locked(new_id, &attach_error)) {
            try {
                page.session->call("Target.closeTarget", json{{"targetId", new_id}}, 3000);
            } catch (...) {}
            throw std::runtime_error("opened a tab but could not control it: " + attach_error);
        }
        try {
            LockedPage fresh{alias_session(session_), new_id};
            navigate_page(fresh, url);
        } catch (...) {
            // Navigation failed: drop the empty tab we just created.
            try {
                session_->call("Target.closeTarget", json{{"targetId", new_id}}, 3000);
            } catch (...) {}
            throw;
        }
        navigated_to = url;
    });

    return digest_result(navigated_to, "opened");
}
// ---------------------------------------------------------------- tabs -----
json BrowserServiceImpl::list_pages() {
    if (!launcher_.running()) {
        return json{{"ok", true}, {"pages", json::array()}};
    }
    std::string attached;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        attached = attached_target_id_;
    }
    return target_list_payload_impl(launcher_.list_targets(), attached);
}

json BrowserServiceImpl::switch_page(const std::string& query, int index) {
    if (!launcher_.running()) {
        throw std::runtime_error("the browser is not running");
    }
    const auto refs = page_refs(launcher_.list_targets());
    if (refs.empty()) {
        throw std::runtime_error("no open browser pages");
    }

    const PageRef* match = nullptr;
    if (!query.empty()) {
        const std::string wanted = lower_copy(query);
        for (const auto& ref : refs) {
            if (lower_copy(ref.title).find(wanted) != std::string::npos ||
                lower_copy(ref.url).find(wanted) != std::string::npos) {
                match = &ref;
                break;
            }
        }
        if (!match) {
            std::string titles;
            for (size_t i = 0; i < refs.size() && i < 8; ++i) {
                if (!titles.empty()) titles += ", ";
                titles += refs[i].title.substr(0, 60);
            }
            throw std::runtime_error("no open page matches \"" + query +
                                     "\" | open pages: " + titles);
        }
    } else {
        const int wanted = index > 0 ? index - 1 : 0;
        if (wanted >= static_cast<int>(refs.size())) {
            throw std::runtime_error("only " + std::to_string(refs.size()) +
                                     " page(s) open - index " +
                                     std::to_string(index) + " does not exist");
        }
        match = &refs[static_cast<size_t>(wanted)];
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (attached_target_id_ != match->id) {
            std::string error;
            if (!attach_to_locked(match->id, &error)) {
                throw std::runtime_error("could not switch pages: " + error);
            }
        }
    }
    with_page([&](LockedPage& page) {
        cdp_call(page, "Page.bringToFront", json::object(), 3000);
    });
    return json{{"ok", true}, {"switched", true},
                {"title", match->title}, {"url", match->url}};
}

json BrowserServiceImpl::new_tab() {
    if (!launcher_.running()) startup();
    with_page([&](LockedPage& page) {
        const json created = cdp_call(page, "Target.createTarget",
                                      json{{"url", "about:blank"}}, 10000);
        const std::string new_id = created.value("targetId", std::string());
        if (new_id.empty()) {
            throw std::runtime_error("failed to open a new tab");
        }
        std::string error;
        if (!attach_to_locked(new_id, &error)) {
            throw std::runtime_error("opened a tab but could not control it: " + error);
        }
    });
    return json{{"ok", true}, {"opened", "new tab"},
                {"note", "ready - use browser_open to load a site in it"}};
}

json BrowserServiceImpl::close_page(const std::string& target_id) {
    if (!launcher_.running()) {
        throw std::runtime_error("the browser is not running");
    }

    std::string doomed = target_id;
    if (doomed.empty()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        doomed = attached_target_id_;
    }
    if (doomed.empty()) {
        const auto refs = page_refs(launcher_.list_targets());
        if (refs.empty()) throw std::runtime_error("no open browser pages");
        doomed = refs.front().id;
    }

    const auto refs = page_refs(launcher_.list_targets());

    // Closing the only page means closing the browser window itself.
    if (refs.size() <= 1) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        invalidate_page_locked("closing the last page");
        launcher_.stop(true);
        return json{{"ok", true}, {"closed", doomed},
                    {"note", "it was the only open page - browser closed"}};
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (attached_target_id_ == doomed) {
            invalidate_page_locked("closed the active page");
        }
        // Issue Target.closeTarget from any OTHER live page session. A browser
        // endpoint connection would also work, but a page proxy reuses the
        // attachment machinery we already trust.
        for (const auto& ref : refs) {
            if (ref.id == doomed) continue;
            if (!ensure_attached_locked()) break;
            if (attached_target_id_ == doomed) break;
            try {
                LockedPage proxy{alias_session(session_), attached_target_id_};
                cdp_call(proxy, "Target.closeTarget", json{{"targetId", doomed}}, 5000);
                break;
            } catch (const std::exception&) {
                continue;  // try the next page as the proxy
            }
        }
    }
    return json{{"ok", true}, {"closed", doomed}};
}
// ----------------------------------------------------------- inspection ----
json BrowserServiceImpl::inspect_page(const std::string& kind, const std::string& selector,
                                      int limit) {
    json data;
    with_page([&](LockedPage& page) {
        // Both arguments are interpolated into a JS expression, so they must be
        // emitted as JSON literals: an empty selector becomes `null` instead of
        // an empty expression ("API.extract(\"links\", , 40)" is a syntax error).
        data = call_runtime(page, Browser::Scripts::extract_body(
            json_string(kind.empty() ? std::string("text") : kind),
            selector.empty() ? std::string("null") : json_string(selector), limit));
    });
    return json{{"ok", true}, {"kind", kind}, {"data", data}};
}

json BrowserServiceImpl::find_elements(const std::string& target, int index,
                                       const std::string& filter, bool get_all) {
    (void)index;  // only meaningful for targeted operations (fill/click/read)
    if (target.empty()) {
        throw std::runtime_error(
            "no search text given - say what to look for, e.g. 'login button'");
    }
    json options = json::object();
    if (filter != "any") options["filter"] = filter;
    if (get_all) options["limit"] = 12;
    json matches;
    with_page([&](LockedPage& page) {
        matches = call_runtime(page, Browser::Scripts::find_body(
            json_string(target), options.empty() ? std::string("null") : options.dump()));
    });
    return json{{"ok", true}, {"matches", matches}};
}

json BrowserServiceImpl::read_element(const std::string& target, int index) {
    json data;
    with_page([&](LockedPage& page) {
        data = call_runtime(page, Browser::Scripts::read_body(
            json_string(target), std::to_string(index)));
    });
    return json{{"ok", true}, {"element", data}};
}

json BrowserServiceImpl::scroll_page(const std::string& direction, int amount) {
    std::string dir = lower_copy(direction);
    if (dir.empty()) dir = "down";
    json data;
    with_page([&](LockedPage& page) {
        data = call_runtime(page, Browser::Scripts::scroll_body(
            json_string(dir), std::to_string(amount)));
    });
    return json{{"ok", true}, {"scroll", data}};
}
// ------------------------------------------------------- trusted input -----
void BrowserServiceImpl::dispatch_mouse(LockedPage& page, const std::string& type,
                                        int x, int y, const std::string& button,
                                        int click_count) {
    json params{{"type", type}, {"x", x}, {"y", y}};
    if (!button.empty()) params["button"] = button;
    if (click_count > 0) params["clickCount"] = click_count;
    cdp_call(page, "Input.dispatchMouseEvent", params, 8000);
}

void BrowserServiceImpl::dispatch_key(LockedPage& page, const std::string& type,
                                      const std::string& code, int vk,
                                      const std::string& text,
                                      const std::vector<std::string>& modifiers) {
    json params{{"type", type},
                {"code", code},
                {"key", text.empty() ? code : text},
                {"windowsVirtualKeyCode", vk},
                {"nativeVirtualKeyCode", vk}};
    // Printable text only on the down transition; keyUp with text is rejected.
    if (!text.empty() && type == "keyDown") params["text"] = text;
    for (const std::string& modifier : modifiers) {
        if (modifier == "control") params["ctrlKey"] = true;
        else if (modifier == "alt") params["altKey"] = true;
        else if (modifier == "shift") params["shiftKey"] = true;
        else if (modifier == "meta") params["metaKey"] = true;
    }
    cdp_call(page, "Input.dispatchKeyEvent", params, 8000);
}

void BrowserServiceImpl::press_key(LockedPage& page, const std::string& key,
                                   const std::vector<std::string>& modifiers) {
    KeySpec spec;
    if (!lookup_special_key(key, spec) && !lookup_char_key(key, spec)) {
        throw std::runtime_error("unsupported key: \"" + key + "\"");
    }
    dispatch_key(page, "keyDown", spec.code, spec.vk, spec.text, modifiers);
    dispatch_key(page, "keyUp", spec.code, spec.vk, spec.text, modifiers);
}

// The strict click: refuses to act on ambiguous targets (>1 match), scrolls
// the element into view, then drives Chrome's own input pipeline so the page
// sees trusted mouse events at real coordinates.
json BrowserServiceImpl::trusted_click_core(LockedPage& page, const std::string& target,
                                            int index) {
    const json parsed = cdp_eval_wrapped(page, Browser::Scripts::prepare_body(
        json_string(target), std::to_string(index)));
    if (!parsed.is_object() || !parsed.value("ok", false)) {
        throw_action_error(parsed.is_object() ? parsed : json::object());
    }
    const int total = parsed.value("total", 0);
    if (total > 1) {
        throw std::runtime_error(
            "\"" + target + "\" matches " + std::to_string(total) +
            " elements - say which one (e.g. 'the second one'), or use browser_find to inspect them");
    }
    const json element = parsed.value("data", json::object());
    if (!element.is_object()) {
        throw std::runtime_error("could not determine click coordinates for \"" + target + "\"");
    }
    // describe() publishes the click point as cx/cy *next to* the rect (which
    // carries x/y/w/h). Accept a nested rect.cx/rect.cy too, so the layout of
    // the payload can evolve without breaking clicks.
    auto rect = element.find("rect");
    const bool nested = rect != element.end() && rect->is_object();
    int x = element.contains("cx") ? element.value("cx", -1)
                                   : (nested ? rect->value("cx", -1) : -1);
    int y = element.contains("cy") ? element.value("cy", -1)
                                   : (nested ? rect->value("cy", -1) : -1);
    if (x < 0 || y < 0) {
        // Trusted input needs usable geometry. A page throttled in a background
        // tab, mid-scroll, or covered by an overlay can report a rect the input
        // pipeline cannot use, so fall back to the page's own click pipeline -
        // the equivalent of Playwright's locator.click({ force: true }).
        LOG_WARN("Browser", "degenerate click rect for \"" + target + "\": " +
                                element.dump().substr(0, 300) +
                                " - using the JS click pipeline instead");
        return call_runtime(page, Browser::Scripts::click_body(
                                     json_string(target), std::to_string(index),
                                     std::string("null")));
    }
    dispatch_mouse(page, "mousePressed", x, y, "left", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    dispatch_mouse(page, "mouseReleased", x, y, "left", 1);
    return element;
}

json BrowserServiceImpl::trusted_click(const std::string& target, int index) {
    if (target.empty()) {
        throw std::runtime_error("no target given - say what to click, e.g. 'sign in'");
    }
    json element;
    with_page([&](LockedPage& page) {
        element = trusted_click_core(page, target, index);
    });
    // Give SPA route changes a beat, then digest the page for the LLM.
    try {
        with_page([&](LockedPage& page) { wait_dom_settled(page, 4000); });
    } catch (const std::exception&) {
        // Long-lived navigations are fine; the digest below reports either way.
    }
    return digest_result(first_string_field(element, {"label"}), "clicked");
}
// ------------------------------------------------------- element actions ---
// JS-pipeline click fallback for overlays/shadow hosts where the coordinates
// dispatch cannot land. Not strict: picks the best-ranked match.
json BrowserServiceImpl::click_element(const std::string& target, int index,
                                       const std::string& filter) {
    if (target.empty()) {
        throw std::runtime_error("no target given - say what to click, e.g. 'sign in'");
    }
    json element;
    with_page([&](LockedPage& page) {
        json opts = json::object();
        if (filter != "any") opts["filter"] = filter;
        element = call_runtime(page, Browser::Scripts::click_body(
            json_string(target), std::to_string(index),
            opts.empty() ? std::string("null") : opts.dump()));
    });
    try {
        with_page([&](LockedPage& page) { wait_dom_settled(page, 4000); });
    } catch (const std::exception&) {
    }
    return digest_result(first_string_field(element, {"label"}), "clicked");
}

json BrowserServiceImpl::hover_element(const std::string& target, int index) {
    if (target.empty()) {
        throw std::runtime_error("no target given - say what to hover, e.g. 'menu button'");
    }
    json element;
    with_page([&](LockedPage& page) {
        element = call_runtime(page, Browser::Scripts::hover_body(
            json_string(target), std::to_string(index)));
    });
    return digest_result(first_string_field(element, {"label"}), "hovered");
}

json BrowserServiceImpl::fill_element(const std::string& target, const std::string& value,
                                      int index) {
    if (target.empty()) {
        throw std::runtime_error("no target given - say which field to fill");
    }
    json data;
    with_page([&](LockedPage& page) {
        data = call_runtime(page, Browser::Scripts::fill_body(
            json_string(target), std::to_string(index), json_string(value)));
    });
    return json{{"ok", true}, {"filled", data}};
}

json BrowserServiceImpl::clear_field(const std::string& target, int index) {
    json data;
    with_page([&](LockedPage& page) {
        data = call_runtime(page, Browser::Scripts::fill_body(
            json_string(target), std::to_string(index), std::string("\"\"")));
    });
    return json{{"ok", true}, {"cleared", data}};
}

// Runs a sequence of steps against ONE page binding: between steps the DOM is
// given a chance to settle, so a step that triggers navigation (login submit)
// is absorbed before the next step runs on the fresh document.
json BrowserServiceImpl::batch_actions(const json& actions) {
    if (!actions.is_array() || actions.empty()) {
        throw std::runtime_error("actions must be a non-empty array of steps");
    }
    if (actions.size() > 10) {
        throw std::runtime_error("at most 10 actions per call - split the sequence");
    }
    json results = json::array();
    with_page([&](LockedPage& page) {
        int step = 0;
        for (const auto& action : actions) {
            ++step;
            if (!action.is_object()) {
                throw std::runtime_error("action " + std::to_string(step) +
                                         " is not an object");
            }
            const std::string type = lower_copy(first_string_field(action, {"type", "action"}));
            const std::string target = first_string_field(action, {"target"});
            const std::string value = first_string_field(action, {"value"});
            const std::string key = lower_copy(first_string_field(action, {"key"}));
            const int index = arg_int(action, "index", 0);

            json entry{{"step", step}, {"type", type}};
            try {
                if (type == "fill") {
                    entry["result"] = call_runtime(page, Browser::Scripts::fill_body(
                        json_string(target), std::to_string(index), json_string(value)));
                } else if (type == "click") {
                    entry["result"] = trusted_click_core(page, target, index);
                } else if (type == "key") {
                    std::vector<std::string> mods;
                    auto mods_it = action.find("modifiers");
                    if (mods_it != action.end() && mods_it->is_array()) {
                        for (const auto& modifier : *mods_it) {
                            if (modifier.is_string()) {
                                mods.push_back(lower_copy(modifier.get<std::string>()));
                            }
                        }
                    }
                    press_key(page, key.empty() ? std::string("Enter") : key, mods);
                    entry["result"] = json{{"pressed", key.empty() ? "Enter" : key}};
                } else if (type == "scroll") {
                    entry["result"] = call_runtime(page, Browser::Scripts::scroll_body(
                        json_string(target.empty() ? std::string("down") : target),
                        std::to_string(arg_int(action, "amount", 0))));
                } else if (type == "wait") {
                    const int ms = std::clamp(arg_int(action, "ms", 500), 0, 5000);
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                    entry["result"] = json{{"waited", ms}};
                } else if (type == "hover") {
                    entry["result"] = call_runtime(page, Browser::Scripts::hover_body(
                        json_string(target), std::to_string(index)));
                } else {
                    throw std::runtime_error(
                        "unknown action type \"" + type + "\" (use fill/click/key/scroll/wait/hover)");
                }
            } catch (const std::exception& error) {
                entry["error"] = error.what();
                results.push_back(entry);
                throw std::runtime_error("action step " + std::to_string(step) + " (" +
                                         type + ") failed: " + error.what());
            }
            try {
                wait_dom_settled(page, 5000);
            } catch (const std::exception& error) {
                entry["note"] = std::string("page still settling: ") + error.what();
            }
            results.push_back(entry);
        }
    });
    return json{{"ok", true}, {"results", results}};
}
// ------------------------------------------------------------- read: misc --
json BrowserServiceImpl::summarize() {
    json summary;
    std::string url;
    with_page([&](LockedPage& page) {
        summary = call_runtime(page, Browser::Scripts::summary_body());
        const json info = call_runtime(page, Browser::Scripts::extract_body(
            json_string("metadata"), "null", 1));
        url = first_string_field(info, {"url"});
    });
    trim_summary(summary);
    return json{{"ok", true}, {"url", url}, {"page", summary}};
}

json BrowserServiceImpl::screenshot(const std::string& raw_path, bool full_page) {
    if (!launcher_.running()) startup();

    // Default destination: the Sonny download folder with a timestamped name.
    std::string path = raw_path;
    if (path.empty()) {
        path = Browser::ChromiumLauncher::default_download_dir() + "\\sonny-" +
               timestamp_for_file() + ".png";
    }
    const bool jpeg = path.size() > 4 &&
                      (path.compare(path.size() - 4, 4, ".jpg") == 0 ||
                       path.compare(path.size() - 5, 5, ".jpeg") == 0);
    if (!jpeg && (path.size() <= 4 || path.compare(path.size() - 4, 4, ".png") != 0)) {
        path += ".png";
    }

    std::error_code fs_error;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), fs_error);

    std::string encoded;
    std::string note;
    with_page([&](LockedPage& page) {
        // A capture goes through the compositor of the *visible* tab, so an
        // occluded or backgrounded target can stall it. Bring the page to the
        // front first (Playwright's page.bringToFront()), then retry once
        // before surfacing a failure to the model.
        try {
            cdp_call(page, "Page.bringToFront", json::object(), 5000);
        } catch (const std::exception& error) {
            LOG_DEBUG("Browser", std::string("bringToFront skipped: ") + error.what());
        }
        auto capture = [&](bool beyond_viewport) {
            json params{{"format", jpeg ? "jpeg" : "png"}, {"fromSurface", true}};
            if (beyond_viewport) {
                // captureBeyondViewport (Chrome 90+) = Playwright's fullPage:true.
                params["captureBeyondViewport"] = true;
            }
            const json reply = cdp_call(page, "Page.captureScreenshot", params, 30000);
            return reply.value("data", std::string());
        };
        for (int attempt = 0; attempt < 2 && encoded.empty(); ++attempt) {
            try {
                encoded = capture(full_page);
            } catch (const std::exception& error) {
                if (attempt == 1) {
                    if (!full_page) throw;
                    // captureBeyondViewport is the usual culprit on a throttled
                    // tab: a visible-area shot is far better than no evidence.
                    note = std::string("full-page capture failed (") + error.what() +
                           "); saved the visible viewport instead";
                    LOG_WARN("Browser", note);
                    encoded = capture(false);
                    break;
                }
                LOG_DEBUG("Browser", std::string("screenshot retry after: ") + error.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    });
    if (encoded.empty()) {
        throw std::runtime_error("the browser returned an empty screenshot");
    }

    std::string bytes;
    if (!Browser::Crypto::base64_decode(encoded, bytes) || bytes.empty()) {
        throw std::runtime_error("screenshot payload could not be decoded");
    }
    std::ofstream file(target, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not write screenshot to " + path);
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();

    LOG_INFO("Browser", "Screenshot saved: " + path + " (" +
                            std::to_string(bytes.size()) + " bytes)");
    json out{{"ok", true}, {"saved", path}, {"bytes", bytes.size()}};
    if (!note.empty()) out["note"] = note;
    return out;
}

json BrowserServiceImpl::print_pdf(const std::string& raw_path) {
    if (!launcher_.running()) startup();

    std::string path = raw_path;
    if (path.empty()) {
        path = Browser::ChromiumLauncher::default_download_dir() + "\\sonny-" +
               timestamp_for_file() + ".pdf";
    }
    if (path.size() <= 4 || path.compare(path.size() - 4, 4, ".pdf") != 0) {
        path += ".pdf";
    }

    std::error_code fs_error;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), fs_error);

    std::string encoded;
    with_page([&](LockedPage& page) {
        // Letter-ish A4 defaults with backgrounds on, mirroring Playwright's
        // page.pdf() defaults as closely as CDP allows.
        const json reply = cdp_call(page, "Page.printToPDF",
                                    json{{"printBackground", true},
                                         {"paperWidth", 8.27},
                                         {"paperHeight", 11.69},
                                         {"marginTop", 0.4},
                                         {"marginBottom", 0.4},
                                         {"marginLeft", 0.4},
                                         {"marginRight", 0.4}},
                                    40000);
        encoded = reply.value("data", std::string());
    });
    if (encoded.empty()) {
        throw std::runtime_error("the browser returned an empty PDF (headless only in some builds)");
    }

    std::string bytes;
    if (!Browser::Crypto::base64_decode(encoded, bytes) || bytes.empty()) {
        throw std::runtime_error("pdf payload could not be decoded");
    }
    std::ofstream file(target, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not write pdf to " + path);
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();

    LOG_INFO("Browser", "PDF saved: " + path + " (" + std::to_string(bytes.size()) + " bytes)");
    return json{{"ok", true}, {"saved", path}, {"bytes", bytes.size()}};
}

json BrowserServiceImpl::eval_js(const std::string& expression) {
    if (expression.empty()) {
        throw std::runtime_error("no expression given");
    }
    json value;
    with_page([&](LockedPage& page) {
        value = cdp_eval(page, expression);
    });
    if (value.is_object() && value.contains("value")) value = value["value"];
    return json{{"ok", true}, {"value", value}};
}
json BrowserServiceImpl::form_fields(const std::string& scope) {
    json fields;
    with_page([&](LockedPage& page) {
        fields = call_runtime(page, Browser::Scripts::fields_body(
            scope.empty() ? std::string("null") : json_string(scope), false));
    });
    trim_form_fields(fields);
    return json{{"ok", true}, {"fields", fields}};
}

json BrowserServiceImpl::wait_for(const std::string& target, int index, int timeout_ms) {
    if (target.empty()) {
        throw std::runtime_error("no target given - say what to wait for, e.g. 'results table'");
    }
    const int budget = timeout_ms > 0 ? timeout_ms : 10000;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budget);
    std::string last_error;
    for (;;) {
        try {
            json found;
            const bool hit = with_page([&](LockedPage& page) -> bool {
                const json parsed = cdp_eval_wrapped(page, Browser::Scripts::prepare_body(
                    json_string(target), std::to_string(index)));
                if (parsed.is_object() && parsed.value("ok", false)) {
                    found = json{{"ok", true},
                                 {"element", parsed.value("data", json::object())}};
                    return true;
                }
                last_error = first_string_field(parsed, {"error"}, "not found yet");
                return false;
            });
            if (hit) return found;
        } catch (const std::exception& error) {
            // Execution-context teardowns during navigation are expected here.
            last_error = error.what();
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitPollMs));
    }
    throw std::runtime_error("\"" + target + "\" did not appear within " +
                             std::to_string(budget / 1000) + "s (" + last_error + ")");
}
json BrowserServiceImpl::history_nav(const std::string& action) {
    const std::string direction = lower_copy(action);
    json out{{"ok", true}};
    with_page([&](LockedPage& page) {
        if (direction == "reload" || direction == "refresh") {
            cdp_call(page, "Page.reload", json{{"ignoreCache", false}}, kNavTimeoutMs);
            try {
                wait_dom_settled(page, kDomSettleTimeoutMs);
            } catch (const std::exception&) {
            }
            out["reloaded"] = true;
        } else if (direction == "back" || direction == "forward") {
            const json history =
                cdp_call(page, "Page.getNavigationHistory", json::object(), 8000);
            const int current = history.value("currentIndex", 0);
            const json& entries = history.contains("entries") && history["entries"].is_array()
                                      ? history["entries"]
                                      : json::array();
            const int wanted = direction == "back" ? current - 1 : current + 1;
            if (wanted < 0 || wanted >= static_cast<int>(entries.size())) {
                throw std::runtime_error("no " + direction +
                                         " entry in this tab's history");
            }
            const size_t entry_index = static_cast<size_t>(wanted);
            const std::string entry_id = first_string_field(entries[entry_index], {"id"});
            const std::string entry_url = first_string_field(entries[entry_index], {"url"});
            cdp_call(page, "Page.navigateToHistoryEntry",
                     json{{"entryId", entry_id}}, kNavTimeoutMs);
            try {
                wait_dom_settled(page, kDomSettleTimeoutMs);
            } catch (const std::exception&) {
            }
            out["navigated"] = direction;
            out["url"] = entry_url;
        } else {
            throw std::runtime_error("unknown navigation \"" + action +
                                     "\" (use back, forward or reload)");
        }
        const json info = call_runtime(page, Browser::Scripts::extract_body(
            json_string("metadata"), "null", 1));
        out["title"] = first_string_field(info, {"title"});
        out["url"] = first_string_field(info, {"url"});
    });
    return out;
}

// ------------------------------------------------------------- autofill -----
// Fills every form control the page classifies against the user's profile
// store. Values the user never saved are reported as "missing" so the LLM can
// ask once and (via the profile tooling) remember the answer for next time.
json BrowserServiceImpl::autofill() {
    const auto values = UserProfileStore::getInstance().autofill_values();
    if (values.empty()) {
        throw std::runtime_error(
            "the profile store is empty - save your details first "
            "(name, email, phone, address...) and try again");
    }

    json payload = json::object();
    for (const auto& [key, value] : values) {
        payload[key] = value;
    }

    json data;
    with_page([&](LockedPage& page) {
        data = call_runtime(page, Browser::Scripts::autofill_body(payload.dump()));
    });

    json missing = json::array();
    for (const auto& [key, value] : values) {
        bool used = false;
        if (data.contains("fields") && data["fields"].is_array()) {
            for (const auto& field : data["fields"]) {
                if (field.value("key", std::string()) == key) {
                    used = true;
                    break;
                }
            }
        }
        if (!used) missing.push_back(key);
    }

    json out{{"ok", true},
             {"filled", data.value("filled", 0)},
             {"fields", data.value("fields", json::array())},
             {"skipped", data.value("skipped", json::array())},
             {"unused", std::move(missing)}};
    LOG_INFO("Browser", "Autofill applied " + std::to_string(out["filled"].get<int>()) +
                            " field(s) from the profile store");
    return out;
}

json BrowserServiceImpl::press_key_action(const std::string& key) {
    json out;
    with_page([&](LockedPage& page) {
        press_key(page, key.empty() ? std::string("Enter") : key, {});
        try {
            wait_dom_settled(page, 3000);
        } catch (const std::exception&) {
        }
        const json info = call_runtime(page, Browser::Scripts::extract_body(
            json_string("metadata"), "null", 1));
        out = json{{"ok", true}, {"pressed", key.empty() ? "Enter" : key},
                   {"title", first_string_field(info, {"title"})},
                   {"url", first_string_field(info, {"url"})}};
    });
    return out;
}
//__SONNY_IMPL_CHUNK10B__

}  // namespace Jarvis




