#pragma once

#include "Tool.h"

namespace Jarvis {

class TimeTool : public Tool {
public:
    std::string getName() const override { return "get_time"; }
    std::string getDescription() const override { return "Gets the current date and time"; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
