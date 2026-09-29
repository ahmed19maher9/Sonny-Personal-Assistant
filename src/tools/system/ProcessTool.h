#pragma once
#include "Tool.h"

namespace Jarvis {

class ProcessTool : public Tool {
public:
    std::string getName() const override { return "process_manager"; }
    std::string getDescription() const override {
        return "Manage running Windows processes. Actions: 'list' (list all running processes), 'kill' (terminate process by name, requires 'name' param).";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
