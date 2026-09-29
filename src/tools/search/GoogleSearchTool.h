#pragma once

#include "Tool.h"

namespace Jarvis {

class GoogleSearchTool : public Tool {
public:
    std::string getName() const override { return "google_search"; }
    std::string getDescription() const override { return "Searches Google via the automation browser and returns scraped results. Set open_first=true to open the best result in the same interactive Chromium session."; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
