#pragma once
#include "Tool.h"

namespace Jarvis {

class NoteTool : public Tool {
public:
    std::string getName() const override { return "notes"; }
    std::string getDescription() const override {
        return "Manage personal notes saved locally. Actions: 'add' (requires 'text'), 'list' (list all), 'read' (read by index or all), 'delete' (by index), 'clear' (delete all).";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;

private:
    std::string get_notes_dir() const;
};

} // namespace Jarvis
