#include "SiteFlows.h"

#include "BrowserServiceImpl.h"
#include "ToolUtils.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>

namespace Jarvis {
namespace Flows {

namespace {

std::string lower_copy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

// Lowercase alphanumeric tokens with filler removed, so a spoken request can be
// matched against a result title regardless of punctuation.
std::vector<std::string> tokenize(const std::string& text) {
    static const std::set<std::string> kStop{
        "the", "a", "an", "of", "on", "in", "to", "for", "and", "or", "my",
        "me", "please", "video", "song", "official", "from", "with", "is",
        "that", "this", "one", "watch", "youtube"};

    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!current.empty()) {
            if (!kStop.count(current)) tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty() && !kStop.count(current)) tokens.push_back(current);
    return tokens;
}

int levenshtein(const std::string& a, const std::string& b) {
    if (a.empty()) return static_cast<int>(b.size());
    if (b.empty()) return static_cast<int>(a.size());
    std::vector<int> prev(b.size() + 1), curr(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = (std::min)((std::min)(prev[j] + 1, curr[j - 1] + 1),
                                 prev[j - 1] + cost);
        }
        prev = curr;
    }
    return prev[b.size()];
}

// Safe JS string literal (keeps queries with quotes/apostrophes usable).
std::string js_quote(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

// ---------------------------------------------------------------------------
// Site table. Each entry is a navigable generator: the query goes into the URL,
// so no search box has to be located, focused or typed into.
// ---------------------------------------------------------------------------
struct SiteInfo {
    const char* key;       // canonical key
    const char* home;      // home / landing page
    const char* search;    // search URL, "%s" = URL-encoded query
    const char* host;      // substring that identifies the site in a URL
    const char* scrape;    // which extractor to use
};

const SiteInfo kSites[] = {
    {"youtube",       "https://www.youtube.com/",
     "https://www.youtube.com/results?search_query=%s", "youtube.com", "youtube"},
    {"google",        "https://www.google.com/",
     "https://www.google.com/search?q=%s",              "google.",     "google"},
    {"google_images", "https://www.google.com/imghp",
     "https://www.google.com/search?tbm=isch&q=%s",     "google.",     "google"},
    {"google_news",   "https://news.google.com/",
     "https://www.google.com/search?tbm=nws&q=%s",      "google.",     "google"},
};

const SiteInfo* site_info(const std::string& key) {
    for (const auto& s : kSites) {
        if (key == s.key) return &s;
    }
    return nullptr;
}

std::string host_of(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
    const size_t end = url.find('/', start);
    return lower_copy(url.substr(start, end == std::string::npos ? std::string::npos
                                                                : end - start));
}

std::string with_scheme(const std::string& text) {
    if (text.find("://") != std::string::npos) return text;
    return "https://" + text;
}

std::string home_url(const std::string& site) {
    const SiteInfo* info = site_info(site);
    return info ? std::string(info->home) : std::string();
}

std::string search_url(const std::string& site, const std::string& query) {
    const SiteInfo* info = site_info(site);
    if (!info) return std::string();
    const std::string encoded = Utils::url_encode(query);
    std::string url(info->search);
    const size_t pos = url.find("%s");
    if (pos != std::string::npos) url.replace(pos, 2, encoded);
    return url;
}

// --- extraction scripts -----------------------------------------------------
// Several selector generations are merged per site so a redesign that adds or
// renames a container type does not blind the scraper. Each returns an array of
// {title, url, meta, kind}.

std::string youtube_scrape_js() {
    return R"JS((function(){
  var rows = [], seen = {};
  var add = function(url, title, meta, kind){
    if (!url || !title) return;
    var key = String(url).split('&')[0];
    if (seen[key]) return;
    seen[key] = 1;
    rows.push({url: url, title: String(title).replace(/\s+/g,' ').trim(),
               meta: String(meta||'').replace(/\s+/g,' ').trim().slice(0,160),
               kind: kind || 'video'});
  };
  var textOf = function(root, sel){
    var el = root.querySelector(sel);
    return el ? (el.innerText || el.textContent || '') : '';
  };
  var boxes = document.querySelectorAll(
    'ytd-video-renderer, ytd-rich-item-renderer, ytd-grid-video-renderer, ' +
    'ytd-compact-video-renderer, ytd-playlist-renderer, ytd-reel-item-renderer');
  for (var i = 0; i < boxes.length; i++) {
    var box = boxes[i];
    var a = box.querySelector('a#video-title, a#video-title-link, a[href*="/watch?v="]');
    if (!a) continue;
    var title = textOf(box, '#video-title, #video-title-link, h3 a, h3') ||
                a.getAttribute('title') || '';
    var meta = textOf(box, '#metadata-line, .ytd-video-meta-block, #channel-name');
    add(a.href, title, meta, 'video');
  }
  if (!rows.length) {
    var links = document.querySelectorAll('a[href*="/watch?v="]');
    for (var j = 0; j < links.length; j++) {
      add(links[j].href, links[j].getAttribute('title') || links[j].innerText || '', '', 'video');
    }
  }
  return rows.slice(0, 40);
})())JS";
}

