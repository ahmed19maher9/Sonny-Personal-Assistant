#pragma once
// ============================================================================
// PathUtil.h - UTF-8 <-> UTF-16 path/text conversion helpers.
//
// Why this exists: MSVC's std::filesystem::path::string() converts wide paths
// to the system ANSI code page (ACP, e.g. CP-1252) and *throws* when a file
// name contains characters the ACP cannot represent (e.g. fullwidth '＂'
// U+FF02 or '：' U+FF1A in many YouTube-dl filenames). Every directory scan
// that calls .string() therefore silently loses those files.
//
// The app's internal text encoding is UTF-8; these helpers convert to/from
// UTF-16 (the native Windows encoding) directly, never through the ACP, and
// never throw.
// ============================================================================

#include <string>
#include <windows.h>

namespace Jarvis {
namespace pathutil {

// UTF-8 -> UTF-16. Never throws. Falls back to lenient conversion (invalid
// sequences become U+FFFD) and finally to naive byte widening.
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_UTF8, 0,
                                s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return std::wstring(s.begin(), s.end());
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

// UTF-16 -> UTF-8. Never throws. Returns an empty string only if the input
// cannot be converted at all (practically impossible for CP_UTF8).
inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

} // namespace pathutil
} // namespace Jarvis
