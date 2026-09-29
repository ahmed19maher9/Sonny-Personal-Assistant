#pragma once

#include "Tool.h"

namespace Jarvis {

class ClipboardTool : public Tool {
public:
    std::string getName() const override { return "clipboard"; }
    std::string getDescription() const override { return "Gets or sets the clipboard text content"; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
