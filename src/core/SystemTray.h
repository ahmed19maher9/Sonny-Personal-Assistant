#pragma once

#include <windows.h>
#include <shellapi.h>
#include "ConfigManager.h"
#include <functional>

class SystemTray {
public:
    using SettingsCallback = std::function<void(AppConfig&)>;
    using ExitCallback = std::function<void()>;
    using ConsoleCallback = std::function<void()>;
    using ConsoleVisibleCallback = std::function<bool()>;
    using MeshCallback = std::function<void()>;
    
    SystemTray();
    ~SystemTray();
    
    bool initialize(HINSTANCE hInstance, const char* tooltip = "Sonny Assistant");
    void shutdown();
    
    void set_settings_callback(SettingsCallback callback) { settings_callback_ = callback; }
    void set_exit_callback(ExitCallback callback) { exit_callback_ = callback; }
    void set_console_callback(ConsoleCallback callback) { console_callback_ = callback; }
    // Drives the checkmark next to the Console entry, read fresh every time
    // the menu opens.
    void set_console_visible_callback(ConsoleVisibleCallback callback) { console_visible_callback_ = callback; }
    void set_mesh_callback(MeshCallback callback) { mesh_callback_ = callback; }
    
    void update_tooltip(const char* tooltip);
    
    // Message pump for tray events
    void process_messages();
    
private:
    NOTIFYICONDATA nid_;
    HWND hwnd_;
    HINSTANCE h_instance_;
    bool initialized_;
    
    SettingsCallback settings_callback_;
    ExitCallback exit_callback_;
    ConsoleCallback console_callback_;
    ConsoleVisibleCallback console_visible_callback_;
    MeshCallback mesh_callback_;
    
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static SystemTray* instance_;
    
    void create_context_menu();
    void on_tray_click(WPARAM wParam);
};