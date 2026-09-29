#include "FileTool.h"
#include <iostream>
#include <fstream>
#include <filesystem>

namespace Jarvis {

std::vector<ToolParameter> FileTool::getParameters() const {
    return {
        {"action", "string", "The action to perform: 'read', 'write', 'list', 'delete'", true, ""},
        {"path", "string", "The file or directory path", true, ""},
        {"content", "string", "The content to write (only for 'write' action)", false, ""}
    };
}

ToolResult FileTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    if (action_it == params.end()) {
        return {false, "", "Missing required parameter: action"};
    }
    
    auto path_it = params.find("path");
    if (path_it == params.end()) {
        return {false, "", "Missing required parameter: path"};
    }
    
    std::string action = action_it->second;
    std::string path = path_it->second;
    
    if (action == "read") {
        std::ifstream file(path);
        if (!file.is_open()) {
            return {false, "", "Failed to open file: " + path};
        }
        
        std::string content((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
        file.close();
        
        return {true, content, ""};
    } else if (action == "write") {
        auto content_it = params.find("content");
        if (content_it == params.end()) {
            return {false, "", "Missing required parameter: content for 'write' action"};
        }
        
        std::ofstream file(path);
        if (!file.is_open()) {
            return {false, "", "Failed to create file: " + path};
        }
        
        file << content_it->second;
        file.close();
        
        return {true, "File written successfully: " + path, ""};
    } else if (action == "list") {
        std::string result;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                result += entry.path().string();
                if (entry.is_directory()) {
                    result += " [DIR]";
                }
                result += "\n";
            }
        } catch (const std::filesystem::filesystem_error& e) {
            return {false, "", "Failed to list directory: " + std::string(e.what())};
        }
        
        return {true, result, ""};
    } else if (action == "delete") {
        try {
            if (std::filesystem::remove(path)) {
                return {true, "File deleted successfully: " + path, ""};
            } else {
                return {false, "", "Failed to delete file: " + path};
            }
        } catch (const std::filesystem::filesystem_error& e) {
            return {false, "", "Failed to delete file: " + std::string(e.what())};
        }
    } else {
        return {false, "", "Invalid action: " + action + ". Must be 'read', 'write', 'list', or 'delete'"};
    }
}

} // namespace Jarvis
