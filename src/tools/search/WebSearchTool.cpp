#include "WebSearchTool.h"
#include "Logger.h"
#include "ToolUtils.h"
#include <windows.h>
#include <wininet.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#pragma comment(lib, "wininet.lib")

namespace Jarvis {

std::vector<ToolParameter> WebSearchTool::getParameters() const {
    return {
        {"query", "string", "The search query to search for on the web", true, ""}
    };
}

// Helper: fetch HTML content from a URL with timeouts and proper browser headers
static std::string fetch_url(const std::string& url) {
    HINTERNET hInternet = InternetOpenA("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        std::cerr << "[WebSearch] Failed to initialize WinINet" << std::endl;
        return "";
    }
    
    // Set timeouts to prevent hanging
    DWORD timeout = 8000;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    
    // Add proper HTTP headers to look like a real browser
    // Don't request compression to avoid gzip decompression issues
    LPCSTR headers = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
                     "Accept-Language: en-US,en;q=0.9\r\n"
                     "Connection: keep-alive\r\n"
                     "Upgrade-Insecure-Requests: 1\r\n"
                     "Sec-Fetch-Dest: document\r\n"
                     "Sec-Fetch-Mode: navigate\r\n"
                     "Sec-Fetch-Site: none\r\n"
                     "Sec-Fetch-User: ?1\r\n"
                     "Cache-Control: max-age=0\r\n";
    
    HINTERNET hConnect = InternetOpenUrlA(hInternet, url.c_str(), headers,
        lstrlenA(headers),
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
        INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID |
        INTERNET_FLAG_HYPERLINK | INTERNET_FLAG_NO_COOKIES, 0);
    if (!hConnect) {
        DWORD err = GetLastError();
        std::cerr << "[WebSearch] InternetOpenUrlA failed with error: " << err << std::endl;
        InternetCloseHandle(hInternet);
        return "";
    }
    
    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!HttpQueryInfoA(hConnect, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusCodeSize, NULL)) {
        std::cerr << "[WebSearch] Failed to get HTTP status" << std::endl;
    }
    
    if (statusCode >= 300 && statusCode < 400) {
        // Redirect - get the new location
        char redirectUrl[2048] = {0};
        DWORD redirectSize = sizeof(redirectUrl);
        if (HttpQueryInfoA(hConnect, HTTP_QUERY_LOCATION, redirectUrl, &redirectSize, NULL)) {
            LOG_WEBSEARCH("Following redirect to: " + std::string(redirectUrl));
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hInternet);
            // Follow redirect recursively (limit 1 level)
            HINTERNET hInternet2 = InternetOpenA("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
            if (hInternet2) {
                HINTERNET hConnect2 = InternetOpenUrlA(hInternet2, redirectUrl, headers,
                    lstrlenA(headers),
                    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
                    INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
                if (hConnect2) {
                    hInternet = hInternet2;
                    hConnect = hConnect2;
                } else {
                    InternetCloseHandle(hInternet2);
                    return "";
                }
            }
        }
    }
    
    std::string response;
    char buffer[4096];
    DWORD bytesRead;
    const size_t MAX_RESPONSE_SIZE = 150 * 1024; // 150KB max
    
    while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
        if (response.size() > MAX_RESPONSE_SIZE) {
            std::cerr << "[WebSearch] Response too large, truncating" << std::endl;
            break;
        }
    }
    
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    
    LOG_WEBSEARCH("Fetched " + std::to_string(response.size()) + " bytes, HTTP status: " + std::to_string(statusCode));
    return response;
}

