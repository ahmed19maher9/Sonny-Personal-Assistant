#pragma once

#include <string>
#include <vector>
#include <map>

namespace Jarvis {
namespace Browser {

struct ProfileField {
    std::string key;
    std::string value;
};

// ---------------------------------------------------------------------------
// The user's autofill profile: personal details Sonny can enter into web forms
// automatically, plus a DPAPI-encrypted credential vault so Sonny can log into
// sites for the user ("Sonny, log into my dashboard").
//
// Design notes
//   * One source of truth for field names. The canonical key list and its
//     synonym table below drive BOTH the C++ matcher and the JavaScript
//     classifier injected into the page (see BrowserPageScripts), so a rule
//     like "postal code means zip" only ever has to be written once.
//   * Values live in %APPDATA%\Sonny\browser_profile.json - plain JSON for the
//     profile fields (they are the same details a browser's own autofill keeps)
//     and DPAPI-protected blobs for passwords, which are encrypted with the
//     Windows user's key via CryptProtectData, exactly like Chrome's own
//     credential store does at rest.
//   * Nothing here is ever sent anywhere: autofill happens in the local browser
//     instance Sonny controls.
// ---------------------------------------------------------------------------
class UserProfileStore {
public:
    static UserProfileStore& getInstance();

    // Explicit profile file. Defaults to %APPDATA%\Sonny\browser_profile.json.
    void configure(const std::string& profile_file);
    std::string path();

    bool load();
    bool save();

    // ---- field access ----
    bool has(const std::string& key) const;
    std::string get(const std::string& key) const;
    void set(const std::string& key, const std::string& value);
    bool remove(const std::string& key);
    std::vector<ProfileField> all() const;

    // ---- field naming model (shared with the injected JavaScript) ----
    // JSON object: canonical_key -> [synonym, ...]
    static const std::string& synonyms_json();
    static std::vector<std::string> canonical_keys();
    // Map a human/HTML phrase ("Postal code", "given-name") to a canonical key.
    // Returns "" when nothing matches with reasonable confidence.
    static std::string canonical_key(const std::string& phrase);
    static std::string display_name(const std::string& key);

    // Canonical key -> value, credentials excluded (safe for autofill).
    std::map<std::string, std::string> autofill_values() const;

    // ---- credential vault ----
    bool store_credential(const std::string& site, const std::string& username,
                          const std::string& password);
    bool get_credential(const std::string& site, std::string& username,
                        std::string& password) const;
    bool forget_credential(const std::string& site);
    std::vector<std::string> credential_sites() const;
    // "https://github.com/login?x=1" -> "github.com"
    static std::string normalize_site(const std::string& site_or_url);

private:
    UserProfileStore();

    mutable std::map<std::string, std::string> fields_;
    mutable std::map<std::string, std::string> custom_;
    // site -> { username, password (DPAPI base64) }
    mutable std::map<std::string, std::map<std::string, std::string>> credentials_;
    mutable bool loaded_ = false;
    std::string profile_file_;
};

}  // namespace Browser
}  // namespace Jarvis