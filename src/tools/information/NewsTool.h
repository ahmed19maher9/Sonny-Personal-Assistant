#pragma once
#include "Tool.h"

namespace Jarvis {

class NewsTool : public Tool {
public:
    std::string getName() const override { return "news"; }
    std::string getDescription() const override {
        return "Fetch the latest news headlines from BBC News RSS feed (no API key required). Param 'count' controls number of headlines (default 5). Param 'category': 'world', 'tech', 'business', 'health', 'science' (default 'world').";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
