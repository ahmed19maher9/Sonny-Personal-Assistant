#include "TimeTool.h"
#include <iostream>
#include <sstream>
#include <ctime>

namespace Jarvis {

std::vector<ToolParameter> TimeTool::getParameters() const {
    return {
        {"format", "string", "The format for the time output (default: 'full'). Options: 'full', 'date', 'time', 'day'", false, "full"}
    };
}

ToolResult TimeTool::execute(const std::map<std::string, std::string>& params) {
    auto it = params.find("format");
    std::string format = (it != params.end()) ? it->second : "full";
    
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now);
    
    char buffer[128];
    std::string result;
    
    if (format == "date") {
        // Friendly: Monday, 4 July 2026
        strftime(buffer, sizeof(buffer), "%A, %d %B %Y", &timeinfo);
        result = buffer;
    } else if (format == "time") {
        // 12-hour with AM/PM
        strftime(buffer, sizeof(buffer), "%I:%M %p", &timeinfo);
        result = buffer;
        // Remove leading zero from hour
        if (result[0] == '0') result = result.substr(1);
    } else if (format == "day") {
        strftime(buffer, sizeof(buffer), "%A", &timeinfo);
        result = std::string("Today is ") + buffer;
    } else {
        // Full: Wednesday, 4 July 2026 at 2:34 PM
        char day_buf[64], date_buf[64], time_buf[32];
        strftime(day_buf,  sizeof(day_buf),  "%A",        &timeinfo);
        strftime(date_buf, sizeof(date_buf), "%d %B %Y",  &timeinfo);
        strftime(time_buf, sizeof(time_buf), "%I:%M %p",  &timeinfo);
        // Remove leading zero from hour
        std::string time_str = time_buf;
        if (!time_str.empty() && time_str[0] == '0') time_str = time_str.substr(1);
        result = std::string(day_buf) + ", " + date_buf + " at " + time_str;
    }
    
    return {true, result, ""};
}

} // namespace Jarvis