// Helper: decode HTML entities
static std::string decode_html_entities(const std::string& input) {
    std::string result = input;
    const std::string AMP_ENT = std::string(1, (char)0x26) + "amp;";
    const std::string LT_ENT = std::string(1, (char)0x26) + "lt;";
    const std::string GT_ENT = std::string(1, (char)0x26) + "gt;";
    const std::string QUOT_ENT = std::string(1, (char)0x26) + "quot;";
    const std::string APOS_ENT = std::string(1, (char)0x26) + "#39;";
    const std::string NBSP_ENT = std::string(1, (char)0x26) + "nbsp;";
    
    size_t pos = 0;
    while ((pos = result.find(AMP_ENT, pos)) != std::string::npos) {
        result.replace(pos, 5, "&");
        pos++;
    }
    pos = 0;
    while ((pos = result.find(LT_ENT, pos)) != std::string::npos) {
        result.replace(pos, 4, "<");
        pos++;
    }
    pos = 0;
    while ((pos = result.find(GT_ENT, pos)) != std::string::npos) {
        result.replace(pos, 4, ">");
        pos++;
    }
    pos = 0;
    while ((pos = result.find(QUOT_ENT, pos)) != std::string::npos) {
        result.replace(pos, 6, "\"");
        pos++;
    }
    pos = 0;
    while ((pos = result.find(APOS_ENT, pos)) != std::string::npos) {
        result.replace(pos, 5, "'");
        pos++;
    }
    pos = 0;
    while ((pos = result.find(NBSP_ENT, pos)) != std::string::npos) {
        result.replace(pos, 6, " ");
        pos++;
    }
    return result;
}

// Helper: strip HTML tags from a string
static std::string strip_html_tags(const std::string& input) {
    std::string result = input;
    size_t block_start = 0;
    while ((block_start = result.find("<script", block_start)) != std::string::npos) {
        size_t block_end = result.find("</script>", block_start);
        if (block_end == std::string::npos) {
            result.erase(block_start);
            break;
        }
        result.erase(block_start, block_end + 9 - block_start);
    }
    block_start = 0;
    while ((block_start = result.find("<style", block_start)) != std::string::npos) {
        size_t block_end = result.find("</style>", block_start);
        if (block_end == std::string::npos) {
            result.erase(block_start);
            break;
        }
        result.erase(block_start, block_end + 8 - block_start);
    }
    size_t pos = 0;
    while ((pos = result.find("<", pos)) != std::string::npos) {
        size_t end = result.find(">", pos);
        if (end != std::string::npos) {
            result.erase(pos, end - pos + 1);
        } else {
            break;
        }
    }
    // Trim whitespace
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);
    return result;
}

