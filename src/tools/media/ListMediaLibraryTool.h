#pragma once
#include "Tool.h"
#include <string>
#include <vector>

namespace Jarvis {

class ListMediaLibraryTool : public Tool {
public:
    std::string getName() const override { return "list_media_library"; }
    std::string getDescription() const override {
        return "Browse Sonny's registered media libraries. Use when the user asks "
               "'what music do I have?', 'what movies are available?', 'list my media folders', "
               "'search my library for X', 'what y do you know?', etc. "
               "Actions: "
               "'libraries' - show all registered media library paths with type and file count; "
               "'contents' - list all media files in a specific library or path; "
               "'search' - fuzzy-search filenames across all registered libraries. "
               "For 'search', set search_term to exactly what the user asked for.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