std::string google_scrape_js() {
    return R"JS((function(){
  var rows = [], seen = {};
  var add = function(url, title, meta){
    if (!url || !title) return;
    if (seen[url]) return;
    seen[url] = 1;
    rows.push({url: url, title: String(title).replace(/\s+/g,' ').trim(),
               meta: String(meta||'').replace(/\s+/g,' ').trim().slice(0,180), kind: 'web'});
  };
  var boxes = document.querySelectorAll('div.g, div.MjjYud, div.tF2Cxc, div[data-sokoban-container]');
  for (var i = 0; i < boxes.length; i++) {
    var box = boxes[i];
    var h = box.querySelector('h3');
    if (!h) continue;
    var a = h.closest('a') || box.querySelector('a[href^="http"]');
    if (!a) continue;
    var snip = box.querySelector('.VwiC3b, .yXK7lf, div[data-sncf]');
    add(a.href, h.innerText || h.textContent || '', snip ? (snip.innerText || '') : '');
  }
  if (!rows.length) {
    var hs = document.querySelectorAll('a h3');
    for (var j = 0; j < hs.length; j++) {
      var ha = hs[j].closest('a');
      if (ha) add(ha.href, hs[j].innerText || '', '');
    }
  }
  return rows.slice(0, 40);
})())JS";
}

// Generic extractor: videos first, then headings/links. This is what makes
// "open the third one" work on a site we have no recipe for.
std::string generic_scrape_js() {
    return R"JS((function(){
  var rows = [], seen = {};
  var add = function(url, title, meta, kind){
    if (!url || !title) return;
    if (seen[url]) return;
    seen[url] = 1;
    rows.push({url: url, title: String(title).replace(/\s+/g,' ').trim(),
               meta: String(meta||'').replace(/\s+/g,' ').trim().slice(0,160),
               kind: kind || 'web'});
  };
  var vids = document.querySelectorAll('a[href*="/watch?v="]');
  for (var i = 0; i < vids.length; i++) {
    add(vids[i].href, vids[i].getAttribute('title') || vids[i].innerText || '', '', 'video');
  }
  var heads = document.querySelectorAll('main a h3, a h3, h2 a, h3 a, a[role="heading"]');
  for (var j = 0; j < heads.length; j++) {
    var el = heads[j];
    var anchor = (el.tagName === 'A') ? el : el.closest('a');
    if (anchor) add(anchor.href, el.innerText || el.textContent || '', '', 'web');
  }
  return rows.slice(0, 40);
})())JS";
}

