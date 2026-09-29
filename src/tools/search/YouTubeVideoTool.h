#pragma once

#include "Tool.h"

namespace Jarvis {

class YouTubeVideoTool : public Tool {
public:
    std::string getName() const override { return "youtube_video"; }
    std::string getDescription() const override { return "Searches YouTube via the automation browser, opens the best-matching (or Nth) video result, and starts playback in the same interactive Chromium session."; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
