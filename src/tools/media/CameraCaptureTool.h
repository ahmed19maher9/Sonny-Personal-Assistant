#pragma once

#include "Tool.h"

namespace Jarvis {

// Camera capabilities: take a photo, record video with start/stop, and open
// the folder or the file that came out of it. Reaches the camera through
// CameraCapture::active() (the orchestrator owns the one capture instance).
class CameraCaptureTool : public Tool {
public:
    std::string getName() const override { return "camera"; }
    std::string getDescription() const override;
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
