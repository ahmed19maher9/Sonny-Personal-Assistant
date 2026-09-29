#pragma once
#include "Tool.h"
#include <chrono>
#include <atomic>

namespace Jarvis {

class StopwatchTool : public Tool {
public:
    StopwatchTool();

    std::string getName() const override { return "stopwatch"; }
    std::string getDescription() const override {
        return "Control a stopwatch. Actions: 'start' (begin timing), 'stop' (stop and show elapsed), 'lap' (record a lap time), 'reset' (reset to zero), 'status' (show current elapsed time).";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;

private:
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::duration<double> accumulated_{0};
    std::atomic<bool> running_{false};
    int lap_count_ = 0;

    std::string format_duration(double seconds) const;
};

} // namespace Jarvis