// Helper: extract search results from DuckDuckGo HTML
static bool extract_duckduckgo_results(const std::string& response, const std::string& query, std::string& results, std::vector<std::string>& urls, int max_results = 3) {
    results = "";
    urls.clear();
    int count = 0;
    size_t pos = 0;

    // DuckDuckGo results are in <div class="result"> or <div class="web-result"> containers
    while ((pos = response.find("<div class=\"result", pos)) != std::string::npos && count < max_results) {
        // Find the end of this result block
        size_t result_end = response.find("</div>", pos + 20);
        if (result_end == std::string::npos) break;
        size_t next_result = response.find("<div class=\"result", pos + 1);
        if (next_result != std::string::npos && next_result < result_end) {
            result_end = next_result;
        }

        std::string result_block = response.substr(pos, result_end - pos);

        // Extract URL from href - try multiple patterns
        std::string url;
        // Pattern 1: href="//duckduckgo.com/l/?uddg=..." (DuckDuckGo redirect)
        size_t href_start = result_block.find("href=\"//duckduckgo.com/l/?uddg=");
        if (href_start == std::string::npos) {
            // Pattern 2: href='//duckduckgo.com/l/?uddg=...'
            href_start = result_block.find("href='//duckduckgo.com/l/?uddg=");
        }
        if (href_start == std::string::npos) {
            // Pattern 3: href="https://..."
            href_start = result_block.find("href=\"https://");
        }
        if (href_start == std::string::npos) {
            // Pattern 4: href='https://...
            href_start = result_block.find("href='https://");
        }
        if (href_start == std::string::npos) {
            // Pattern 5: href="http://...
            href_start = result_block.find("href=\"http://");
        }
        if (href_start == std::string::npos) {
            // Pattern 6: href='http://...
            href_start = result_block.find("href='http://");
        }

        if (href_start != std::string::npos) {
            // Find the quote character used
            char quote = result_block[href_start + 5];
            href_start += 6; // Skip past href="
            size_t href_end = result_block.find(quote, href_start);
            if (href_end != std::string::npos) {
                url = result_block.substr(href_start, href_end - href_start);
                // Decode DuckDuckGo redirect URLs
                size_t ludd = url.find("uddg=");
                if (ludd != std::string::npos) {
                    ludd += 5;
                    size_t ludd_end = url.find("&", ludd);
                    if (ludd_end != std::string::npos) {
                        url = url.substr(ludd, ludd_end - ludd);
                    } else {
                        url = url.substr(ludd);
                    }
                }
                // URL decode
                size_t u_pos = 0;
                while ((u_pos = url.find('%', u_pos)) != std::string::npos && u_pos + 2 < url.length()) {
                    char hex[3] = {url[u_pos + 1], url[u_pos + 2], 0};
                    char* endptr;
                    long val = strtol(hex, &endptr, 16);
                    if (val > 0) {
                        url[u_pos] = (char)val;
                        url.erase(u_pos + 1, 2);
                    }
                    u_pos++;
                }
            }
        }

        // Extract title from <a class="result__a"> or <h2>
        std::string title;
        size_t title_start = result_block.find("<a class=\"result__a\"");
        if (title_start == std::string::npos) {
            title_start = result_block.find("<h2");
        }
        if (title_start != std::string::npos) {
            size_t title_tag_end = result_block.find(">", title_start);
            if (title_tag_end != std::string::npos) {
                title_tag_end++;
                size_t title_close = result_block.find("</a>", title_tag_end);
                if (title_close == std::string::npos) {
                    title_close = result_block.find("</h2>", title_tag_end);
                }
                if (title_close != std::string::npos) {
                    title = result_block.substr(title_tag_end, title_close - title_tag_end);
                }
            }
        }

        // Extract snippet from various possible classes
        std::string snippet;
        std::vector<std::string> snippet_classes = {
            "class=\"result__snippet\"", "class=\"result__url\"", "class=\"result__description\"",
            "class=\"snippet\"", "class=\"description\""
        };

        for (const auto& snippet_class : snippet_classes) {
            size_t snip_start = result_block.find(snippet_class);
            if (snip_start != std::string::npos) {
                size_t gt = result_block.find(">", snip_start);
                if (gt != std::string::npos) {
                    gt++;
                    size_t snip_end = result_block.find("</div>", gt);
                    size_t span_end = result_block.find("</span>", gt);
                    size_t a_end = result_block.find("</a>", gt);
                    if (snip_end == std::string::npos || (span_end != std::string::npos && span_end < snip_end)) {
                        snip_end = span_end;
                    }
                    if (snip_end == std::string::npos || (a_end != std::string::npos && a_end < snip_end)) {
                        snip_end = a_end;
                    }
                    if (snip_end != std::string::npos && snip_end - gt < 500) {
                        snippet = result_block.substr(gt, snip_end - gt);
                        break;
                    }
                }
            }
        }

        // If no snippet found in classes, try to extract from the entire result block
        if (snippet.empty()) {
            // Look for text after the title that might be a description
            size_t title_end = result_block.find("</a>");
            if (title_end != std::string::npos) {
                title_end += 4;
                // Find the next meaningful text block
                size_t text_start = result_block.find("<", title_end);
                while (text_start != std::string::npos && text_start < result_end - 100) {
                    size_t text_end = result_block.find(">", text_start);
                    if (text_end != std::string::npos) {
                        text_end++;
                        size_t next_tag = result_block.find("<", text_end);
                        if (next_tag != std::string::npos && next_tag - text_end > 50) {
                            std::string potential_snippet = result_block.substr(text_end, next_tag - text_end);
                            if (potential_snippet.length() > 50 && potential_snippet.length() < 500) {
                                snippet = potential_snippet;
                                break;
                            }
                        }
                    }
                    text_start = result_block.find("<", text_end);
                }
            }
        }

        // Clean up HTML
        title = strip_html_tags(title);
        title = decode_html_entities(title);
        snippet = strip_html_tags(snippet);
        snippet = decode_html_entities(snippet);

        // Only add if we have meaningful content
        if (!title.empty() && title.length() > 5) {
            if (!results.empty()) results += "\n";
            results += std::to_string(count + 1) + ". " + title;
            if (!snippet.empty() && snippet.length() > 10) {
                // Keep more of the snippet for better context - increased to 500 chars
                if (snippet.length() > 500) {
                    snippet = snippet.substr(0, 497) + "...";
                }
                results += ": " + snippet;
            }
            if (!url.empty()) {
                urls.push_back(url);
                LOG_WEBSEARCH("Extracted URL: " + url);
            } else {
                urls.push_back("");
                LOG_WEBSEARCH("No URL found for result " + std::to_string(count + 1));
            }
            count++;
        }

        pos = result_end;
    }

    if (count > 0) {
        LOG_WEBSEARCH("Extracted " + std::to_string(count) + " DuckDuckGo results");
        return true;
    }

    return false;
}

