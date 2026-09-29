#include "NoteTool.h"
#include "Logger.h"
#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace Jarvis {
namespace fs = std::filesystem;

std::string NoteTool::get_notes_dir() const {
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
        std::string dir = std::string(appdata) + "\\Sonny\\notes";
        fs::create_directories(dir);
        return dir;
    }
    // Fallback
    std::string dir = "notes";
    fs::create_directories(dir);
    return dir;
}

std::vector<ToolParameter> NoteTool::getParameters() const {
    return {
        {"action", "string", "Action: 'add', 'list', 'read', 'delete', 'clear'", true, "list"},
        {"text",   "string", "Note text (for 'add' action)", false, ""},
        {"index",  "string", "Note number (for 'read' or 'delete' action)", false, ""}
    };
}

ToolResult NoteTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "list";

    std::string notes_dir = get_notes_dir();

    if (action == "add") {
        auto text_it = params.find("text");
        if (text_it == params.end() || text_it->second.empty()) {
            return {false, "", "Missing 'text' parameter for add action."};
        }

        // Create timestamped filename
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_info;
        localtime_s(&tm_info, &t);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);

        std::string filename = notes_dir + "\\note_" + ts + ".txt";
        std::ofstream out(filename);
        if (!out.is_open()) {
            return {false, "", "Failed to create note file."};
        }

        // Write timestamp header + content
        char readable_ts[64];
        strftime(readable_ts, sizeof(readable_ts), "%Y-%m-%d %H:%M:%S", &tm_info);
        out << "[" << readable_ts << "]\n" << text_it->second << "\n";
        out.close();

        LOG_DEBUG_COMPONENT("Notes", "Saved note to: " + filename);
        return {true, "Note saved: " + text_it->second, ""};

    } else if (action == "list" || action == "read") {
        // Collect all note files sorted by name (which is by time)
        std::vector<fs::path> note_files;
        try {
            for (const auto& entry : fs::directory_iterator(notes_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                    note_files.push_back(entry.path());
                }
            }
        } catch (...) {}

        std::sort(note_files.begin(), note_files.end());

        if (note_files.empty()) {
            return {true, "You have no saved notes.", ""};
        }

        // If specific index requested
        auto idx_it = params.find("index");
        if (idx_it != params.end() && !idx_it->second.empty()) {
            int idx = -1;
            try { idx = std::stoi(idx_it->second) - 1; } catch (...) {}
            if (idx < 0 || idx >= (int)note_files.size()) {
                return {false, "", "Note index out of range. You have " + std::to_string(note_files.size()) + " notes."};
            }
            std::ifstream in(note_files[idx]);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return {true, "Note " + std::to_string(idx + 1) + ":\n" + content, ""};
        }

        // Read all notes
        std::string result = "You have " + std::to_string(note_files.size()) + " note" +
                             (note_files.size() != 1 ? "s" : "") + ":\n";
        int i = 1;
        for (const auto& f : note_files) {
            std::ifstream in(f);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            // Truncate for voice
            if (content.length() > 200) content = content.substr(0, 197) + "...";
            result += std::to_string(i++) + ". " + content + "\n";
        }
        return {true, result, ""};

    } else if (action == "delete") {
        std::vector<fs::path> note_files;
        try {
            for (const auto& entry : fs::directory_iterator(notes_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                    note_files.push_back(entry.path());
                }
            }
        } catch (...) {}
        std::sort(note_files.begin(), note_files.end());

        auto idx_it = params.find("index");
        if (idx_it == params.end() || idx_it->second.empty()) {
            return {false, "", "Missing 'index' parameter for delete action."};
        }
        int idx = -1;
        try { idx = std::stoi(idx_it->second) - 1; } catch (...) {}
        if (idx < 0 || idx >= (int)note_files.size()) {
            return {false, "", "Invalid note index."};
        }
        fs::remove(note_files[idx]);
        return {true, "Note " + std::to_string(idx + 1) + " deleted.", ""};

    } else if (action == "clear") {
        int count = 0;
        try {
            for (const auto& entry : fs::directory_iterator(notes_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                    fs::remove(entry.path());
                    count++;
                }
            }
        } catch (...) {}
        return {true, "Deleted " + std::to_string(count) + " note" + (count != 1 ? "s" : "") + ".", ""};
    }

    return {false, "", "Unknown action: " + action};
}

} // namespace Jarvis
