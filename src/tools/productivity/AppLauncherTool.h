#pragma once
#include "Tool.h"

namespace Jarvis {

class AppLauncherTool : public Tool {
public:
    std::string getName() const override { return "app_launcher"; }
    std::string getDescription() const override {
        return "Launch an installed Windows application by its friendly name (e.g. 'notepad', 'chrome', 'spotify', 'calculator', 'paint', 'vlc', 'word', 'excel').";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
