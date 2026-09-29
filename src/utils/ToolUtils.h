#pragma once

#include <string>
#include <sstream>

namespace Jarvis {
namespace Utils {

// URL-encode a string for use in URLs
inline std::string url_encode(const std::string& value) {
    std::string encoded;
    for (char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || 
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            std::ostringstream oss;
            oss << '%' << std::hex << (int)(unsigned char)c;
            encoded += oss.str();
        }
    }
    return encoded;
}

}
}