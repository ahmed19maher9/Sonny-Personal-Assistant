#include "BrowserTool.h"
#include "BrowserServiceImpl.h"
#include "SiteFlows.h"
#include "Logger.h"

#include <json.hpp>  // nlohmann (vendored in resources/avatars/, on the include path)
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace Jarvis {

namespace {

using json = nlohmann::json;

// BrowserTool is the single browser action surface: every browser interaction
// (open, navigate, search, open-result, scroll, back, forward, reload, and all
// CDP-level actions) routes through this one tool. The browser service itself
// is shared with SiteFlows via Flows::shared_service(), so site "recipes" and
// low-level CDP calls operate on the same Chromium session — the LLM can search
// YouTube and then click a result without losing the page.
BrowserServiceImpl& browser_service() {
    return Flows::shared_service();
}

const char* kActionHelp =
    "open, navigate, search, open_result, new_tab, close_tab, tabs, switch_tab, "
    "read, find, summarize, eval, click, fill, scroll, key, actions, screenshot, "
    "pdf, wait, back, forward, reload, fields, autofill, hover, select, text, links, buttons";

std::string compact_json(const json& value, size_t max_chars) {
    std::string text = value.is_string() ? value.get<std::string>() : value.dump();
    if (text.size() > max_chars) {
        text.resize(max_chars);
        text += " ...[truncated]";
    }
    return text;
}

// arg_string for the flat map<string,string> tool interface.
std::string param(const std::map<std::string, std::string>& params, const char* key,
                  const std::string& fallback = {}) {
    auto it = params.find(key);
    return it == params.end() ? fallback : it->second;
}

std::string first_or(const json& obj, const char* key, const std::string& fallback) {
    auto it = obj.find(key);
    if (it != obj.end() && it->is_string() && !it->get<std::string>().empty()) {
        return it->get<std::string>();
    }
    return fallback;
}

ToolResult ok(std::string text) { return {true, std::move(text), {}}; }
ToolResult fail(std::string error) { return {false, {}, std::move(error)}; }

}  // namespace

namespace {

bool looks_like_local_path(const std::string& path) {
    if (path.empty()) return false;
    // Windows: "E:\", "E:", "C:\Users\...", "D:\folder\file.txt"
    // Linux/Mac: "/home/...", "/mnt/...", "/media/..."
    const char* p = path.c_str();
    // Windows drive letter with colon
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
        return true;
    }
    // Unix absolute path
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    return false;
}

std::string local_path_to_file_url(const std::string& path) {
    std::string url = "file:///";
    // Convert Windows backslashes to forward slashes
    for (char c : path) {
        if (c == '\\') url += '/';
        else url += c;
    }
    // Ensure Windows drive letter format: file:///E:/ or file:///C:/Users/...
    if (url.size() >= 9 && std::isalpha(static_cast<unsigned char>(url[8])) && url[9] == ':') {
        // Already in correct format like file:///E:/
        return url;
    }
    return url;
}

// Natural language preprocessing for better user experience
std::string preprocess_target(const std::string& raw) {
    std::string result = raw;
    
    // Remove common filler words
    static const std::vector<std::string> fillers = {
        "the ", "a ", "an ", "please ", "can you ", "could you ", 
        "i want to ", "i need to ", "please help me ", "help me "
    };
    
    for (const auto& filler : fillers) {
        size_t pos = result.find(filler);
        if (pos == 0) {
            result = result.substr(filler.length());
        }
    }
    
    // Trim leading/trailing whitespace
    size_t start = result.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\r\n");
    result = result.substr(start, end - start + 1);
    
    return result;
}

// Smart action detection from natural language
std::string detect_action_from_text(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // Click patterns
    if (lower.find("click") != std::string::npos || 
        lower.find("press") != std::string::npos ||
        lower.find("tap") != std::string::npos ||
        lower.find("select") != std::string::npos) {
        return "click";
    }
    
    // Fill patterns
    if (lower.find("fill") != std::string::npos || 
        lower.find("enter") != std::string::npos ||
        lower.find("type") != std::string::npos ||
        lower.find("input") != std::string::npos) {
        return "fill";
    }
    
    // Open patterns
    if (lower.find("open") != std::string::npos || 
        lower.find("go to") != std::string::npos ||
        lower.find("navigate") != std::string::npos ||
        lower.find("visit") != std::string::npos) {
        return "open";
    }
    
    // Search patterns
    if (lower.find("search") != std::string::npos || 
        lower.find("find") != std::string::npos ||
        lower.find("look for") != std::string::npos) {
        return "search";
    }
    
    // Scroll patterns
    if (lower.find("scroll") != std::string::npos || 
        lower.find("down") != std::string::npos ||
        lower.find("up") != std::string::npos) {
        return "scroll";
    }
    
    return "";
}

// Extract target from natural language (e.g., "click the sign in button" -> "sign in button")
std::string extract_target(const std::string& text, const std::string& action) {
    std::string result = text;
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // Remove action words
    size_t action_pos = lower.find(action);
    if (action_pos != std::string::npos) {
        result = result.substr(action_pos + action.length());
    }
    
    // Remove common prepositions and articles
    static const std::vector<std::string> remove_words = {
        " the ", " a ", " an ", " on ", " at ", " in ", " with ", " for "
    };
    
    for (const auto& word : remove_words) {
        size_t pos;
        while ((pos = result.find(word)) != std::string::npos) {
            result.replace(pos, word.length(), " ");
        }
    }
    
    // Clean up multiple spaces
    while (result.find("  ") != std::string::npos) {
        result.replace(result.find("  "), 2, " ");
    }
    
    // Trim
    size_t start = result.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(" \t\r\n");
    result = result.substr(start, end - start + 1);
    
    return result;
}

// Extract value from fill commands (e.g., "fill email with test@example.com" -> "test@example.com")
std::string extract_value(const std::string& text) {
    size_t with_pos = text.find(" with ");
    if (with_pos != std::string::npos) {
        return text.substr(with_pos + 6); // " with " is 6 characters
    }
    
    size_t as_pos = text.find(" as ");
    if (as_pos != std::string::npos) {
        return text.substr(as_pos + 4); // " as " is 4 characters
    }
    
    // Try to extract quoted value
    size_t quote_start = text.find('"');
    if (quote_start != std::string::npos) {
        size_t quote_end = text.find('"', quote_start + 1);
        if (quote_end != std::string::npos) {
            return text.substr(quote_start + 1, quote_end - quote_start - 1);
        }
    }
    
    return "";
}

}  // namespace

