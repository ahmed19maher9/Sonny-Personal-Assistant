#include "ProcessTool.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <set>

#pragma comment(lib, "psapi.lib")

namespace Jarvis {

std::vector<ToolParameter> ProcessTool::getParameters() const {
    return {
        {"action", "string", "Action: 'list' or 'kill'", true, "list"},
        {"name",   "string", "Process name to kill (for 'kill' action, e.g. 'notepad.exe')", false, ""}
    };
}

ToolResult ProcessTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "list";
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return {false, "", "Failed to create process snapshot."};
    }

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (action == "list") {
        // Return a summary of user-facing processes (filter system/idle)
        std::set<std::string, std::less<>> system_procs = {
            "system", "system idle process", "smss.exe", "csrss.exe",
            "wininit.exe", "services.exe", "lsass.exe", "svchost.exe",
            "dwm.exe", "winlogon.exe", "fontdrvhost.exe", "sihost.exe"
        };

        std::vector<std::string> proc_names;
        std::set<std::string> seen;

        if (Process32First(hSnap, &pe)) {
            do {
                std::string name = pe.szExeFile;
                std::string lower_name = name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

                if (system_procs.find(lower_name) == system_procs.end() && seen.find(lower_name) == seen.end()) {
                    proc_names.push_back(name);
                    seen.insert(lower_name);
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);

        if (proc_names.empty()) {
            return {true, "No user processes found.", ""};
        }

        // Sort alphabetically
        std::sort(proc_names.begin(), proc_names.end(), [](const std::string& a, const std::string& b) {
            std::string la = a, lb = b;
            std::transform(la.begin(), la.end(), la.begin(), ::tolower);
            std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
            return la < lb;
        });

        // Limit to first 20 for voice output
        std::string result = "Running processes (" + std::to_string(proc_names.size()) + " total): ";
        int count = std::min((int)proc_names.size(), 20);
        for (int i = 0; i < count; ++i) {
            result += proc_names[i];
            if (i < count - 1) result += ", ";
        }
        if ((int)proc_names.size() > count) {
            result += " (and " + std::to_string(proc_names.size() - count) + " more)";
        }
        return {true, result, ""};

    } else if (action == "kill") {
        auto name_it = params.find("name");
        if (name_it == params.end() || name_it->second.empty()) {
            CloseHandle(hSnap);
            return {false, "", "Missing 'name' parameter for kill action."};
        }

        std::string target = name_it->second;
        // Add .exe if needed
        if (target.find('.') == std::string::npos) target += ".exe";

        std::string target_lower = target;
        std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), ::tolower);

        int killed = 0;
        if (Process32First(hSnap, &pe)) {
            do {
                std::string proc_name = pe.szExeFile;
                std::string proc_lower = proc_name;
                std::transform(proc_lower.begin(), proc_lower.end(), proc_lower.begin(), ::tolower);

                if (proc_lower == target_lower) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        if (TerminateProcess(hProc, 0)) killed++;
                        CloseHandle(hProc);
                    }
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);

        if (killed > 0) {
            return {true, "Terminated " + std::to_string(killed) + " instance" + (killed > 1 ? "s" : "") + " of " + target, ""};
        }
        return {false, "", "Process '" + target + "' not found."};
    }

    CloseHandle(hSnap);
    return {false, "", "Unknown action: " + action + ". Use 'list' or 'kill'."};
}

} // namespace Jarvis
