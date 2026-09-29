#include "YouTubeVideoTool.h"
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

std::vector<ToolParameter> YouTubeVideoTool::getParameters() const {
    return {
        {"query", "string", "The search query to find a video on YouTube", true, ""},
        {"index", "string", "1-based position of the result to open (default: 1)", false, "1"},
        {"timeout", "string", "Search result read timeout in ms", false, "12000"},
    };
}

ToolResult YouTubeVideoTool::execute(const std::map<std::string, std::string>& params) {
    const std::string query = param_str(params, "query");
    if (query.empty()) {
        try {
            const json result = Jarvis::Flows::open_site("youtube");
            return {true, result.dump(), ""};
        } catch (const std::exception& error) {
            return {false, "", error.what()};
        }
    }

    try {
        const int index = std::atoi(param_str(params, "index", "1").c_str());
        json result = Jarvis::Flows::search(
            "youtube", query, false, false,
            std::atoi(param_str(params, "timeout", "12000").c_str()));
        result["opened"] = Jarvis::Flows::open_result("youtube", query, index);
        result["playback"] = Jarvis::Flows::control("play");
        return {true, result.dump(), ""};
    } catch (const std::exception& error) {
        return {false, "", error.what()};
    }
}

} // namespace Jarvis