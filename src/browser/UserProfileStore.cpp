// UserProfileStore.cpp - see UserProfileStore.h for the design notes.
#include "UserProfileStore.h"

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

#include "BrowserCrypto.h"
#include "Logger.h"
#include "BrowserUtil.h"

#include "../resources/avatars/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>

#pragma comment(lib, "crypt32.lib")

namespace Jarvis {
namespace Browser {

namespace {

// ---------------------------------------------------------------------------
// The canonical field model.
//
// This table is the single source of truth for autofill field names: the C++
// matcher below uses it directly, and the same JSON is injected into the page
// runtime (BrowserPageScripts) so the JavaScript classifier cannot drift from
// it. Multi-word synonyms deliberately outscore single words ("first name"
// beats "name") which is what keeps "First name" out of the "full name" bucket.
// ---------------------------------------------------------------------------
const char* kSynonymsPart1 = R"JSON({
  "first_name": ["first name", "given name", "forename", "fname", "first"],
  "middle_name": ["middle name", "middle initial"],
  "last_name": ["last name", "surname", "family name", "lname", "last"],
  "full_name": ["full name", "your name", "complete name", "legal name", "applicant name", "name"],
  "preferred_name": ["preferred name", "nickname", "display name"],
  "honorific": ["salutation", "prefix", "mr mrs ms dr", "title"],
  "email": ["email", "e mail", "email address", "email id", "mail id"],
  "phone": ["phone", "phone number", "mobile", "mobile number", "cell phone", "cellphone", "telephone", "contact number", "whatsapp"],
  "username": ["username", "user name", "login", "login id", "account name", "handle"],
  "password": ["password", "passcode", "passphrase"],
  "birth_date": ["date of birth", "birth date", "birthday", "dob"],
  "gender": ["gender", "sex"],
  "nationality": ["nationality", "citizenship", "country of citizenship"],
  "marital_status": ["marital status", "marriage status"],
  "address": ["street address", "address line 1", "home address", "residential address", "mailing address", "street"],
  "address2": ["address line 2", "apartment", "suite", "unit number", "floor"],
  "city": ["city", "town", "locality", "municipality"],
)JSON";
const char* kSynonymsPart2 = R"JSON(
  "state": ["state", "province", "region", "county", "address level 1"],
  "zip": ["zip", "zip code", "postal code", "postcode", "post code", "pin code"],
  "country": ["country", "country region", "nation"],
  "company": ["company", "employer", "organization", "organisation", "company name", "current employer"],
  "job_title": ["job title", "position", "job position", "designation", "current title"],
  "department": ["department", "team name", "division", "business unit"],
  "work_email": ["work email", "business email", "office email"],
  "work_phone": ["work phone", "office phone", "business phone", "extension"],
  "website": ["website", "personal website", "homepage", "blog url"],
  "linkedin": ["linkedin", "linkedin profile", "linkedin url"],
  "github": ["github", "github username", "github profile"],
  "twitter": ["twitter", "twitter handle", "x handle", "social handle"],
  "portfolio": ["portfolio", "portfolio url", "projects url"],
  "education": ["education", "highest education", "education level", "qualification", "degree level"],
  "school": ["school name", "university", "college", "institution", "alma mater", "institute"],
  "major": ["major", "field of study", "discipline", "subject", "course"],
  "graduation_year": ["graduation year", "year of graduation", "passing year", "completion year"],
  "gpa": ["gpa", "grade point average", "cgpa", "final grade"],
)JSON";

const char* kSynonymsPart3 = R"JSON(
  "skills": ["skills", "key skills", "competencies", "technical skills", "expertise"],
  "experience_years": ["years of experience", "total experience", "work experience"],
  "resume": ["resume text", "cv text", "professional summary", "bio", "about you", "profile summary"],
  "cover_letter": ["cover letter", "motivation letter", "why do you want", "additional information", "comments"],
  "references": ["references", "referees", "reference contact"],
  "salary_expectation": ["expected salary", "salary expectation", "expected ctc", "desired salary", "compensation expectation"],
  "availability": ["availability", "available from", "start date", "earliest start date", "notice period"],
  "relocate": ["willing to relocate", "relocation", "relocate"],
  "remote_preference": ["remote work", "work mode", "preferred work location", "working arrangement"],
  "visa_status": ["work authorization", "work permit", "visa status", "right to work", "sponsorship required"],
  "national_id": ["social security number", "ssn", "national id", "national insurance number", "aadhaar", "emirates id"],
  "tax_id": ["tax id", "tax identification number", "pan number"],
  "passport_number": ["passport number", "passport no"],
  "driver_license": ["driver license", "driving licence", "license number"],
  "emergency_contact": ["emergency contact", "emergency contact name", "next of kin"],
  "emergency_phone": ["emergency phone", "emergency contact number", "emergency telephone"],
  "bank_account": ["bank account", "account number", "iban", "sort code", "routing number"],
  "cardholder": ["name on card", "cardholder name", "card holder"],
  "card_number": ["card number", "credit card number", "debit card number"],
  "card_expiry": ["card expiry", "card expiration", "valid thru"],
  "card_cvv": ["cvv", "cvc", "card verification value"],
  "student_id": ["student id", "student number", "registration number", "roll number"],
  "employee_id": ["employee id", "staff id", "employee number"],
  "consent": ["i agree", "terms and conditions", "privacy policy", "accept the terms", "terms of service"],
  "newsletter": ["newsletter", "subscribe", "mailing list", "keep me updated"],
  "coupon": ["coupon", "promo code", "discount code", "voucher code"],
  "otp": ["one time code", "verification code", "otp", "authentication code", "two factor code"]
}
)JSON";
std::string normalize_phrase(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_space = true;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
            last_space = false;
        } else if (!last_space) {
            out.push_back(' ');
            last_space = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string squash_phrase(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

size_t word_count(const std::string& text) {
    size_t count = 0;
    bool in_word = false;
    for (char c : text) {
        if (c == ' ') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++count;
        }
    }
    return count;
}

bool contains_word_sequence(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    return (" " + haystack + " ").find(" " + needle + " ") != std::string::npos;
}

const std::map<std::string, std::vector<std::string>>& synonym_table() {
    static std::map<std::string, std::vector<std::string>> table;
    static std::mutex guard;
    std::lock_guard<std::mutex> lock(guard);
    if (!table.empty()) return table;

    nlohmann::json parsed = nlohmann::json::parse(UserProfileStore::synonyms_json(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        LOG_ERROR("Browser", "Synonym table is not valid JSON - autofill matching disabled");
        return table;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        std::vector<std::string> synonyms;
        if (it.value().is_array()) {
            for (const auto& entry : it.value()) {
                if (entry.is_string()) synonyms.push_back(entry.get<std::string>());
            }
        }
        table[it.key()] = std::move(synonyms);
    }
    return table;
}

std::string normalize_host(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '.' || c == '-') {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

std::string default_profile_file() {
    return appdata_dir() + "\\Sonny\\browser_profile.json";
}

// ---- DPAPI: passwords are encrypted with the current Windows user's key ----

std::string dpapi_protect(const std::string& plain) {
    if (plain.empty()) return "";
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Sonny browser credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        LOG_ERROR("Browser", "CryptProtectData failed (" + std::to_string(GetLastError()) + ")");
        return "";
    }
    std::string encrypted(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return Crypto::base64_encode(encrypted);
}

std::string dpapi_unprotect(const std::string& encoded) {
    std::string encrypted;
    if (!Crypto::base64_decode(encoded, encrypted) || encrypted.empty()) return "";
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()));
    input.cbData = static_cast<DWORD>(encrypted.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        LOG_WARN("Browser", "Stored credential could not be decrypted (different Windows user?)");
        return "";
    }
    std::string plain(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return plain;
}

}  // anonymous namespace
UserProfileStore::UserProfileStore() : profile_file_(default_profile_file()) {}

UserProfileStore& UserProfileStore::getInstance() {
    static UserProfileStore instance;
    return instance;
}

void UserProfileStore::configure(const std::string& profile_file) {
    if (!profile_file.empty() && profile_file != profile_file_) {
        profile_file_ = profile_file;
        loaded_ = false;
        fields_.clear();
        custom_.clear();
        credentials_.clear();
    }
}

std::string UserProfileStore::path() {
    if (profile_file_.empty()) profile_file_ = default_profile_file();
    return profile_file_;
}

const std::string& UserProfileStore::synonyms_json() {
    static const std::string table = std::string(kSynonymsPart1) + kSynonymsPart2 +
                                     kSynonymsPart3;
    return table;
}

std::vector<std::string> UserProfileStore::canonical_keys() {
    std::vector<std::string> keys;
    for (const auto& entry : synonym_table()) keys.push_back(entry.first);
    return keys;
}

std::string UserProfileStore::canonical_key(const std::string& phrase) {
    const std::string haystack = normalize_phrase(phrase);
    if (haystack.empty()) return "";
    const std::string compact = squash_phrase(phrase);

    std::string best_key;
    int best_score = 0;
    for (const auto& entry : synonym_table()) {
        for (const auto& raw_synonym : entry.second) {
            const std::string synonym = normalize_phrase(raw_synonym);
            if (synonym.empty()) continue;
            const int words = static_cast<int>(word_count(synonym));

            int score = 0;
            if (haystack == synonym) {
                score = 90 + words * 12;
            } else if (contains_word_sequence(haystack, synonym)) {
                score = 60 + words * 12;
            } else {
                const std::string compact_synonym = squash_phrase(synonym);
                if (compact_synonym.size() > 3 &&
                    compact.find(compact_synonym) != std::string::npos) {
                    score = 30 + words * 12;
                }
            }
            if (score > best_score) {
                best_score = score;
                best_key = entry.first;
            }
        }
    }
    // Below this threshold the "match" is noise, and filling the wrong field is
    // worse than leaving it for the browser's own autofill.
    return best_score >= 42 ? best_key : "";
}

std::string UserProfileStore::display_name(const std::string& key) {
    std::string label;
    label.reserve(key.size());
    bool capitalize = true;
    for (char c : key) {
        if (c == '_') {
            label.push_back(' ');
            capitalize = true;
        } else if (capitalize) {
            label.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            capitalize = false;
        } else {
            label.push_back(c);
        }
    }
    return label;
}
bool UserProfileStore::load() {
    if (loaded_) return true;

    fields_.clear();
    custom_.clear();
    credentials_.clear();

    std::ifstream file(path());
    if (!file.is_open()) {
        loaded_ = true;
        LOG_INFO("Browser", "No autofill profile yet - it will be created at " + path());
        return true;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        LOG_ERROR("Browser", "Autofill profile is not valid JSON: " + path());
        loaded_ = true;
        return false;
    }

    auto read_map = [](const nlohmann::json& source,
                       std::map<std::string, std::string>& target) {
        if (!source.is_object()) return;
        for (auto it = source.begin(); it != source.end(); ++it) {
            if (it.value().is_string()) {
                target[it.key()] = it.value().get<std::string>();
            } else if (it.value().is_number() || it.value().is_boolean()) {
                target[it.key()] = it.value().dump();
            }
        }
    };

    read_map(parsed.value("fields", nlohmann::json::object()), fields_);
    read_map(parsed.value("custom", nlohmann::json::object()), custom_);

    const nlohmann::json creds = parsed.value("credentials", nlohmann::json::object());
    if (creds.is_object()) {
        for (auto it = creds.begin(); it != creds.end(); ++it) {
            if (!it.value().is_object()) continue;
            std::map<std::string, std::string> entry;
            read_map(it.value(), entry);
            credentials_[it.key()] = std::move(entry);
        }
    }

    loaded_ = true;
    LOG_INFO("Browser", "Loaded autofill profile: " + std::to_string(fields_.size()) +
                            " field(s), " + std::to_string(credentials_.size()) +
                            " saved site credential(s)");
    return true;
}

bool UserProfileStore::save() {
    std::error_code ec;
    std::filesystem::path target(path());
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    nlohmann::json root;
    root["version"] = 1;
    root["fields"] = fields_;
    root["custom"] = custom_;

    nlohmann::json creds = nlohmann::json::object();
    for (const auto& entry : credentials_) {
        nlohmann::json item = nlohmann::json::object();
        for (const auto& field : entry.second) item[field.first] = field.second;
        creds[entry.first] = item;
    }
    root["credentials"] = creds;

    std::ofstream file(target, std::ios::trunc);
    if (!file.is_open()) {
        LOG_ERROR("Browser", "Cannot write autofill profile: " + path());
        return false;
    }
    file << root.dump(2);
    file.close();
    LOG_DEBUG_COMPONENT("Browser", "Saved autofill profile");
    return true;
}
// ------------------------------------------------------- field access ------

bool UserProfileStore::has(const std::string& key) const {
    return fields_.find(key) != fields_.end() && !fields_.at(key).empty();
}

std::string UserProfileStore::get(const std::string& key) const {
    auto it = fields_.find(key);
    return it == fields_.end() ? "" : it->second;
}

void UserProfileStore::set(const std::string& key, const std::string& value) {
    if (key.empty()) return;
    if (value.empty()) {
        fields_.erase(key);
        return;
    }
    fields_[key] = value;
}

bool UserProfileStore::remove(const std::string& key) {
    return fields_.erase(key) > 0;
}

std::vector<ProfileField> UserProfileStore::all() const {
    std::vector<ProfileField> out;
    out.reserve(fields_.size() + custom_.size());
    for (const auto& entry : fields_) out.push_back({entry.first, entry.second});
    for (const auto& entry : custom_) out.push_back({entry.first, entry.second});
    std::sort(out.begin(), out.end(), [](const ProfileField& a, const ProfileField& b) {
        return a.key < b.key;
    });
    return out;
}

std::map<std::string, std::string> UserProfileStore::autofill_values() const {
    std::map<std::string, std::string> values = fields_;
    // Custom (user-taught) keys are merged last so they always win, including
    // for canonical keys the user wants to override.
    for (const auto& entry : custom_) values[entry.first] = entry.second;
    return values;
}

// --------------------------------------------------- credential vault ------

std::string UserProfileStore::normalize_site(const std::string& site_or_url) {
    std::string site = site_or_url;
    const size_t scheme = site.find("://");
    if (scheme != std::string::npos) site = site.substr(scheme + 3);
    size_t cut = site.find_first_of("/?#");
    if (cut != std::string::npos) site = site.substr(0, cut);
    const size_t at = site.find('@');
    if (at != std::string::npos) site = site.substr(at + 1);
    const size_t colon = site.rfind(':');
    if (colon != std::string::npos && site.find(']') == std::string::npos) {
        site = site.substr(0, colon);
    }
    if (site.rfind("www.", 0) == 0) site = site.substr(4);
    return normalize_host(site);
}

bool UserProfileStore::store_credential(const std::string& site, const std::string& username,
                                        const std::string& password) {
    const std::string host = normalize_site(site);
    if (host.empty()) return false;

    std::map<std::string, std::string> entry = credentials_[host];
    if (!username.empty()) entry["username"] = username;
    if (!password.empty()) {
        const std::string encrypted = dpapi_protect(password);
        if (encrypted.empty()) {
            LOG_ERROR("Browser", "Password could not be encrypted - credential not stored");
            return false;
        }
        entry["password"] = encrypted;
    }
    credentials_[host] = std::move(entry);
    return save();
}

bool UserProfileStore::get_credential(const std::string& site, std::string& username,
                                      std::string& password) const {
    const std::string host = normalize_site(site);
    username.clear();
    password.clear();

    auto it = credentials_.find(host);
    if (it == credentials_.end()) {
        // Fall back to a parent-domain match (login.contoso.com -> contoso.com).
        for (const auto& entry : credentials_) {
            if (host.size() > entry.first.size() &&
                host.compare(host.size() - entry.first.size(), entry.first.size(),
                             entry.first) == 0) {
                it = credentials_.find(entry.first);
                break;
            }
        }
    }
    if (it == credentials_.end()) return false;

    auto user = it->second.find("username");
    if (user != it->second.end()) username = user->second;
    auto secret = it->second.find("password");
    if (secret != it->second.end()) password = dpapi_unprotect(secret->second);
    return !username.empty() || !password.empty();
}

bool UserProfileStore::forget_credential(const std::string& site) {
    const std::string host = normalize_site(site);
    if (credentials_.erase(host) == 0) return false;
    return save();
}

std::vector<std::string> UserProfileStore::credential_sites() const {
    std::vector<std::string> sites;
    sites.reserve(credentials_.size());
    for (const auto& entry : credentials_) sites.push_back(entry.first);
    return sites;
}

}  // namespace Browser
}  // namespace Jarvis