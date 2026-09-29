#pragma once

#include <string>

namespace Jarvis {
namespace Browser {

// ---------------------------------------------------------------------------
// The JavaScript runtime that Sonny injects into the page before every action.
//
// Playwright gets its robustness from a set of locator "engines" (css, xpath,
// text, role, label, placeholder, alt, title) plus a two-phase action model
// (resolve element -> dispatch real input events). This file provides the same
// behaviour for Sonny, in one self-contained script:
//
//   * findAll(target, opts)  - resilient element resolution with ranking
//   * describe(el)           - element metadata (rect, accessible name, path)
//   * formFields(scope)      - form model with canonical field classification
//   * setValue / setChecked / selectOption / submitControls
//   * summary()              - compact page digest for the language model
//   * extract*()             - text / links / tables / forms / metadata / html
//
// Two properties matter for correctness:
//
//  1. The script is wrapped in an IIFE that returns a fresh API object, so it
//     can be sent with EVERY action (no stale state when a SPA re-renders or
//     the page navigates) and nothing leaks into the page's global scope.
//     Chrome remains same-origin/anti-fingerprinting clean: it is a plain
//     Runtime.evaluate, exactly what DevTools' own console does.
//
//  2. Field classification is driven by the synonym table passed in from C++,
//     so the profile store is the single source of truth for autofill keys.
// ---------------------------------------------------------------------------
namespace BrowserPageScripts {

// The runtime library with the synonym table compiled in.
// `synonyms_json` is a JSON object: canonical_key -> [synonym, ...].
const std::string& runtime_js(const std::string& synonyms_json);

// Wrap an action body into a complete, exception-safe page expression.
// The body runs with `API` in scope and should `return JSON.stringify(...)`.
// `synonyms_json` is the live synonym table injected into the runtime.
std::string wrap(const std::string& action_body, const std::string& synonyms_json = "{}");

// Convenience builders for the most common action bodies.
std::string json_string(const std::string& value);  // JS string literal (safe escaping)
std::string find_body(const std::string& target_json, const std::string& options_json);
std::string summary_body();
std::string fields_body(const std::string& scope_json, bool include_hidden);
std::string extract_body(const std::string& kind_json, const std::string& selector_json,
                         int limit);

// Installs the runtime as window.__sonny once per page and stamps the runtime
// fingerprint onto <html> (the service probes this marker instead of re-sending
// the bundle on every action). `signature` is the fingerprint of synonyms_json.
std::string install_runtime(const std::string& synonyms_json, const std::string& signature);

// Element action bodies (see the runtime API at the bottom of the .cpp).
std::string read_body(const std::string& target_json, const std::string& index_json);
std::string scroll_body(const std::string& direction_json, const std::string& amount_json);
std::string prepare_body(const std::string& target_json, const std::string& index_json);
std::string click_body(const std::string& target_json, const std::string& index_json,
                       const std::string& options_json);
std::string fill_body(const std::string& target_json, const std::string& index_json,
                      const std::string& value_json);
std::string hover_body(const std::string& target_json, const std::string& index_json);
std::string submit_body(const std::string& target_json, const std::string& index_json);
std::string autofill_body(const std::string& values_json);

}  // namespace BrowserPageScripts

// Short alias used by the service layer: Browser::Scripts::wrap(...)
namespace Scripts = BrowserPageScripts;


}  // namespace Browser
}  // namespace Jarvis
