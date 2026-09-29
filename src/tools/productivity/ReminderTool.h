#pragma once
#include "Tool.h"
#include <thread>
#include <vector>
#include <mutex>
#include <functional>

namespace Jarvis {

class ReminderTool : public Tool {
public:
    using SpeakCallback = std::function<void(const std::string&)>;

    ReminderTool();
    ~ReminderTool();

    std::string getName() const override { return "reminder"; }
    std::string getDescription() const override {
        return "Set a reminder or timer. Action 'set': requires 'seconds' (int) and 'message'. Action 'list': lists active reminders. Action 'cancel': cancels by id.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;

    // Optional: inject a TTS callback so reminder fires audibly
    void set_speak_callback(SpeakCallback cb) { speak_callback_ = cb; }

private:
    struct Reminder {
        int id;
        std::string message;
        std::thread thread;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> completed{false};
    };

    std::vector<std::unique_ptr<Reminder>> reminders_;
    std::mutex mutex_;
    int next_id_ = 1;
    SpeakCallback speak_callback_;
};

} // namespace Jarvis
