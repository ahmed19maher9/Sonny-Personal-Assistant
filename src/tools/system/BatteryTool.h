#pragma once
#include "Tool.h"

namespace Jarvis {

class BatteryTool : public Tool {
public:
    std::string getName() const override { return "battery_status"; }
    std::string getDescription() const override {
        return "Get the current battery status: charge level, charging state, and estimated time remaining.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
