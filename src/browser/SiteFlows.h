#pragma once

// ---------------------------------------------------------------------------
// SiteFlows - the "recipe" layer that turns a site + an intent into a verified
// browser sequence, on top of the single Chromium session owned by
// BrowserServiceImpl.
//
// Why this exists: Sonny previously had two unrelated web stacks - the CDP
// automation engine (interactive, own profile) and the ShellExecute search
// tools (default browser, dead end). The language model saw four overlapping
// tools, so the same spoken request could land in either browser and the user
// could never continue ("open the second one", "scroll down") afterwards.
//
// SiteFlows is the single execution path for "do something on a website":
// every recipe drives the automation browser, so the page is always
// interactive after the call, and signing in once covers every tool.
//
// Recipe design rules (this is what makes the flows survive site updates):
//
//   1. Prefer a navigable URL over simulating typing. YouTube and Google both
//      accept the query in the URL, which removes search-box markup, focus and
//      IME timing from the failure surface entirely.
//   2. Extract with several selector generations at once and merge, so a site
//      redesign that introduces ytd-rich-item-renderer does not blind us.
//   3. Verify: after every navigation re-read the page and report what was
//      actually found - never "it probably worked".
//   4. Handle consent interstitials explicitly; Google's consent wall is the
//      most common cause of "it opened but showed nothing".
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <json.hpp>

namespace Jarvis {

class BrowserServiceImpl;

namespace Flows {

using json = nlohmann::json;

// Process-wide browser service accessor. EVERYTHING that touches the web goes
// through this one instance, which is what guarantees a single Chromium
// session: one profile, one set of cookies, one set of tabs.
BrowserServiceImpl& shared_service();

// A scraped result row (video / web result).
struct ResultItem {
    std::string title;
    std::string url;
    std::string meta;   // channel / duration / snippet, best effort
    std::string kind;   // "video" | "web"
};

// Fuzzy relevance of `candidate` against a spoken `query`: token overlap with
// a substring bonus, one-edit tolerance (voice misspellings), and a length
// penalty so "Titanic official trailer" beats "Titanic 2 full reaction".
// Returns 0..1.
double compute_title_score(const std::string& query, const std::string& candidate);

// Best index in `items` for `query`, or -1 when nothing clears the floor.
int best_match(const std::string& query, const std::vector<ResultItem>& items,
               double* score_out = nullptr);

// Canonical site keys: "youtube" | "google" | "google_images" | "google_news"
// ("" when the site is not recognised).
std::string normalize_site(const std::string& site);

// True when the text is an address rather than a search phrase
// ("youtube.com", "https://x.dev/y" - not "cats playing piano").
bool looks_like_url(const std::string& text);

// ---------------------------------------------------------------------------
// SiteFlows - the recipe engine. One instance per browser service.
// ---------------------------------------------------------------------------
class SiteFlows {
public:
    explicit SiteFlows(BrowserServiceImpl& service);

    // Open a site's home page ("youtube", "google", ...). Throws on failure.
    json open_site(const std::string& site);

    // Run a query on a site and report the ranked results. Does NOT open a
    // result: the caller decides, so "search X" alone stays a search.
    json search(const std::string& site, const std::string& query,
                int timeout_ms = 12000);

    // Open the result best matching `title` (or the 1-based `index`) on the
    // current site. Returns needs_clarification instead of guessing.
    json open_result(const std::string& site, const std::string& title,
                     int index = 0);

    // Re-read the result rows of the active page.
    json results(const std::string& site, int limit = 20);

    // Media control on the active page: play, pause, togglemute, mute, unmute,
    // next, previous, fullscreen, exitfullscreen, skipad, restart,
    // seek_forward, seek_back.
    json control(const std::string& command, int amount = 0);

    // Scroll the active page: up | down | top | bottom | left | right.
    json scroll(const std::string& direction, int amount = 0);

    // Navigation history: back | forward | reload.
    json history(const std::string& action);

    // Site of the active tab ("youtube" / "google" / "" when unknown).
    std::string current_site();

    // Live URL of the active tab ("" when detached).
    std::string current_url();

// Compact page digest for the spoken confirmation.
    json describe();

private:
    std::vector<ResultItem> scrape_results(const std::string& site, int limit);
    std::vector<ResultItem> wait_for_results(const std::string& site,
                                             int timeout_ms, int limit);

    // Dismisses Google/YouTube consent interstitials when present.
    bool dismiss_consent();

    // Verifies a navigation actually landed where we expected.
    void assert_page(const std::string& expect_substring, const std::string& what);

    // Confirms a <video>/<audio> element is advancing; autoplay is commonly
    // blocked, so "play it" is verified rather than assumed.
    std::string ensure_playing();

    BrowserServiceImpl& service_;
};

// ---------------------------------------------------------------------------
// Free-function API - what the tools call. One shared engine, one browser.
// ---------------------------------------------------------------------------
SiteFlows& engine();

json open_site(const std::string& site);
json open_url(const std::string& url, bool new_tab = false);

// Search a site; optionally open the first result and start playback.
json search(const std::string& site, const std::string& query,
            bool open_first = false, bool play = false, int timeout_ms = 12000);

json open_result(const std::string& site, const std::string& title, int index = 0);
json results(const std::string& site, int limit = 0);
json control(const std::string& command, int amount = 0);
json scroll(const std::string& direction, int amount = 0);
json history(const std::string& action);
std::string current_site();
std::string current_url();
json describe();

}  // namespace Flows

}  // namespace Jarvis
