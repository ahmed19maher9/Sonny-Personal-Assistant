#include "SystemCommandTool.h"
#include <windows.h>
#include <iostream>
#include <sstream>
#include <array>

namespace Jarvis {

std::vector<ToolParameter> SystemCommandTool::getParameters() const {
    return {
        {"command", "string", "The system command to execute", true, ""}
    };
}

ToolResult SystemCommandTool::execute(const std::map<std::string, std::string>& params) {
    auto it = params.find("command");
    if (it == params.end()) {
        return {false, "", "Missing required parameter: command"};
    }
    
    std::string command = it->second;
    
    // Execute the command using Windows API
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return {false, "", "Failed to create pipe"};
    }
    
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;
    
    char cmdLine[MAX_PATH];
    strcpy(cmdLine, command.c_str());
    
    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE, 
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return {false, "", "Failed to execute command"};
    }
    
    CloseHandle(hWritePipe);
    
    // Read output
    std::string result;
    char buffer[4096];
    DWORD bytesRead;
    
    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        result += buffer;
    }
    
    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    return {true, result, ""};
}

} // namespace Jarvis