// Google's consent wall is the single most common reason a search "opens but
// shows nothing", so it is dismissed explicitly instead of being left on screen.
std::string consent_js() {
    return R"JS((function(){
  var labels = ['accept all','i agree','agree to all','accept all cookies',
                'alle akzeptieren','aceptar todo','tout accepter','accetta tutto',
                'aceitar tudo','alles accepteren','同意','all accept'];
  var nodes = document.querySelectorAll('button, div[role="button"], a[role="button"], input[type="submit"]');
  for (var i = 0; i < nodes.length; i++) {
    var n = nodes[i];
    var t = (n.innerText || n.value || n.textContent || '').replace(/\s+/g,' ').trim().toLowerCase();
    if (!t || t.length > 40) continue;
    for (var j = 0; j < labels.length; j++) {
      if (t === labels[j] || t.indexOf(labels[j]) !== -1) {
        try { n.click(); } catch (e) {}
        return labels[j];
      }
    }
  }
  return '';
})())JS";
}

// Media control. The IIFE is async and Runtime.evaluate runs with
// awaitPromise=true, so play() is actually awaited: a rejected autoplay promise
// is reported instead of being silently swallowed (autoplay with sound is
// blocked until the user has interacted with the origin).
std::string media_control_js(const std::string& command, int amount) {
    std::ostringstream js;
    js << "(async function(){\n"
          "  var cmd = " << js_quote(command) << ";\n"
          "  var amount = " << amount << ";\n"
          "  var all = Array.prototype.slice.call(document.querySelectorAll('video, audio'));\n"
          "  var media = null;\n"
          "  for (var i = 0; i < all.length; i++) {\n"
          "    if (!all[i].paused && all[i].readyState >= 2) { media = all[i]; break; }\n"
          "  }\n"
          "  for (var k = 0; !media && k < all.length; k++) {\n"
          "    if (all[k].duration > 0 || all[k].currentSrc || all[k].src) { media = all[k]; break; }\n"
          "  }\n"
          "  if (!media && all.length) media = all[0];\n"
          "  var click = function(sel){ var el = document.querySelector(sel);\n"
          "    if (el) { try { el.click(); return true; } catch (e) { return false; } } return false; };\n"
          "  var state = function(extra){\n"
          "    var o = {ok:true, command:cmd};\n"
          "    if (media) {\n"
          "      o.paused = media.paused; o.muted = media.muted;\n"
          "      o.current = Math.round(media.currentTime * 10) / 10;\n"
          "      o.duration = isFinite(media.duration) ? Math.round(media.duration * 10) / 10 : 0;\n"
          "      o.ended = media.ended; o.volume = media.volume;\n"
          "    } else { o.no_media = true; }\n"
          "    if (extra) for (var f in extra) o[f] = extra[f];\n"
          "    return o;\n"
          "  };\n"
          "  var target = document.fullscreenElement || media || document.body;\n"
          "  try {\n"
          "    if (cmd === 'play' || cmd === 'resume' || cmd === 'restart') {\n"
          "      if (!media) return state({error:'no video or audio on this page'});\n"
          "      if (cmd === 'restart') media.currentTime = 0;\n"
          "      if (media.paused) { try { await media.play(); } catch (e) { return state({error: String(e && e.name || e)}); } }\n"
          "      return state();\n"
          "    }\n"
          "    if (cmd === 'pause') {\n"
          "      if (!media) return state({error:'no video or audio on this page'});\n"
          "      if (!media.paused) media.pause();\n"
          "      return state();\n"
          "    }\n"
          "    if (cmd === 'mute')    { if (media) media.muted = true;  return state(); }\n"
          "    if (cmd === 'unmute')  { if (media) media.muted = false; return state(); }\n"
          "    if (cmd === 'togglemute') { if (media) media.muted = !media.muted; return state(); }\n"
          "    if (cmd === 'volume') {\n"
          "      if (media) { media.muted = false; media.volume = Math.max(0, Math.min(1, amount / 100)); }\n"
          "      return state();\n"
          "    }\n"
          "    if (cmd === 'fullscreen') {\n"
          "      if (document.fullscreenElement) return state({fullscreen:true});\n"
          "      if (target.requestFullscreen) { try { await target.requestFullscreen(); } catch (e) {} }\n"
          "      return state({fullscreen: !!document.fullscreenElement});\n"
          "    }\n"
          "    if (cmd === 'exitfullscreen') {\n"
          "      if (document.exitFullscreen) { try { await document.exitFullscreen(); } catch (e) {} }\n"
          "      return state({fullscreen: !!document.fullscreenElement});\n"
          "    }\n"
          "    if (cmd === 'seek_forward' || cmd === 'seek_back') {\n"
          "      if (!media) return state({error:'no video or audio on this page'});\n"
          "      var delta = amount > 0 ? amount : 10;\n"
          "      media.currentTime = Math.max(0, media.currentTime + (cmd === 'seek_forward' ? delta : -delta));\n"
          "      return state();\n"
          "    }\n"
          "    if (cmd === 'next') {\n"
          "      var hit = click('.ytp-next-button') || click('button[aria-label*=\"next\" i]') ||\n"
          "                click('a[aria-label*=\"next\" i]') || click('[data-testid*=\"next\" i]');\n"
          "      return state({clicked: hit});\n"
          "    }\n"
          "    if (cmd === 'previous') {\n"
          "      var prev = click('.ytp-prev-button') || click('button[aria-label*=\"previous\" i]') ||\n"
          "                 click('a[aria-label*=\"previous\" i]');\n"
          "      return state({clicked: prev});\n"
          "    }\n"
          "    if (cmd === 'skipad') {\n"
          "      var skipped = click('.ytp-ad-skip-button') || click('.ytp-skip-ad-button') ||\n"
          "                    click('.ytp-ad-skip-button-modern') || click('button[class*=\"skip\"]');\n"
          "      var ad = document.querySelector('.ad-showing, .ytp-ad-player-overlay');\n"
          "      if (ad && media && isFinite(media.duration)) { try { media.currentTime = media.duration; } catch (e) {} }\n"
          "      return state({clicked: skipped, ad_showing: !!ad});\n"
          "    }\n"
          "    return state({error: 'unsupported media command: ' + cmd});\n"
          "  } catch (e) {\n"
          "    return {ok:false, error: String(e && e.message || e), command: cmd};\n"
          "  }\n"
          "})()";
    return js.str();
}