std::string BrowserTool::getDescription() const {
    return "User-friendly browser control: open websites by name or URL, click buttons "
           "using their text, fill forms by field labels, navigate links by text, "
           "search Google/YouTube, take screenshots, and more. Use natural language like "
           "'click sign in button', 'fill email with test@example.com', 'open youtube', "
           "'click the link about cats'. Smart element matching finds buttons, links, and "
           "fields by their visible text, labels, or attributes.";
}

std::vector<ToolParameter> BrowserTool::getParameters() const {
    return {
        {"action", "string",
         std::string("Which browser operation to run. One of: ") + kActionHelp,
         false, ""},
        {"command", "string", "Natural language command (e.g., 'click the sign in button', "
                            "'fill email with test@example.com', 'open youtube', 'search for cats'). "
                            "If provided, action will be auto-detected.", false, ""},
        {"url", "string", "Target URL or site name (action=open/navigate). "
                          "Site names like 'youtube', 'google' are resolved to their home page; "
                          "addresses like 'https://x.com' or 'youtube.com' are opened directly.",
         false, ""},
        {"site", "string", "Which site recipe to use for search/open_result "
                           "(e.g. 'youtube', 'google'). Auto-detected when blank.",
         false, ""},
        {"query", "string", "Search text (action=search). For spoken queries "
                            "like 'search for cats', pass the extracted phrase.",
         false, ""},
        {"title", "string", "Title of the result to open (action=open_result). "
                            "Fuzzy-matched against result titles; 'open the "
                            "second one' → set index=2 instead.", false, ""},
        {"new_tab", "string", "'true' to open in a new tab (action=open/navigate)", false, ""},
        {"match", "string", "Tab title/URL match (action=switch_tab) or CSS "
                            "selector (action=read/find/wait)", false, ""},
        {"target", "string", "Plain-language element target, e.g. 'search box', 'sign in button', "
                             "'email field', 'link about cats' (click/fill/find/wait/fields/hover/select)",
         false, ""},
        {"value", "string", "Text to type or option to pick (action=fill/select)", false, ""},
        {"index", "string", "1-based match index when several elements match (default 1)",
         false, ""},
        {"kind", "string", "What to read: text, links, tables, forms, metadata, html "
                           "(action=read)", false, ""},
        {"direction", "string", "Scroll direction: up/down/top/bottom (action=scroll)",
         false, ""},
        {"amount", "string", "Scroll amount in pixels (action=scroll)", false, ""},
        {"key", "string", "Key to press, e.g. Enter, Tab, Escape (action=key)", false, ""},
        {"actions", "string", "JSON array of steps for action=actions, e.g. "
                              "[{\"type\":\"fill\",\"target\":\"email\",\"value\":\"a@b.c\"},"
                              "{\"type\":\"click\",\"target\":\"sign in\"}]", false, ""},
        {"path", "string", "Where to save (actions=screenshot/pdf)", false, ""},
        {"full_page", "string", "'true' for a full-page screenshot", false, ""},
        {"timeout", "string", "Wait budget in milliseconds (action=wait)", false, ""},
        {"expression", "string", "JavaScript to evaluate in the page (action=eval)",
         false, ""},
        {"target_id", "string", "Tab id to close (action=close_tab)", false, ""},
        {"element_type", "string", "Element type for text/links/buttons actions: "
                                  "buttons, links, inputs, all (default: all)", false, ""},
    };
}

