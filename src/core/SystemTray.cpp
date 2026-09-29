#include "SystemTray.h"
#include "SettingsDialog.h"
#include "resource_ids.h"
#include <iostream>

SystemTray* SystemTray::instance_ = nullptr;

SystemTray::SystemTray()
    : hwnd_(nullptr)
    , h_instance_(nullptr)
    , initialized_(false)
{
    instance_ = this;
    ZeroMemory(&nid_, sizeof(nid_));
}

SystemTray::~SystemTray() {
    shutdown();
}

bool SystemTray::initialize(HINSTANCE hInstance, const char* tooltip) {
    h_instance_ = hInstance;
    
    // Create a hidden window for message handling
    WNDCLASS wc = {};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "SonnyTrayWindowClass";
    
    if (!RegisterClass(&wc)) {
        std::cerr << "Failed to register window class" << std::endl;
        return false;
    }
    
    hwnd_ = CreateWindowEx(
        0,
        "SonnyTrayWindowClass",
        "Sonny Tray",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        hInstance,
        this
    );
    
    if (!hwnd_) {
        std::cerr << "Failed to create tray window" << std::endl;
        return false;
    }
    
    // Initialize NOTIFYICONDATA
    nid_.cbSize = sizeof(NOTIFYICONDATA);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_USER + 1;
    nid_.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    if (!nid_.hIcon) {
        nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    strncpy_s(nid_.szTip, tooltip, sizeof(nid_.szTip) - 1);
    
    if (!Shell_NotifyIcon(NIM_ADD, &nid_)) {
        std::cerr << "Failed to add tray icon" << std::endl;
        DestroyWindow(hwnd_);
        return false;
    }
    
    initialized_ = true;
    return true;
}

void SystemTray::shutdown() {
    if (initialized_) {
        Shell_NotifyIcon(NIM_DELETE, &nid_);
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        initialized_ = false;
    }
}

void SystemTray::update_tooltip(const char* tooltip) {
    if (!initialized_) return;
    
    strncpy_s(nid_.szTip, tooltip, sizeof(nid_.szTip) - 1);
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIcon(NIM_MODIFY, &nid_);
}

void SystemTray::process_messages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

LRESULT CALLBACK SystemTray::window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_USER + 1) {
        if (instance_) {
            instance_->on_tray_click(lParam);
        }
        return 0;
    }
    
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void SystemTray::on_tray_click(WPARAM lParam) {
    if (lParam == WM_RBUTTONUP) {
        create_context_menu();
    } else if (lParam == WM_LBUTTONDBLCLK) {
        // Double click could open settings
        if (settings_callback_) {
            AppConfig config;
            ConfigManager::load_config(config, ConfigManager::get_config_path());
            settings_callback_(config);
        }
    }
}

void SystemTray::create_context_menu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();

    const bool console_visible = console_visible_callback_ && console_visible_callback_();

    AppendMenu(hMenu, MF_STRING, 1, "Settings");
    AppendMenu(hMenu, MF_STRING | (console_visible ? MF_CHECKED : 0), 3, "Console");
    AppendMenu(hMenu, MF_STRING, 4, "Sonny Mesh");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, 2, "Exit");

    SetForegroundWindow(hwnd_);

    UINT result = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y,
        0,
        hwnd_,
        nullptr
    );

    DestroyMenu(hMenu);

    if (result == 1) {
        // Settings
        if (settings_callback_) {
            AppConfig config;
            ConfigManager::load_config(config, ConfigManager::get_config_path());
            if (SettingsDialog::show_dialog(hwnd_, config)) {
                ConfigManager::save_config(config, ConfigManager::get_config_path());
                settings_callback_(config);
            }
        }
    } else if (result == 3) {
        // Console
        if (console_callback_) {
            console_callback_();
        }
    } else if (result == 4) {
        // Sonny Mesh
        if (mesh_callback_) {
            mesh_callback_();
        }
    } else if (result == 2) {
        // Exit
        if (exit_callback_) {
            exit_callback_();
        }
    }
}
