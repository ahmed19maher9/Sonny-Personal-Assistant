#pragma once

#include <chrono>
#include <atomic>
#include "Logger.h"

class PerfTimer {
public:
    // Set whether debug mode is enabled globally
    static void set_debug_mode(bool enabled) { debug_mode_ = enabled; }
    static bool is_debug_mode() { return debug_mode_; }

    // Start timing an operation (no automatic logging - use explicit LOG_PERF)
    PerfTimer(const char* operation_name)
        : name_(operation_name)
        , start_(std::chrono::high_resolution_clock::now()) {}

    // Get elapsed time in milliseconds
    long long elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
    }

    // Manually log elapsed time
    void log_elapsed(const char* label = nullptr) {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
        if (label) {
            LOG_PERF(std::string(name_) + " - " + std::string(label) + ": " + std::to_string(duration) + "ms");
        } else {
            LOG_PERF(std::string(name_) + ": " + std::to_string(duration) + "ms");
        }
    }

private:
    static std::atomic<int> operation_counter_;
    static bool debug_mode_;
    const char* name_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};