#pragma once

#include "Tool.h"

namespace Jarvis {

class WebSearchTool : public Tool {
public:
    std::string getName() const override { return "web_search"; }
    std::string getDescription() const override { return "Searches the web for information and returns the results. Use this when you need current information or facts that may have changed recently."; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
