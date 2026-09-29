#pragma once
#include "Tool.h"

namespace Jarvis {

class VolumeControlTool : public Tool {
public:
    std::string getName() const override { return "volume_control"; }
    std::string getDescription() const override {
        return "Control the system volume. Actions: 'get' (returns current volume), 'set' (requires level 0-100), 'mute', 'unmute'.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
