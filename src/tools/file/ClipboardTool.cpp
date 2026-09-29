#include "ClipboardTool.h"
#include <windows.h>
#include <iostream>

namespace Jarvis {

std::vector<ToolParameter> ClipboardTool::getParameters() const {
    return {
        {"action", "string", "The action to perform: 'get' or 'set'", true, ""},
        {"text", "string", "The text to set (only for 'set' action)", false, ""}
    };
}

ToolResult ClipboardTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    if (action_it == params.end()) {
        return {false, "", "Missing required parameter: action"};
    }
    
    std::string action = action_it->second;
    
    if (action == "get") {
        if (!OpenClipboard(nullptr)) {
            return {false, "", "Failed to open clipboard"};
        }
        
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData == nullptr) {
            CloseClipboard();
            return {false, "", "No text data in clipboard"};
        }
        
        char* pszText = static_cast<char*>(GlobalLock(hData));
        if (pszText == nullptr) {
            CloseClipboard();
            return {false, "", "Failed to lock clipboard data"};
        }
        
        std::string text(pszText);
        GlobalUnlock(hData);
        CloseClipboard();
        
        return {true, text, ""};
    } else if (action == "set") {
        auto text_it = params.find("text");
        if (text_it == params.end()) {
            return {false, "", "Missing required parameter: text for 'set' action"};
        }
        
        std::string text = text_it->second;
        
        if (!OpenClipboard(nullptr)) {
            return {false, "", "Failed to open clipboard"};
        }
        
        EmptyClipboard();
        
        HGLOBAL hClipboardData = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hClipboardData == nullptr) {
            CloseClipboard();
            return {false, "", "Failed to allocate memory for clipboard data"};
        }
        
        char* pszText = static_cast<char*>(GlobalLock(hClipboardData));
        if (pszText == nullptr) {
            GlobalFree(hClipboardData);
            CloseClipboard();
            return {false, "", "Failed to lock clipboard memory"};
        }
        
        strcpy(pszText, text.c_str());
        GlobalUnlock(hClipboardData);
        
        SetClipboardData(CF_TEXT, hClipboardData);
        CloseClipboard();
        
        return {true, "Clipboard set successfully", ""};
    } else {
        return {false, "", "Invalid action: " + action + ". Must be 'get' or 'set'"};
    }
}

} // namespace Jarvis
