#pragma once
#include "Tool.h"

namespace Jarvis {

class WifiTool : public Tool {
public:
    std::string getName() const override { return "wifi_info"; }
    std::string getDescription() const override {
        return "Get WiFi network information. Actions: 'current' (show connected network name and signal), 'list' (list available networks).";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
