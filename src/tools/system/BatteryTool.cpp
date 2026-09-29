#include "BatteryTool.h"
#include <windows.h>
#include <iostream>
#include <sstream>

namespace Jarvis {

std::vector<ToolParameter> BatteryTool::getParameters() const {
    return {};  // No params needed
}

ToolResult BatteryTool::execute(const std::map<std::string, std::string>& params) {
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) {
        return {false, "", "Failed to read battery status."};
    }

    // AC line status
    std::string power_source;
    if (sps.ACLineStatus == 1) {
        power_source = "plugged in";
    } else if (sps.ACLineStatus == 0) {
        power_source = "on battery";
    } else {
        power_source = "unknown power source";
    }

    // Battery percent
    std::string level_str;
    if (sps.BatteryLifePercent == 255) {
        level_str = "unknown";
    } else {
        level_str = std::to_string(sps.BatteryLifePercent) + "%";
    }

    // Battery flag
    std::string status;
    if (sps.BatteryFlag & 8) {
        status = "charging";
    } else if (sps.BatteryFlag & 128) {
        status = "no battery installed";
    } else if (sps.BatteryFlag & 1) {
        status = "high";
    } else if (sps.BatteryFlag & 2) {
        status = "low";
    } else if (sps.BatteryFlag & 4) {
        status = "critical";
    } else {
        status = "normal";
    }

    // Estimated time remaining
    std::string time_str;
    if (sps.BatteryLifeTime != 0xFFFFFFFF) {
        int total_seconds = sps.BatteryLifeTime;
        int hours = total_seconds / 3600;
        int minutes = (total_seconds % 3600) / 60;
        if (hours > 0) {
            time_str = " (" + std::to_string(hours) + "h " + std::to_string(minutes) + "m remaining)";
        } else {
            time_str = " (" + std::to_string(minutes) + " minutes remaining)";
        }
    }

    std::string result = "Battery: " + level_str + ", " + status + ", " + power_source + time_str;
    return {true, result, ""};
}

} // namespace Jarvis
