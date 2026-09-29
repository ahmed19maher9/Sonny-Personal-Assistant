#pragma once

#include "Tool.h"

namespace Jarvis {

class CalculatorTool : public Tool {
public:
    std::string getName() const override { return "calculator"; }
    std::string getDescription() const override { return "Evaluates a mathematical expression"; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