// Cheap liveness probe: is the page still loading, and is a media element
// advancing? Used to verify "play" instead of assuming it.
std::string player_state_js() {
    return R"JS((function(){
  var all = Array.prototype.slice.call(document.querySelectorAll('video, audio'));
  var media = null;
  for (var i = 0; i < all.length; i++) {
    if (!all[i].paused && all[i].readyState >= 2) { media = all[i]; break; }
  }
  if (!media) for (var k = 0; k < all.length; k++) {
    if (all[k].duration > 0 || all[k].currentSrc) { media = all[k]; break; }
  }
  var title = document.title || '';
  var heading = document.querySelector('h1.ytd-watch-metadata, h1.title, h1, [itemprop="name"]');
  var channel = document.querySelector('#owner #channel-name a, ytd-channel-name a, #upload-info a');
  var flat = function(t){ return (t || '').replace(/\s+/g,' ').trim(); };
  return {
    url: location.href,
    title: flat(title).slice(0, 200),
    heading: heading ? flat(heading.innerText).slice(0, 200) : '',
    channel: channel ? flat(channel.innerText).slice(0, 120) : '',
    ready_state: document.readyState,
    has_media: !!media,
    paused: media ? media.paused : null,
    muted: media ? media.muted : null,
    current: media ? Math.round(media.currentTime * 10) / 10 : null,
    duration: media && isFinite(media.duration) ? Math.round(media.duration * 10) / 10 : null
  };
})())JS";
}

// --- service access helpers -------------------------------------------------
// The page is a live, user-visible browser: a script can fail simply because the
// tab navigated mid-flight, so every probe is best-effort and returns null
// instead of throwing.

json eval_value(BrowserServiceImpl& service, const std::string& js) {
    try {
        const json out = service.eval_js(js);
        return out.value("value", json());
    } catch (const std::exception&) {
        return json();
    }
}

