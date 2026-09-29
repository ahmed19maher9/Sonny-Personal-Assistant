#pragma once
#include "Tool.h"

namespace Jarvis {

class ScreenControlTool : public Tool {
public:
    std::string getName() const override { return "screen_control"; }
    std::string getDescription() const override {
        return "Control the screen and OS power state. Actions: 'lock' (lock workstation), 'screenshot' (save to Desktop), 'sleep' (system sleep), 'shutdown' (power off), 'restart' (reboot), 'brightness_up', 'brightness_down' (brightness control via Action Center).";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
