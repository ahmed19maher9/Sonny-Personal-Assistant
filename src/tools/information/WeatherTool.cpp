#include "WeatherTool.h"
#include "ToolUtils.h"
#include "Logger.h"
#include <windows.h>
#include <wininet.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#pragma comment(lib, "wininet.lib")

namespace Jarvis {

std::vector<ToolParameter> WeatherTool::getParameters() const {
    return {
        {"location", "string", "City or location name (e.g. 'London', 'New York'). Leave empty to auto-detect.", false, ""}
    };
}

static std::string fetch_wttr(const std::string& url) {
    HINTERNET hInternet = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return "";

    DWORD timeout = 8000;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConn = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);

    if (!hConn) {
        InternetCloseHandle(hInternet);
        return "";
    }

    std::string response;
    char buffer[4096];
    DWORD bytesRead;
    while (InternetReadFile(hConn, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
        if (response.size() > 4096) break; // wttr.in ?format=3 is tiny
    }

    InternetCloseHandle(hConn);
    InternetCloseHandle(hInternet);
    return response;
}

ToolResult WeatherTool::execute(const std::map<std::string, std::string>& params) {
    auto loc_it = params.find("location");
    std::string location;
    if (loc_it != params.end() && !loc_it->second.empty()) {
        location = loc_it->second;
    }

    // wttr.in ?format=3 returns "City: ☀️ +25°C" — compact, no API key
    // ?format=4 gives multi-line. We use format=3 for voice (single line).
    std::string url;
    if (location.empty()) {
        url = "https://wttr.in/?format=3&lang=en";
    } else {
        url = "https://wttr.in/" + Utils::url_encode(location) + "?format=3&lang=en";
    }

    LOG_DEBUG_COMPONENT("Weather", "Fetching: " + url);
    std::string response = fetch_wttr(url);

    if (response.empty()) {
        return {false, "", "Could not fetch weather data. Check internet connection."};
    }

    // Clean up the response (remove trailing whitespace / newlines)
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r' || response.back() == ' ')) {
        response.pop_back();
    }

    // wttr.in sometimes returns "Unknown location" 
    if (response.find("Unknown location") != std::string::npos) {
        return {false, "", "Unknown location: " + location};
    }

    LOG_DEBUG_COMPONENT("Weather", "Result: " + response);
    return {true, response, ""};
}

} // namespace Jarvis