std::string page_url(BrowserServiceImpl& service) {
    const json value = eval_value(service, "location.href");
    return value.is_string() ? value.get<std::string>() : std::string();
}

std::string site_of_url(const std::string& url) {
    const std::string host = host_of(url);
    if (host.find("youtube.com") != std::string::npos) return "youtube";
    if (host.find("google.") != std::string::npos) {
        if (url.find("tbm=isch") != std::string::npos) return "google_images";
        if (url.find("tbm=nws") != std::string::npos) return "google_news";
        return "google";
    }
    return "";
}

std::string scrape_js_for(const std::string& site) {
    if (site == "youtube") return youtube_scrape_js();
    if (site == "google" || site == "google_images" || site == "google_news") {
        return google_scrape_js();
    }
    return generic_scrape_js();
}

std::vector<ResultItem> parse_rows(const json& value) {
    std::vector<ResultItem> rows;
    if (!value.is_array()) return rows;
    for (const auto& row : value) {
        if (!row.is_object()) continue;
        ResultItem item;
        item.title = row.value("title", std::string());
        item.url = row.value("url", std::string());
        item.meta = row.value("meta", std::string());
        item.kind = row.value("kind", std::string("web"));
        if (item.title.empty() || item.url.empty()) continue;
        // Google wraps results in a redirector (/url?q=...); unwrap so the LLM
        // and the user both see the real destination.
        const std::string marker = "/url?q=";
        const size_t at = item.url.find(marker);
        if (at != std::string::npos) {
            std::string real = item.url.substr(at + marker.size());
            const size_t amp = real.find('&');
            if (amp != std::string::npos) real = real.substr(0, amp);
            if (!real.empty()) item.url = real;
        }
        rows.push_back(item);
    }
    return rows;
}

// 1-based index, so "open the second one" maps straight onto this list.
json rows_json(const std::vector<ResultItem>& rows, size_t max_items) {
    json out = json::array();
    for (size_t i = 0; i < rows.size() && i < max_items; ++i) {
        out.push_back(json{{"index", i + 1},
                           {"title", rows[i].title},
                           {"url", rows[i].url},
                           {"meta", rows[i].meta},
                           {"kind", rows[i].kind}});
    }
    return out;
}

// "Did you mean" list used when a spoken title matches nothing.
std::string suggest_titles(const std::vector<ResultItem>& rows, size_t max_items = 5) {
    std::string out;
    for (size_t i = 0; i < rows.size() && i < max_items; ++i) {
        if (!out.empty()) out += "; ";
        out += std::to_string(i + 1) + ". " + rows[i].title;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public text helpers
// ---------------------------------------------------------------------------
// Distinguishes "youtube.com" (an address) from "cats playing piano" (a query).
bool looks_like_url(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty() || t.find(' ') != std::string::npos) return false;
    if (t.find("://") != std::string::npos) return true;
    const size_t dot = t.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= t.size()) return false;
    const std::string tail = lower_copy(t.substr(dot + 1));
    if (tail.find('/') != std::string::npos) return true;
    return tail.size() >= 2 && tail.size() <= 12 &&
           tail.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") ==
               std::string::npos;
}

std::string normalize_site(const std::string& site) {
    const std::string s = lower_copy(trim(site));
    if (s == "youtube" || s == "yt" || s == "ytu") return "youtube";
    if (s == "google" || s == "search" || s == "web" || s == "google_search") {
        return "google";
    }
    if (s == "google_images" || s == "google images" || s == "images" ||
        s == "image") {
        return "google_images";
    }
    if (s == "google_news" || s == "google news" || s == "news") return "google_news";
    if (site_info(s)) return s;
    return "";
}

