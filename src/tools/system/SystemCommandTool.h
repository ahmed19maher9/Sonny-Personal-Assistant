#pragma once

#include "Tool.h"

namespace Jarvis {

class SystemCommandTool : public Tool {
public:
    std::string getName() const override { return "system_command"; }
    std::string getDescription() const override { return "Executes a system command (use with caution)"; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
