#pragma once

#include "Tool.h"

namespace Jarvis {

class FileTool : public Tool {
public:
    std::string getName() const override { return "file_operation"; }
    std::string getDescription() const override { return "Performs file operations: read, write, list, delete"; }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