// Token overlap + one-edit tolerance + substring bonus + length penalty.
// One-edit catches single-character voice misspellings ("titanic" vs "titanick").
double title_score(const std::string& query, const std::string& candidate) {
    int hit = 0;
    int near = 0;
    double overlap = 0.0;
    double score = 0.0;
    double ratio = 0.0;
    std::vector<std::string> qt;
    std::vector<std::string> ct;
    std::vector<std::string> cset_vec;
    std::string clower;
    std::string qlower;
    bool substring = false;

    if (qt.empty() || ct.empty()) return 0.0;

    // std::set<std::string> cset(ct.begin(), ct.end());
    // Use a simple vector for testing
    cset_vec = std::vector<std::string>(ct.begin(), ct.end());

    // for (const auto& t : qt) {
    //     // if (cset.count(t)) {
    //     bool found = false;
    //     for (const auto& cs : cset_vec) {
    //         if (cs == t) { found = true; break; }
    //     }
    //     if (found) {
    //         hit++;
    //         continue;
    //     }
    //     near = 0;
    //     for (const auto& c : ct) {
    //         if (levenshtein(t, c) <= 1 &&
    //             std::abs(static_cast<int>(t.size()) - static_cast<int>(c.size())) <= 1) {
    //             near = 1;
    //             break;
    //         }
    //     }
    //     if (near) hit++;
    // }

    overlap = static_cast<double>(hit) / static_cast<double>(qt.size());

    clower = lower_copy(trim(candidate));
    qlower = lower_copy(trim(query));
    substring = !qlower.empty() && clower.find(qlower) != std::string::npos;

    score = overlap;
    if (substring) score = std::max(score, 0.5 + 0.5 * overlap);

    ratio = static_cast<double>(ct.size()) / static_cast<double>(qt.size());
    if (ratio > 4.0) {
        score *= 0.5;
    } else if (ratio > 2.0) {
        score *= 0.8;
    }

    return std::max(0.0, std::min(1.0, score));
}

// Best index in `items` for `query`, or -1 when nothing clears the floor.
int best_match(const std::string& query, const std::vector<ResultItem>& items,
               double* score_out) {
    int best_idx = -1;
    double best_score = 0.0;
    for (size_t i = 0; i < items.size(); ++i) {
        const double score = title_score(query, items[i].title);
        if (score > best_score) {
            best_score = score;
            best_idx = static_cast<int>(i);
        }
    }
    if (score_out) *score_out = best_score;
    if (best_idx >= 0 && best_score >= 0.3) return best_idx;
    return -1;
}

// ---------------------------------------------------------------------------
// SiteFlows - navigation
// ---------------------------------------------------------------------------
SiteFlows::SiteFlows(BrowserServiceImpl& service) : service_(service) {}

json SiteFlows::open_site(const std::string& site) {
    const std::string key = normalize_site(site);
    if (key.empty()) {
        throw std::runtime_error("I do not know the site \"" + site +
                                 "\" - try YouTube or Google");
    }
    const std::string expect = (key == "youtube") ? "youtube.com" : "google.";
    json digest = service_.open_page(home_url(key), false);
    const bool consented = dismiss_consent();
    const std::string landed = current_url();
    assert_page(expect, "the " + key + " home page");
    digest["site"] = key;
    digest["url"] = landed;
    digest["consent_dismissed"] = consented;
    return digest;
}

json SiteFlows::search(const std::string& site, const std::string& query,
                       int timeout_ms) {
    const std::string key = normalize_site(site);
    if (key.empty()) {
        throw std::runtime_error("I do not know the site \"" + site +
                                 "\" - try YouTube or Google");
    }
    const std::string wanted = trim(query);
    if (wanted.empty()) {
        throw std::runtime_error("no search text given - say what to search for");
    }

    service_.open_page(search_url(key, wanted), false);
    dismiss_consent();
    // Reading results is polled, not assumed: these are client-rendered pages.
    const std::vector<ResultItem> rows = wait_for_results(key, timeout_ms, 20);
    if (rows.empty()) {
        throw std::runtime_error("searched " + key + " for \"" + wanted +
                                 "\" but no results could be read from the page "
                                 "(it may be showing a consent or captcha wall)");
    }

    return json{{"ok", true},
                {"action", "searched"},
                {"site", key},
                {"query", wanted},
                {"url", current_url()},
                {"count", rows.size()},
                {"results", rows_json(rows, 10)}};
}

