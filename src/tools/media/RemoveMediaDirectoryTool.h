#pragma once
#include "Tool.h"
#include <string>
#include <vector>

namespace Jarvis {

class RemoveMediaDirectoryTool : public Tool {
public:
    std::string getName() const override { return "remove_media_directory"; }
    std::string getDescription() const override {
        return "Removes a previously registered media library folder from Sonny's knowledge base. "
               "Use when the user says 'remove my music directory', 'forget my movies folder', "
               "'unregister X from my media library', etc. "
               "Pass the path or folder name as the user described it. Partial names are accepted "
               "Never removes a folder the user did not explicitly ask to remove.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
