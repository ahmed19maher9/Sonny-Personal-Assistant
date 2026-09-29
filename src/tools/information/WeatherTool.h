#pragma once
#include "Tool.h"

namespace Jarvis {

class WeatherTool : public Tool {
public:
    std::string getName() const override { return "weather"; }
    std::string getDescription() const override {
        return "Get current weather for a location. Uses wttr.in (no API key). Param 'location' defaults to auto-detect by IP.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