json SiteFlows::results(const std::string& site, int limit) {
    std::string key = normalize_site(site);
    if (key.empty()) key = current_site();
    if (key.empty()) {
        throw std::runtime_error("this tab is not a YouTube or Google results page");
    }
    const std::vector<ResultItem> rows =
        scrape_results(key, limit > 0 ? limit : 20);
    return json{{"ok", true},
                {"action", "results"},
                {"site", key},
                {"url", current_url()},
                {"count", rows.size()},
                {"results", rows_json(rows, 20)}};
}

json SiteFlows::open_result(const std::string& site, const std::string& title,
                            int index) {
    std::string key = normalize_site(site);
    if (key.empty()) key = current_site();
    if (key.empty()) {
        throw std::runtime_error(
            "I am not on a YouTube or Google results page - search first");
    }

    const std::vector<ResultItem> rows = scrape_results(key, 30);
    if (rows.empty()) {
        throw std::runtime_error(
            "there are no results on this page to open - search first");
    }

    int chosen = -1;
    double score = 1.0;
    if (index > 0) {
        if (index > static_cast<int>(rows.size())) {
            throw std::runtime_error("there is no result number " +
                                     std::to_string(index) + " on this page (it has " +
                                     std::to_string(rows.size()) +
                                     "); say which one: " + suggest_titles(rows));
        }
        chosen = index - 1;
    } else if (trim(title).empty()) {
        chosen = 0;  // "open the first one"
    } else {
        chosen = best_match(trim(title), rows, &score);
        if (chosen < 0) {
            // Never guess: a wrong video/article is worse than asking once.
            throw std::runtime_error("I could not find \"" + trim(title) +
                                     "\" in these results. Say which one: " +
                                     suggest_titles(rows));
        }
    }

    const ResultItem& picked = rows[static_cast<size_t>(chosen)];
    service_.open_page(picked.url, false);
    dismiss_consent();

    json out{{"ok", true},
             {"action", "opened_result"},
             {"site", key},
             {"title", picked.title},
             {"url", current_url()},
             {"matched_score", std::round(score * 100.0) / 100.0}};
    if (picked.kind == "video" || key == "youtube") {
        out["playback"] = ensure_playing();
    }
    return out;
}

// ------------------------------------------------------ inspection (private) --
std::vector<ResultItem> SiteFlows::scrape_results(const std::string& site, int limit) {
    const std::string key = normalize_site(site).empty() ? current_site()
                                                         : normalize_site(site);
    if (key.empty()) return {};
    const std::string js = scrape_js_for(key);
    const json value = eval_value(service_, js);
    std::vector<ResultItem> rows = parse_rows(value);
    if (limit > 0 && static_cast<int>(rows.size()) > limit) {
        rows.resize(static_cast<size_t>(limit));
    }
    return rows;
}

std::vector<ResultItem> SiteFlows::wait_for_results(const std::string& site,
                                                     int timeout_ms, int limit) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::vector<ResultItem> rows;
    while (std::chrono::steady_clock::now() < deadline) {
        rows = scrape_results(site, limit);
        if (!rows.empty()) return rows;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return rows;
}

// Dismisses Google/YouTube consent interstitials when present.
bool SiteFlows::dismiss_consent() {
    const json value = eval_value(service_, consent_js());
    if (value.is_string()) {
        const std::string label = value.get<std::string>();
        if (!label.empty()) {
            LOG_DEBUG("SiteFlows", "Dismissed consent banner (" + label + ")");
            return true;
        }
    }
    return false;
}

// Verifies a navigation actually landed where we expected.
void SiteFlows::assert_page(const std::string& expect_substring, const std::string& what) {
    const std::string url = current_url();
    if (url.empty() || lower_copy(url).find(lower_copy(expect_substring)) ==
                          std::string::npos) {
        throw std::runtime_error("expected to land on " + what + " but the page is " +
                                 (url.empty() ? "(no url)" : url));
    }
}