ToolResult WebSearchTool::execute(const std::map<std::string, std::string>& params) {
    auto it = params.find("query");
    if (it == params.end()) {
        return {false, "", "Missing required parameter: query"};
    }

    std::string query = it->second;
    // Use DuckDuckGo HTML version with improved headers to avoid CAPTCHA
    std::string url = "https://duckduckgo.com/html/?q=" + Utils::url_encode(query);

    // Add delay to avoid rate limiting and CAPTCHA
    std::string response = fetch_url(url);
    if (response.empty()) {
        return {false, "", "Failed to fetch search results. Check your internet connection."};
    }

    // Try to extract results
    std::string results;
    std::vector<std::string> urls;
    if (extract_duckduckgo_results(response, query, results, urls, 5)) {
        // Fetch content from search result URLs, trying each one until we get valid content
        for (size_t url_idx = 0; url_idx < urls.size(); url_idx++) {
            if (urls[url_idx].empty() || urls[url_idx].find("http") != 0) {
                continue;
            }
            LOG_WEBSEARCH("Fetching content directly in C++ from result " + std::to_string(url_idx + 1) + ": " + urls[url_idx]);

            std::string raw_html = fetch_url(urls[url_idx]);
            if (!raw_html.empty()) {
                    std::string page_content = decode_html_entities(strip_html_tags(raw_html));
                if (!page_content.empty()) {
                    if (page_content.length() > 3000) {
                        page_content = page_content.substr(0, 2997) + "...";
                    }
                    results += "\n\n---\nContent from " + urls[url_idx] + ":\n" + page_content;
                    break; // Successfully got content, stop trying more URLs
                }
            }
            LOG_WEBSEARCH("No valid content from result " + std::to_string(url_idx + 1) + ", trying next result...");
        }

        // Compact format for TTS (but keep detailed content for LLM)
        if (results.length() > 3000) {
            results = results.substr(0, 2997) + "...";
        }
        return {true, results, ""};
    }

    // Last resort: strip HTML and take first meaningful text
    std::string raw_text = strip_html_tags(response);
    // Find first meaningful content (skip navigation/header text)
    size_t content_start = 0;
    if (raw_text.length() > 500) {
        // Try to find the search result area
        size_t result_pos = raw_text.find("result");
        if (result_pos == std::string::npos) result_pos = 0;
        raw_text = raw_text.substr(result_pos, 500);
    }

    return {true, "Search results for: " + query + "\n" + raw_text, ""};
}

} // namespace Jarvis