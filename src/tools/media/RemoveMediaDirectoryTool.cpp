#include "RemoveMediaDirectoryTool.h"
#include "Tool.h"
#include "RagEngine.h"
#include "Logger.h"
#include "MediaUtil.h"
#include <algorithm>
#include <cctype>

namespace Jarvis {

std::vector<ToolParameter> RemoveMediaDirectoryTool::getParameters() const {
    return {
        {"path", "string",
         "Path or partial name of the media library to remove "
         "(e.g. 'Music', 'D:\\Movies', or the full path).",
         true, ""}
    };
}

ToolResult RemoveMediaDirectoryTool::execute(const std::map<std::string, std::string>& params) {
    std::string raw;
    for (const char* key : {"path", "directory", "folder", "name"}) {
        auto it = params.find(key);
        if (it != params.end() && !it->second.empty()) { raw = it->second; break; }
    }
    raw = trim_copy(raw);
    if (raw.empty())
        return {false, "", "No directory specified. Tell me which media library to remove."};

    ToolRegistry& registry = ToolRegistry::getInstance();
    RagEngine* rag = registry.getRagEngine();
    if (!rag) return {false, "", "The knowledge base is not available."};

    const auto dirs = registry.getMediaDirectories();
    if (dirs.empty())
        return {false, "", "You have no registered media libraries."};

    std::string raw_lower = lower_ascii(raw);
    std::replace(raw_lower.begin(), raw_lower.end(), '/', '\\');

    std::vector<std::string> matched;
    for (const auto& dir : dirs) {
        std::string dir_lower = lower_ascii(dir);
        if (dir_lower == raw_lower || dir_lower.find(raw_lower) != std::string::npos)
            matched.push_back(dir);
    }

    if (matched.empty()) {
        std::string list;
        for (size_t i = 0; i < dirs.size(); ++i)
            list += "\n  " + std::to_string(i+1) + ". " + dirs[i];
        return {false, "", "No media library matches \"" + raw + "\". Registered libraries:" + list};
    }

    std::string removed_list;
    int total_deleted = 0;
    for (const auto& dir : matched) {
        // Try all fact format variants
        for (const std::string& prefix : {
                "Media directory [music]: " + dir,
                "Media directory [video]: " + dir,
                "Media directory [media]: " + dir,
                "Media directory: " + dir}) {
            total_deleted += rag->deleteUserFact(prefix, "media_directories");
        }
        removed_list += "\n  - " + dir;
        LOG_SUCCESS("TOOL", "Removed media library: " + dir);
    }

    if (total_deleted == 0)
        return {false, "", "Could not remove \"" + raw + "\" — it may already have been removed."};

    return {true,
        "Done! Removed " + std::to_string(matched.size()) + " media librar" +
        (matched.size()==1?"y":"ies") + ":" + removed_list, ""};
}

} // namespace Jarvis
