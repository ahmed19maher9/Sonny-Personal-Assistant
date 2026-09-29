#include "ReminderTool.h"
#include "Logger.h"
#include <windows.h>
#include <iostream>
#include <sstream>

namespace Jarvis {

ReminderTool::ReminderTool() {}

ReminderTool::~ReminderTool() {
    // Cancel all active reminders
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : reminders_) {
        r->cancelled = true;
    }
    // Detach threads so destructor doesn't block
    for (auto& r : reminders_) {
        if (r->thread.joinable()) {
            r->thread.detach();
        }
    }
}

std::vector<ToolParameter> ReminderTool::getParameters() const {
    return {
        {"action",  "string", "Action: 'set', 'list', 'cancel'", true, "set"},
        {"seconds", "string", "Number of seconds until the reminder fires (for 'set' action)", false, "60"},
        {"message", "string", "The reminder message to announce (for 'set' action)", false, "Reminder!"},
        {"id",      "string", "Reminder ID to cancel (for 'cancel' action)", false, ""}
    };
}

ToolResult ReminderTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "set";

    if (action == "list") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reminders_.empty()) {
            return {true, "No active reminders.", ""};
        }
        std::string out = "Active reminders: ";
        bool found_any = false;
        for (const auto& r : reminders_) {
            if (!r->cancelled && !r->completed) {
                out += "[ID " + std::to_string(r->id) + "] " + r->message + "; ";
                found_any = true;
            }
        }
        if (!found_any) {
            return {true, "No active reminders.", ""};
        }
        return {true, out, ""};
    }

    if (action == "cancel") {
        auto id_it = params.find("id");
        if (id_it == params.end()) {
            return {false, "", "Missing 'id' parameter for cancel action."};
        }
        int id = -1;
        try {
            id = std::stoi(id_it->second);
        } catch (...) {
            return {false, "", "Invalid reminder ID: " + id_it->second};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& r : reminders_) {
            if (r->id == id) {
                r->cancelled = true;
                return {true, "Reminder " + std::to_string(id) + " cancelled.", ""};
            }
        }
        return {false, "", "Reminder ID " + std::to_string(id) + " not found."};
    }

    // Default: "set"
    int seconds = 60;
    auto sec_it = params.find("seconds");
    if (sec_it != params.end()) {
        try { seconds = std::stoi(sec_it->second); } catch (...) {}
    }

    std::string message = "Reminder!";
    auto msg_it = params.find("message");
    if (msg_it != params.end() && !msg_it->second.empty()) {
        message = msg_it->second;
    }

    if (seconds <= 0) {
        return {false, "", "Seconds must be a positive number."};
    }

    auto reminder = std::make_unique<Reminder>();
    int id = next_id_++;
    reminder->id = id;
    reminder->message = message;

    // Capture by value to be safe across thread lifetime
    Reminder* raw_ptr = reminder.get();
    SpeakCallback speak = speak_callback_; // copy the callback

    reminder->thread = std::thread([raw_ptr, seconds, message, id, speak]() {
        // Sleep in 100ms intervals so we can check cancelled flag
        for (int i = 0; i < seconds * 10; ++i) {
            if (raw_ptr->cancelled) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (raw_ptr->cancelled) return;
        raw_ptr->completed = true;

        LOG_DEBUG_COMPONENT("Reminder", "FIRING: " + message);

        // Windows notification sound
        MessageBeep(MB_ICONEXCLAMATION);

        // Show a balloon notification via tray if possible, or a messagebox
        // Using WM_TRAYICON balloons requires HWND; fallback to MessageBeep + speak
        if (speak) {
            speak("Reminder: " + message);
        } else {
            // Fallback: show a system message box
            MessageBoxA(NULL, message.c_str(), "Sonny Reminder", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        reminders_.push_back(std::move(reminder));
    }

    // Human-friendly time string
    std::string time_str;
    if (seconds >= 3600) {
        int h = seconds / 3600, m = (seconds % 3600) / 60;
        time_str = std::to_string(h) + " hour" + (h > 1 ? "s" : "");
        if (m > 0) time_str += " and " + std::to_string(m) + " minute" + (m > 1 ? "s" : "");
    } else if (seconds >= 60) {
        int m = seconds / 60, s = seconds % 60;
        time_str = std::to_string(m) + " minute" + (m > 1 ? "s" : "");
        if (s > 0) time_str += " and " + std::to_string(s) + " second" + (s > 1 ? "s" : "");
    } else {
        time_str = std::to_string(seconds) + " second" + (seconds != 1 ? "s" : "");
    }

    return {true, "Reminder set for " + time_str + ". I'll remind you: " + message, ""};
}

} // namespace Jarvis
