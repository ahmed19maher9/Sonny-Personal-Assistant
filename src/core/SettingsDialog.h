#pragma once

#include "ConfigManager.h"
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <functional>
#include <vector>

class SettingsDialog {
public:
    using RestartCallback = std::function<void()>;

    static bool show_dialog(HWND parent, AppConfig& config, const std::vector<std::string>& available_voices = {});
    static void set_restart_callback(RestartCallback callback) { settings_restart_callback_ = callback; }
    static void mark_restart_required() { restart_required_ = true; }

    static std::vector<std::string> get_available_voices();
    static std::vector<std::pair<std::string, std::string>> get_supported_languages();
    static std::vector<std::string> get_browser_voices_from_server();
    static std::vector<std::string> get_available_cameras();
    static void browse_llama_model(HWND hwnd);
    static void browse_rag_embedding_model(HWND hwnd);
    static AppConfig* current_config;
    static std::vector<std::string> available_voices_;

private:
    static HWND hwnd_dialog;
    static RestartCallback settings_restart_callback_;
    static bool restart_required_;

    static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};
