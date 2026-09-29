#pragma once

// Common utilities for core components - avoids anonymous namespace conflicts in unity builds

#include <string>

namespace Jarvis {

// Check if text contains Arabic or other RTL characters
inline bool contains_rtl(const std::string& text) {
    for (unsigned char c : text) {
        if ((c >= 0x0600 && c <= 0x06FF) ||  // Arabic
            (c >= 0x0750 && c <= 0x077F) ||  // Arabic Supplement
            (c >= 0x08A0 && c <= 0x08FF) ||  // Arabic Extended-A
            (c >= 0xFB50 && c <= 0xFDFF) ||  // Arabic Presentation Forms-A
            (c >= 0xFE70 && c <= 0xFEFF)) {  // Arabic Presentation Forms-B
            return true;
        }
    }
    return false;
}

// Wrap text with RTL markers for proper console display
inline std::string rtl_wrap(const std::string& text) {
    if (contains_rtl(text)) {
        return "\u202B" + text + "\u202C";
    }
    return text;
}

}  // namespace Jarvis