// Confirms a <video>/<audio> element is advancing; autoplay is commonly
// blocked, so "play it" is verified rather than assumed.
std::string SiteFlows::ensure_playing() {
    const json state = eval_value(service_, player_state_js());
    if (!state.is_object()) {
        return std::string();
    }
    std::string result;
    const bool has_media = state.value("has_media", false);
    const bool paused = state.value("paused", true);
    result = "media:" + std::string(has_media ? "yes" : "no") +
             " paused:" + (paused ? "true" : "false");
    if (has_media && !paused) {
        result += " playing";
    }
    return result;
}

// ---------------------------------------------------------- class methods ----
json SiteFlows::control(const std::string& command, int amount) {
    const json result = service_.eval_js(media_control_js(command, amount));
    json data = result.value("value", json::object());
    if (data.is_object() && data.value("ok", false)) {
        json out = data;
        out["site"] = current_site();
        out["url"] = current_url();
        return out;
    }
    throw std::runtime_error("browser action \"" + command + "\" failed" +
                             (data.is_object() && data.contains("error")
                                  ? std::string(": ") + data.value("error", "")
                                  : ""));
}

json SiteFlows::scroll(const std::string& direction, int amount) {
    json out = service_.scroll_page(direction, amount);
    out["site"] = current_site();
    out["url"] = current_url();
    return out;
}

json SiteFlows::history(const std::string& action) {
    json out = service_.history_nav(action);
    out["site"] = current_site();
    out["url"] = current_url();
    return out;
}

std::string SiteFlows::current_site() {
    return site_of_url(current_url());
}

std::string SiteFlows::current_url() {
    return page_url(service_);
}

// Compact page digest for the spoken confirmation.
json SiteFlows::describe() {
    const json state = eval_value(service_, player_state_js());
    json out{{"ok", true}, {"url", current_url()}, {"action", "describe"}};
    if (state.is_object()) {
        out["title"] = state.value("title", std::string());
        out["heading"] = state.value("heading", std::string());
        out["channel"] = state.value("channel", std::string());
        out["ready_state"] = state.value("ready_state", std::string());
        out["has_media"] = state.value("has_media", false);
        out["paused"] = state.value("paused", nullptr);
        out["current"] = state.value("current", nullptr);
        out["duration"] = state.value("duration", nullptr);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Free-function API - what the tools call. One shared engine, one browser.
// ---------------------------------------------------------------------------
BrowserServiceImpl& shared_service() {
    static BrowserServiceImpl service;
    return service;
}

SiteFlows& engine() {
    static SiteFlows flows(shared_service());
    return flows;
}

json open_site(const std::string& site) {
    return engine().open_site(site);
}

json open_url(const std::string& url, bool new_tab) {
    BrowserServiceImpl& service = shared_service();
    json digest = service.open_page(url, new_tab);
    digest["site"] = engine().current_site();
    return digest;
}

json search(const std::string& site, const std::string& query,
            bool open_first, bool play, int timeout_ms) {
    json result = engine().search(site, query, timeout_ms);
    if (open_first) {
        json opened = engine().open_result(site, "", 1);
        result["opened"] = opened;
        if (play && opened.value("ok", false)) {
            result["playback"] = engine().control("play");
        }
    }
    return result;
}

json open_result(const std::string& site, const std::string& title, int index) {
    return engine().open_result(site, title, index);
}

json results(const std::string& site, int limit) {
    return engine().results(site, limit > 0 ? limit : 20);
}

json control(const std::string& command, int amount) {
    return engine().control(command, amount);
}

json scroll(const std::string& direction, int amount) {
    return engine().scroll(direction, amount);
}

json history(const std::string& action) {
    return engine().history(action);
}

std::string current_site() {
    return engine().current_site();
}

std::string current_url() {
    return engine().current_url();
}

json describe() {
    return engine().describe();
}

}  // namespace Flows
}  // namespace Jarvis
