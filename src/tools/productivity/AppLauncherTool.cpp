#include "AppLauncherTool.h"
#include "Logger.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <algorithm>
#include <map>

namespace Jarvis {

std::vector<ToolParameter> AppLauncherTool::getParameters() const {
    return {
        {"name", "string", "The name of the application to launch (e.g. 'notepad', 'chrome', 'calculator', 'explorer')", true, ""}
    };
}

ToolResult AppLauncherTool::execute(const std::map<std::string, std::string>& params) {
    auto it = params.find("name");
    if (it == params.end() || it->second.empty()) {
        return {false, "", "Missing required parameter: name"};
    }

    std::string name = it->second;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Trim whitespace
    lower.erase(0, lower.find_first_not_of(" \t\n\r"));
    lower.erase(lower.find_last_not_of(" \t\n\r") + 1);

    // Map of friendly names to executable names / special shell commands
    static const std::map<std::string, std::string> app_map = {
        // System apps
        {"notepad", "notepad.exe"},
        {"calculator", "calc.exe"},
        {"calc", "calc.exe"},
        {"paint", "mspaint.exe"},
        {"mspaint", "mspaint.exe"},
        {"wordpad", "wordpad.exe"},
        {"explorer", "explorer.exe"},
        {"file explorer", "explorer.exe"},
        {"task manager", "taskmgr.exe"},
        {"taskmgr", "taskmgr.exe"},
        {"registry editor", "regedit.exe"},
        {"regedit", "regedit.exe"},
        {"command prompt", "cmd.exe"},
        {"cmd", "cmd.exe"},
        {"powershell", "powershell.exe"},
        {"control panel", "control.exe"},
        {"snipping tool", "snippingtool.exe"},
        {"snip", "snippingtool.exe"},
        {"magnifier", "magnify.exe"},
        {"narrator", "narrator.exe"},
        {"on-screen keyboard", "osk.exe"},
        {"character map", "charmap.exe"},
        {"disk cleanup", "cleanmgr.exe"},
        {"device manager", "devmgmt.msc"},
        {"event viewer", "eventvwr.msc"},
        {"services", "services.msc"},
        {"disk management", "diskmgmt.msc"},
        {"clock", "ms-clock:"},
        {"alarm", "ms-clock:"},
        {"settings", "ms-settings:"},
        {"store", "ms-windows-store:"},

        // Browsers
        {"chrome", "chrome.exe"},
        {"google chrome", "chrome.exe"},
        {"firefox", "firefox.exe"},
        {"mozilla firefox", "firefox.exe"},
        {"edge", "msedge.exe"},
        {"microsoft edge", "msedge.exe"},
        {"brave", "brave.exe"},
        {"opera", "opera.exe"},

        // Office / productivity
        {"word", "winword.exe"},
        {"excel", "excel.exe"},
        {"powerpoint", "powerpnt.exe"},
        {"outlook", "outlook.exe"},
        {"onenote", "onenote.exe"},
        {"teams", "teams.exe"},
        {"microsoft teams", "teams.exe"},
        {"access", "msaccess.exe"},
        {"publisher", "mspub.exe"},

        // Media
        {"vlc", "vlc.exe"},
        {"spotify", "spotify.exe"},
        {"media player", "wmplayer.exe"},
        {"windows media player", "wmplayer.exe"},
        {"photos", "ms-photos:"},
        {"groove music", "mswindowsmusic:"},

        // Dev tools
        {"visual studio code", "code.exe"},
        {"vscode", "code.exe"},
        {"code", "code.exe"},
        {"visual studio", "devenv.exe"},
        {"git bash", "git-bash.exe"},
        {"notepad++", "notepad++.exe"},

        // Communication
        {"discord", "discord.exe"},
        {"slack", "slack.exe"},
        {"zoom", "zoom.exe"},
        {"skype", "skype.exe"},
        {"telegram", "telegram.exe"},
        {"whatsapp", "whatsapp.exe"},

        // Other utilities
        {"steam", "steam.exe"},
        {"winrar", "winrar.exe"},
        {"7-zip", "7zFM.exe"},
        {"7zip", "7zFM.exe"},
        {"obs", "obs64.exe"},
    };

    std::string executable;
    auto app_it = app_map.find(lower);
    if (app_it != app_map.end()) {
        executable = app_it->second;
    } else {
        // Try the raw name (user might say exact exe name)
        executable = name;
        // Append .exe if no extension
        if (executable.find('.') == std::string::npos) {
            executable += ".exe";
        }
    }

    std::ostringstream oss;
    oss << "Launching: " << executable;
    LOG_APPLAUNCHER(oss.str());

    // Use ShellExecute to launch with default open verb
    HINSTANCE result = ShellExecuteA(NULL, "open", executable.c_str(), NULL, NULL, SW_SHOWNORMAL);

    if ((intptr_t)result > 32) {
        return {true, "Launched " + name, ""};
    }

    // Try with full path search using CreateProcess fallback
    // Some UWP/modern apps use protocol handlers (ms-settings:, etc.)
    if (executable.find(':') != std::string::npos) {
        result = ShellExecuteA(NULL, "open", executable.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((intptr_t)result > 32) {
            return {true, "Launched " + name, ""};
        }
    }

    return {false, "", "Could not launch '" + name + "'. Application not found or not installed."};
}

} // namespace Jarvis
