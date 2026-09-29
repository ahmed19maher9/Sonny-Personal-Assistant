#include "NewsTool.h"
#include "ToolUtils.h"
#include "Logger.h"
#include <windows.h>
#include <wininet.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#pragma comment(lib, "wininet.lib")

namespace Jarvis {

std::vector<ToolParameter> NewsTool::getParameters() const {
    return {
        {"count",    "string", "Number of headlines to return (default 5, max 10)", false, "5"},
        {"category", "string", "Category: 'world', 'tech', 'business', 'health', 'science', 'entertainment' (default: 'world')", false, "world"}
    };
}

static std::map<std::string, std::string> rss_feeds = {
    {"world",         "https://feeds.bbci.co.uk/news/world/rss.xml"},
    {"tech",          "https://feeds.bbci.co.uk/news/technology/rss.xml"},
    {"business",      "https://feeds.bbci.co.uk/news/business/rss.xml"},
    {"health",        "https://feeds.bbci.co.uk/news/health/rss.xml"},
    {"science",       "https://feeds.bbci.co.uk/news/science_and_environment/rss.xml"},
    {"entertainment", "https://feeds.bbci.co.uk/news/entertainment_and_arts/rss.xml"}
};

static std::string fetch_rss(const std::string& url) {
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
    char buffer[8192];
    DWORD bytesRead;
    while (InternetReadFile(hConn, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
        if (response.size() > 100 * 1024) break;
    }

    InternetCloseHandle(hConn);
    InternetCloseHandle(hInternet);
    return response;
}

static std::string extract_tag(const std::string& xml, const std::string& tag, size_t& pos) {
    std::string open = "<" + tag + ">";
    std::string open_cdata = "<" + tag + "><![CDATA[";
    std::string close = "</" + tag + ">";

    size_t start = xml.find(open, pos);
    if (start == std::string::npos) return "";

    size_t content_start = start + open.length();

    // Handle CDATA
    if (xml.substr(content_start, 9) == "<![CDATA[") {
        content_start += 9;
        size_t cdata_end = xml.find("]]>", content_start);
        if (cdata_end == std::string::npos) return "";
        pos = cdata_end + 3;
        return xml.substr(content_start, cdata_end - content_start);
    }

    size_t end = xml.find(close, content_start);
    if (end == std::string::npos) return "";

    pos = end + close.length();
    return xml.substr(content_start, end - content_start);
}

ToolResult NewsTool::execute(const std::map<std::string, std::string>& params) {
    int count = 5;
    auto count_it = params.find("count");
    if (count_it != params.end()) {
        try { count = std::stoi(count_it->second); } catch (...) {}
    }
    count = std::max(1, std::min(10, count));

    std::string category = "world";
    auto cat_it = params.find("category");
    if (cat_it != params.end() && !cat_it->second.empty()) {
        category = cat_it->second;
        std::transform(category.begin(), category.end(), category.begin(), ::tolower);
    }

    auto feed_it = rss_feeds.find(category);
    if (feed_it == rss_feeds.end()) {
        category = "world";
        feed_it = rss_feeds.find("world");
    }

    std::string url = feed_it->second;
    LOG_DEBUG_COMPONENT("News", "Fetching RSS: " + url);

    std::string xml = fetch_rss(url);
    if (xml.empty()) {
        return {false, "", "Could not fetch news. Check internet connection."};
    }

    // Parse <item> blocks and extract <title>
    std::vector<std::string> headlines;
    size_t pos = 0;
    size_t item_pos = xml.find("<item>", 0);

    while (item_pos != std::string::npos && (int)headlines.size() < count) {
        size_t item_end = xml.find("</item>", item_pos);
        if (item_end == std::string::npos) break;

        std::string item = xml.substr(item_pos, item_end - item_pos);
        size_t title_pos = 0;
        std::string title = extract_tag(item, "title", title_pos);

        if (!title.empty()) {
            // Clean up HTML entities
            auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
                size_t p = 0;
                while ((p = s.find(from, p)) != std::string::npos) {
                    s.replace(p, from.length(), to);
                    p += to.length();
                }
            };
            replace_all(title, "&amp;", "&");
            replace_all(title, "&lt;", "<");
            replace_all(title, "&gt;", ">");
            replace_all(title, "&quot;", "\"");
            replace_all(title, "&#39;", "'");
            replace_all(title, "&apos;", "'");

            headlines.push_back(title);
        }

        item_pos = xml.find("<item>", item_end);
    }

    if (headlines.empty()) {
        return {false, "", "No headlines found in RSS feed."};
    }

    std::string result = "Latest " + category + " headlines from BBC News:\n";
    for (int i = 0; i < (int)headlines.size(); ++i) {
        result += std::to_string(i + 1) + ". " + headlines[i] + "\n";
    }

    return {true, result, ""};
}

} // namespace Jarvis
