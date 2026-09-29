#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct AppConfig;

class FirstRunWizard {
public:
    static bool show_dialog(HWND parent, AppConfig& config);

private:
    static void browse_model_path(HWND hwnd);
    static std::vector<std::pair<std::string, std::string>> get_supported_languages();

    static AppConfig* current_config_;
    static HWND hwnd_dialog_;

    static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};
