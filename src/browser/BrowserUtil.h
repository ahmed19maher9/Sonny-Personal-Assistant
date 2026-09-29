#pragma once

// Common utilities for browser components - avoids anonymous namespace conflicts in unity builds

#include <string>
#include <chrono>
#include <windows.h>
#include <shlobj.h>
#include <filesystem>

namespace Jarvis {
namespace Browser {

// Winsock bootstrap
struct WinsockBootstrap {
    WinsockBootstrap() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockBootstrap() { WSACleanup(); }
};

inline void ensure_winsock() {
    static WinsockBootstrap bootstrap;
    (void)bootstrap;
}

// Current time in milliseconds
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// String utilities
inline std::string trim(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

inline std::string to_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// AppData directory
inline std::string appdata_dir() {
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::string(path);
    }
    // Fallback to environment variable
    char buffer[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA("APPDATA", buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ".";
    return std::string(buffer, len);
}

// Environment variable path
inline std::string env_path(const char* name) {
    char buffer[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA(name, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    return std::string(buffer, len);
}

// Read a REG_SZ value; returns "" when missing.
inline std::string read_registry_string(HKEY root, const std::string& subkey,
                                        const std::string& value_name) {
    char buffer[MAX_PATH * 2] = {0};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegGetValueA(root, subkey.c_str(), value_name.c_str(), RRF_RT_REG_SZ, &type, buffer,
                     &size) != ERROR_SUCCESS) {
        return "";
    }
    return std::string(buffer);
}

// File existence check
inline bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Ensure directory exists
inline void ensure_directory(const std::string& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}

// Quote argument for command line
inline std::string quote_arg(const std::string& value) {
    return "\"" + value + "\"";
}

}  // namespace Browser
}  // namespace Jarvis