#include "GoogleSearchTool.h"
#include "SiteFlows.h"
#include <json.hpp>
#include <cstdlib>
#include <string>
#include <map>

namespace Jarvis {

using json = nlohmann::json;

namespace {

std::string param_str(const std::map<std::string, std::string>& params,
                      const std::string& key, const std::string& fallback = "") {
    auto it = params.find(key);
    return it == params.end() ? fallback : it->second;
}

bool param_bool(const std::map<std::string, std::string>& params,
                const std::string& key) {
    const std::string val = param_str(params, key);
    return val == "true" || val == "1" || val == "yes";
}

} // namespace

std::vector<ToolParameter> GoogleSearchTool::getParameters() const {
    return {
        {"query", "string", "The search query to search for on Google", true, ""},
        {"open_first", "string", "'true' to open the first result in the browser", false, "false"},
        {"play", "string", "'true' to start playback of the opened result (YouTube)", false, "false"},
        {"timeout", "string", "Search result read timeout in ms", false, "12000"},
    };
}

ToolResult GoogleSearchTool::execute(const std::map<std::string, std::string>& params) {
    const std::string query = param_str(params, "query");
    if (query.empty()) {
        return {false, "", "Missing required parameter: query"};
    }

    try {
        const json result = Jarvis::Flows::search(
            "google", query, param_bool(params, "open_first"),
            param_bool(params, "play"),
            std::atoi(param_str(params, "timeout", "12000").c_str()));
        return {true, result.dump(), ""};
    } catch (const std::exception& error) {
        return {false, "", error.what()};
    }
}

} // namespace Jarvis