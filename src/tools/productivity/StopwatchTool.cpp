#include "StopwatchTool.h"
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>

namespace Jarvis {

StopwatchTool::StopwatchTool() : accumulated_(std::chrono::duration<double>::zero()) {}

std::vector<ToolParameter> StopwatchTool::getParameters() const {
    return {
        {"action", "string", "Action: 'start', 'stop', 'lap', 'reset', 'status'", true, "status"}
    };
}

std::string StopwatchTool::format_duration(double seconds) const {
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    double s = seconds - h * 3600 - m * 60;

    std::ostringstream oss;
    if (h > 0) {
        oss << h << " hour" << (h != 1 ? "s" : "") << ", ";
        oss << m << " minute" << (m != 1 ? "s" : "") << ", ";
        oss << std::fixed << std::setprecision(2) << s << " seconds";
    } else if (m > 0) {
        oss << m << " minute" << (m != 1 ? "s" : "") << ", ";
        oss << std::fixed << std::setprecision(2) << s << " seconds";
    } else {
        oss << std::fixed << std::setprecision(2) << s << " seconds";
    }
    return oss.str();
}

ToolResult StopwatchTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "status";
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    if (action == "start") {
        if (running_) {
            return {true, "Stopwatch is already running.", ""};
        }
        start_time_ = std::chrono::steady_clock::now();
        running_ = true;
        return {true, "Stopwatch started.", ""};

    } else if (action == "stop") {
        if (!running_) {
            double total = accumulated_.count();
            return {true, "Stopwatch is stopped at " + format_duration(total), ""};
        }
        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start_time_;
        accumulated_ += elapsed;
        running_ = false;
        return {true, "Stopwatch stopped at " + format_duration(accumulated_.count()), ""};

    } else if (action == "lap") {
        if (!running_) {
            return {false, "", "Stopwatch is not running. Start it first."};
        }
        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start_time_;
        double total = accumulated_.count() + elapsed.count();
        lap_count_++;
        return {true, "Lap " + std::to_string(lap_count_) + ": " + format_duration(total), ""};

    } else if (action == "reset") {
        running_ = false;
        accumulated_ = std::chrono::duration<double>::zero();
        lap_count_ = 0;
        return {true, "Stopwatch reset to zero.", ""};

    } else if (action == "status") {
        if (running_) {
            std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start_time_;
            double total = accumulated_.count() + elapsed.count();
            return {true, "Stopwatch is running: " + format_duration(total), ""};
        } else {
            double total = accumulated_.count();
            if (total == 0.0) {
                return {true, "Stopwatch is not started.", ""};
            }
            return {true, "Stopwatch is stopped at " + format_duration(total), ""};
        }
    }

    return {false, "", "Unknown action: " + action + ". Use: start, stop, lap, reset, status."};
}

} // namespace Jarvis
