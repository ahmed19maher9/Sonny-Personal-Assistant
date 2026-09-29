#pragma once
#include "Tool.h"

namespace Jarvis {

class JokeTool : public Tool {
public:
    std::string getName() const override { return "tell_joke"; }
    std::string getDescription() const override {
        return "Tell a random joke. No parameters needed. Category param optional: 'programming', 'general', 'pun'.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
