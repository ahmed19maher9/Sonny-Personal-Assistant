#pragma once

#include "Tool.h"

namespace Jarvis {

class YouTubeSearchTool : public Tool {
public:
    std::string getName() const override { return "youtube_search"; }
    std::string getDescription() const override { return "Searches YouTube via the automation browser and returns scraped results. By default opens the first video and starts playback in the same interactive Chromium session."; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