ToolResult BrowserTool::execute(const std::map<std::string, std::string>& params) {
    std::string action = param(params, "action");
    
    // Smart natural language processing: if action is not provided, try to detect it from other parameters
    if (action.empty()) {
        const std::string url = param(params, "url");
        const std::string target = param(params, "target");
        const std::string query = param(params, "query");
        
        // If URL is provided, it's likely an open/navigate action
        if (!url.empty()) {
            action = "open";
        }
        // If query is provided, it's a search action
        else if (!query.empty()) {
            action = "search";
        }
        // If target is provided, try to detect the action from context
        else if (!target.empty()) {
            const std::string value = param(params, "value");
            if (!value.empty()) {
                action = "fill";
            } else {
                action = "click";
            }
        }
        // If no action can be detected, provide helpful error
        else {
            return fail(std::string("Please specify what you want to do. Examples: ") +
                       "'open youtube', 'click the sign in button', 'fill email with test@example.com', " +
                       "'search for cats', 'scroll down'. Available actions: " + kActionHelp);
        }
    }

    auto& service = browser_service();
    try {
        json out;
        
        // Handle natural language commands by preprocessing
        std::string processed_target = preprocess_target(param(params, "target"));
        std::string processed_url = param(params, "url");
        std::string processed_query = param(params, "query");
        std::string processed_value = param(params, "value");
        std::string processed_site = param(params, "site");
        std::string processed_title = param(params, "title");
        
        // If the user provided a natural language command in the "command" parameter
        const std::string command = param(params, "command");
        if (!command.empty()) {
            std::string detected_action = detect_action_from_text(command);
            std::string extracted_target = extract_target(command, detected_action);
            std::string extracted_value = extract_value(command);
            
            if (!detected_action.empty()) {
                action = detected_action;
                if (!extracted_target.empty()) {
                    processed_target = extracted_target;
                }
                if (!extracted_value.empty()) {
                    processed_value = extracted_value;
                }
                // For search commands, extract the query
                if (detected_action == "search") {
                    processed_query = extracted_target; // The target becomes the search query
                    processed_target = ""; // Clear target for search
                }
                // For open commands, the target might be a URL or site name
                if (detected_action == "open") {
                    processed_url = extracted_target;
                    processed_target = "";
                }
            }
        }
        
        if (action == "open" || action == "navigate") {
            const std::string target = processed_url.empty() ? param(params, "url") : processed_url;
            if (target.empty()) {
                return fail("action \"" + action + "\" needs a url or site name");
            }
            if (looks_like_local_path(target)) {
                std::string file_url = local_path_to_file_url(target);
                out = service.open_page(file_url,
                                        param(params, "new_tab") == "true" ||
                                            param(params, "new_tab") == "1");
            } else if (Flows::looks_like_url(target)) {
                out = service.open_page(target,
                                        param(params, "new_tab") == "true" ||
                                            param(params, "new_tab") == "1");
            } else {
                out = Flows::open_site(target);
            }
        } else if (action == "search") {
            const std::string site = processed_site.empty() ? param(params, "site") : processed_site;
            const std::string query = processed_query.empty() ? param(params, "query") : processed_query;
            if (query.empty()) {
                return fail("action=search needs a query");
            }
            out = Flows::search(site, query, false, false,
                                std::atoi(param(params, "timeout", "12000").c_str()));
        } else if (action == "open_result") {
            const std::string site = processed_site.empty() ? param(params, "site") : processed_site;
            const std::string title = processed_title.empty() ? param(params, "title") : processed_title;
            out = Flows::open_result(site, title,
                                     std::atoi(param(params, "index", "0").c_str()));
        } else if (action == "new_tab") {
            out = service.new_tab();
        } else if (action == "close_tab") {
            out = service.close_page(param(params, "target_id"));
        } else if (action == "tabs") {
            out = service.list_pages();
        } else if (action == "switch_tab") {
            out = service.switch_page(param(params, "match"),
                                      std::atoi(param(params, "index", "0").c_str()));
        } else if (action == "read") {
            out = service.inspect_page(
                param(params, "kind", "text"), param(params, "match"),
                std::atoi(param(params, "limit", "40").c_str()));
        } else if (action == "find") {
            out = service.find_elements(processed_target.empty() ? param(params, "target") : processed_target,
                                        std::atoi(param(params, "index", "1").c_str()),
                                        param(params, "filter", "any"), false);
        } else if (action == "summarize") {
            out = service.summarize();
        } else if (action == "eval") {
            out = service.eval_js(param(params, "expression"));
        } else if (action == "click") {
            out = service.trusted_click(processed_target.empty() ? param(params, "target") : processed_target,
                                        std::atoi(param(params, "index", "1").c_str()));
        } else if (action == "fill") {
            out = service.fill_element(processed_target.empty() ? param(params, "target") : processed_target, 
                                       processed_value.empty() ? param(params, "value") : processed_value,
                                       std::atoi(param(params, "index", "1").c_str()));
        } else if (action == "scroll") {
            out = service.scroll_page(param(params, "direction", "down"),
                                      std::atoi(param(params, "amount", "0").c_str()));
        } else if (action == "key") {
            out = service.press_key_action(param(params, "key", "Enter"));
        } else if (action == "actions") {
            json parsed = json::parse(param(params, "actions"), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) {
                return fail("actions must be a JSON array of steps "
                            "({type, target, value, key, ...})");
            }
            out = service.batch_actions(parsed);
        } else if (action == "screenshot") {
            out = service.screenshot(param(params, "path"),
                                     param(params, "full_page") == "true");
        } else if (action == "pdf") {
            out = service.print_pdf(param(params, "path"));
        } else if (action == "wait") {
            out = service.wait_for(processed_target.empty() ? param(params, "target") : processed_target,
                                   std::atoi(param(params, "index", "1").c_str()),
                                   std::atoi(param(params, "timeout", "10000").c_str()));
        } else if (action == "back") {
            out = service.history_nav("back");
        } else if (action == "forward") {
            out = service.history_nav("forward");
        } else if (action == "reload") {
            out = service.history_nav("reload");
        } else if (action == "fields") {
            out = service.form_fields(processed_target.empty() ? param(params, "target") : processed_target);
        } else if (action == "autofill") {
            out = service.autofill();
        } else if (action == "hover") {
            std::string target = processed_target.empty() ? param(params, "target") : processed_target;
            if (target.empty()) {
                return fail("action=hover needs a target (e.g., 'hover over menu button')");
            }
            out = service.hover_element(target, std::atoi(param(params, "index", "1").c_str()));
        } else if (action == "select") {
            std::string target = processed_target.empty() ? param(params, "target") : processed_target;
            std::string value = processed_value.empty() ? param(params, "value") : processed_value;
            if (target.empty() || value.empty()) {
                return fail("action=select needs both target and value (e.g., select 'United States' from country dropdown)");
            }
            out = service.fill_element(target, value, std::atoi(param(params, "index", "1").c_str()));
        } else if (action == "text") {
            std::string target = processed_target.empty() ? param(params, "target") : processed_target;
            if (target.empty()) {
                out = service.inspect_page("text", param(params, "match"),
                                           std::atoi(param(params, "limit", "40").c_str()));
            } else {
                out = service.read_element(target, std::atoi(param(params, "index", "1").c_str()));
            }
        } else if (action == "links") {
            out = service.inspect_page("links", param(params, "match"),
                                       std::atoi(param(params, "limit", "40").c_str()));
        } else if (action == "buttons") {
            out = service.inspect_page("buttons", param(params, "match"),
                                       std::atoi(param(params, "limit", "40").c_str()));
        } else {
            return fail("unknown action \"" + action + "\" (use one of: " +
                        kActionHelp + ")");
        }

        if (!out.value("ok", false)) {
            return fail(first_or(out, "error", "browser action failed"));
        }
        return ok(compact_json(out, 6000));
    } catch (const std::exception& error) {
        LOG_WARN("BrowserTool", "action \"" + action + "\" failed: " + error.what());
        return fail(error.what());
    }
}

} // namespace Jarvis