#pragma once

#include <string>
#include <sstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <fstream>

enum class LogLevel {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level) {
        min_level_ = level;
    }

    void setDebugEnabled(bool enabled) {
        debug_enabled_ = enabled;
    }

    void log(LogLevel level, const std::string& component, const std::string& message) {
        if (level < min_level_) return;
        if (level == LogLevel::LOG_DEBUG && !debug_enabled_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << "\033[" << getColorCode(level) << "m";
        ss << "[" << getLevelString(level) << "]";
        ss << "\033[0m ";
        
        ss << "\033[90m"; // Gray for timestamp
        struct tm timeinfo;
        localtime_s(&timeinfo, &time_t);
        ss << std::put_time(&timeinfo, "%H:%M:%S");
        ss << "." << std::setfill('0') << std::setw(3) << ms.count();
        ss << "\033[0m ";
        
        if (!component.empty()) {
            ss << "\033[36m"; // Cyan for component
            ss << "[" << component << "]";
            ss << "\033[0m ";
        }
        
        ss << message;
        ss << std::endl;

        if (level >= LogLevel::LOG_ERROR) {
            std::cerr << "\r\033[K" << ss.str();
        } else {
            std::cout << "\r\033[K" << ss.str();
        }
    }

private:
    Logger() : min_level_(LogLevel::LOG_INFO), debug_enabled_(false) {}
    ~Logger() = default;

    const char* getColorCode(LogLevel level) {
        switch (level) {
            case LogLevel::LOG_DEBUG:   return "90";    // Gray
            case LogLevel::LOG_INFO:    return "36";    // Cyan
            case LogLevel::LOG_WARN:    return "33";    // Yellow
            case LogLevel::LOG_ERROR:   return "31";    // Red
            default:                    return "0";
        }
    }

    const char* getLevelString(LogLevel level) {
        switch (level) {
            case LogLevel::LOG_DEBUG:   return "DEBUG";
            case LogLevel::LOG_INFO:    return "INFO ";
            case LogLevel::LOG_WARN:    return "WARN ";
            case LogLevel::LOG_ERROR:   return "ERROR";
            default:                    return "?????";
        }
    }

    LogLevel min_level_;
    bool debug_enabled_;
    std::mutex mutex_;
};

// Convenience macros
#define LOG_DEBUG(component, msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, component, msg)
#define LOG_INFO(component, msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, component, msg)
#define LOG_WARN(component, msg) \
    Logger::getInstance().log(LogLevel::LOG_WARN, component, msg)
#define LOG_ERROR(component, msg) \
    Logger::getInstance().log(LogLevel::LOG_ERROR, component, msg)
#define LOG_SUCCESS(component, msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, component, msg)

// User-facing logs (simpler, no component needed)
#define SONNY_SAY(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "SONNY", msg)
#define SONNY_THINKING() \
    Logger::getInstance().log(LogLevel::LOG_INFO, "SONNY", "Thinking...")
#define SONNY_LISTENING() \
    Logger::getInstance().log(LogLevel::LOG_INFO, "SONNY", "Listening...")

// Component-specific convenience macros
#define LOG_STT(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "STT", msg)
#define LOG_STT_DEBUG(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "STT", msg)
#define LOG_WAKE(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "WAKE", msg)
#define LOG_TOOL(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "TOOL", msg)
#define LOG_TOOL_DEBUG(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "TOOL", msg)
#define LOG_AI(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "AI", msg)
#define LOG_EAR(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "EAR", msg)
#define LOG_RAG(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "RAG", msg)
#define LOG_RAG_DEBUG(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "RAG", msg)
#define LOG_VIDEOPLAYER(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "VideoPlayer", msg)
#define LOG_WEBSEARCH(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "WebSearch", msg)
#define LOG_LLAMANATIVE(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "LLAMA-NATIVE", msg)
#define LOG_PERF(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "PERF", msg)
#define LOG_AUDIO(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "AUDIO", msg)
#define LOG_DIRECT_CMD(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "DIRECT_CMD", msg)
#define LOG_BROWSER_STT(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "BrowserSTT", msg)
#define LOG_FILEEXPLORER(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "FileExplorer", msg)
#define LOG_FILEINDEX(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "FileIndex", msg)
#define LOG_APPLAUNCHER(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "AppLauncher", msg)
#define LOG_KOKORO(msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, "Kokoro", msg)
#define LOG_DEBUG_COMPONENT(component, msg) \
    Logger::getInstance().log(LogLevel::LOG_DEBUG, component, msg)
#define LOG_STATUS(msg) \
    Logger::getInstance().log(LogLevel::LOG_INFO, "STATUS", msg)
