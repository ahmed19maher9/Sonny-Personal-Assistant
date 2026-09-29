#include "SettingsDialog.h"
#include "resource_ids.h"
#include "CameraCapture.h"
#include <windows.h>
#include <shlobj.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <regex>
#include <commctrl.h>
#include <objbase.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

AppConfig* SettingsDialog::current_config = nullptr;
HWND SettingsDialog::hwnd_dialog = nullptr;
SettingsDialog::RestartCallback SettingsDialog::settings_restart_callback_ = nullptr;
bool SettingsDialog::restart_required_ = false;
std::vector<std::string> SettingsDialog::available_voices_;

struct TabPage {
    HWND hwnd = nullptr;
    int dlg_id = 0;
    DLGPROC proc = nullptr;
};

static TabPage g_tabs[5];
static int g_current_tab = 0;
static HWND g_hwnd_tab_ctrl = nullptr;

static std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), size);
    result.resize(static_cast<size_t>(size - 1));
    return result;
}

static void populate_kokoro_voices(HWND hCombo, const std::string& current) {
    if (!hCombo) return;
    for (const std::string& voice : SettingsDialog::available_voices_) {
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)voice.c_str());
    }
    int index = static_cast<int>(SendMessage(hCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)current.c_str()));
    if (index != CB_ERR) SendMessage(hCombo, CB_SETCURSEL, index, 0);
    else SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

static void populate_languages(HWND hCombo, const std::string& current) {
    if (!hCombo) return;
    auto languages = SettingsDialog::get_supported_languages();
    for (const auto& lang : languages) {
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)lang.second.c_str());
    }
    int lang_index = 0;
    for (size_t i = 0; i < languages.size(); ++i) {
        if (languages[i].first == current) { lang_index = (int)i; break; }
    }
    SendMessage(hCombo, CB_SETCURSEL, lang_index, 0);
}

static void populate_browser_voices(HWND hCombo, const std::string& current) {
    if (!hCombo) return;
    std::vector<std::string> browser_voices = SettingsDialog::get_browser_voices_from_server();
    if (browser_voices.empty()) {
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Default (Chrome)");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Custom voice name...");
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"(Server not running - start assistant with browser STT enabled)");
    } else {
        for (const std::string& voice : browser_voices) {
            SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)voice.c_str());
        }
    }
    if (!current.empty()) {
        int index = static_cast<int>(SendMessage(hCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)current.c_str()));
        if (index != CB_ERR) SendMessage(hCombo, CB_SETCURSEL, index, 0);
        else SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    } else {
        SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    }
}

static void populate_cameras(HWND hCombo, const std::string& current) {
    if (!hCombo) return;
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Default Camera");
    std::vector<std::string> cameras = SettingsDialog::get_available_cameras();
    std::cout << "populate_cameras: Found " << cameras.size() << " cameras" << std::endl;
    for (const std::string& camera : cameras) {
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)camera.c_str());
        std::cout << "  Added camera: " << camera << std::endl;
    }
    if (!current.empty()) {
        int index = static_cast<int>(SendMessage(hCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)current.c_str()));
        if (index != CB_ERR) SendMessage(hCombo, CB_SETCURSEL, index, 0);
        else SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    } else {
        SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    }
}

static void populate_vectors_mode(HWND hCombo, const std::string& current) {
    if (!hCombo) return;
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"Off");
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"LAN");
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"WAN");
    int mode_index = 0;
    if (current == "lan") mode_index = 1;
    else if (current == "wan") mode_index = 2;
    SendMessage(hCombo, CB_SETCURSEL, mode_index, 0);
}

static void tab_ai_voice_init(HWND hwnd) {
    if (!SettingsDialog::current_config) return;
    SetDlgItemText(hwnd, IDC_TAB_AI_VOICE_LLAMA_MODEL, SettingsDialog::current_config->llama_model_path.c_str());
    populate_kokoro_voices(GetDlgItem(hwnd, IDC_TAB_AI_VOICE_KOKORO_VOICE), SettingsDialog::current_config->kokoro_voice);
    populate_languages(GetDlgItem(hwnd, IDC_TAB_AI_VOICE_LANGUAGE), SettingsDialog::current_config->language);
    SetDlgItemText(hwnd, IDC_TAB_AI_VOICE_TTS_SPEED, std::to_string(SettingsDialog::current_config->tts_speed).c_str());
    SetDlgItemText(hwnd, IDC_TAB_AI_VOICE_MIC_GAIN, std::to_string(SettingsDialog::current_config->mic_gain).c_str());
}

static void tab_ai_voice_ok(HWND hwnd, const AppConfig& original_config) {
    if (!SettingsDialog::current_config) return;
    char buffer[MAX_PATH];
    GetDlgItemText(hwnd, IDC_TAB_AI_VOICE_LLAMA_MODEL, buffer, MAX_PATH);
    std::string new_model_path = buffer;
    if (new_model_path != original_config.llama_model_path) {
        SettingsDialog::mark_restart_required();
    }
    SettingsDialog::current_config->llama_model_path = new_model_path;

    HWND hCombo = GetDlgItem(hwnd, IDC_TAB_AI_VOICE_KOKORO_VOICE);
    if (hCombo) {
        int index = static_cast<int>(SendMessage(hCombo, CB_GETCURSEL, 0, 0));
        if (index != CB_ERR) {
            SendMessage(hCombo, CB_GETLBTEXT, index, (LPARAM)buffer);
            SettingsDialog::current_config->kokoro_voice = buffer;
        }
    }

    HWND hLangCombo = GetDlgItem(hwnd, IDC_TAB_AI_VOICE_LANGUAGE);
    if (hLangCombo) {
        int lang_index = static_cast<int>(SendMessage(hLangCombo, CB_GETCURSEL, 0, 0));
        if (lang_index != CB_ERR) {
            auto languages = SettingsDialog::get_supported_languages();
            if (lang_index >= 0 && lang_index < (int)languages.size()) {
                std::string new_language = languages[lang_index].first;
                if (new_language != original_config.language) {
                    SettingsDialog::mark_restart_required();
                }
                SettingsDialog::current_config->language = new_language;
            }
        }
    }

    char speed_buf[32] = {0};
    GetDlgItemText(hwnd, IDC_TAB_AI_VOICE_TTS_SPEED, speed_buf, sizeof(speed_buf));
    float new_speed = std::stof(speed_buf);
    if (new_speed != original_config.tts_speed) {
        SettingsDialog::mark_restart_required();
    }
    SettingsDialog::current_config->tts_speed = new_speed;

    char gain_buf[32] = {0};
    GetDlgItemText(hwnd, IDC_TAB_AI_VOICE_MIC_GAIN, gain_buf, sizeof(gain_buf));
    SettingsDialog::current_config->mic_gain = std::stof(gain_buf);
}

static void tab_browser_stt_tts_init(HWND hwnd) {
    if (!SettingsDialog::current_config) return;
    CheckDlgButton(hwnd, IDC_TAB_BROWSER_STT, SettingsDialog::current_config->use_browser_stt ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_TAB_BROWSER_TTS, SettingsDialog::current_config->use_browser_tts ? BST_CHECKED : BST_UNCHECKED);
    populate_browser_voices(GetDlgItem(hwnd, IDC_TAB_BROWSER_VOICE), SettingsDialog::current_config->browser_voice);
}

static void tab_browser_stt_tts_ok(HWND hwnd, const AppConfig& original_config) {
    (void)original_config; // These settings don't require restart
    if (!SettingsDialog::current_config) return;
    SettingsDialog::current_config->use_browser_stt = (IsDlgButtonChecked(hwnd, IDC_TAB_BROWSER_STT) == BST_CHECKED);
    SettingsDialog::current_config->use_browser_tts = (IsDlgButtonChecked(hwnd, IDC_TAB_BROWSER_TTS) == BST_CHECKED);

    HWND hCombo = GetDlgItem(hwnd, IDC_TAB_BROWSER_VOICE);
    if (hCombo) {
        int index = static_cast<int>(SendMessage(hCombo, CB_GETCURSEL, 0, 0));
        if (index != CB_ERR) {
            char buffer[MAX_PATH];
            SendMessage(hCombo, CB_GETLBTEXT, index, (LPARAM)buffer);
            SettingsDialog::current_config->browser_voice = buffer;
        }
    }
}

static void tab_avatar_prompt_init(HWND hwnd) {
    if (!SettingsDialog::current_config) return;
    CheckDlgButton(hwnd, IDC_TAB_AVATAR_SHOW, SettingsDialog::current_config->show_avatar ? BST_CHECKED : BST_UNCHECKED);
    if (SettingsDialog::current_config->avatar_type == "iron-man") {
        CheckRadioButton(hwnd, IDC_TAB_AVATAR_SONNY, IDC_TAB_AVATAR_IRONMAN, IDC_TAB_AVATAR_IRONMAN);
    } else {
        CheckRadioButton(hwnd, IDC_TAB_AVATAR_SONNY, IDC_TAB_AVATAR_IRONMAN, IDC_TAB_AVATAR_SONNY);
    }
    CheckDlgButton(hwnd, IDC_TAB_VISION_CAMERA_ENABLED, SettingsDialog::current_config->camera_feed_enabled ? BST_CHECKED : BST_UNCHECKED);
    populate_cameras(GetDlgItem(hwnd, IDC_TAB_VISION_CAMERA_DEVICE), SettingsDialog::current_config->camera_device_name);
    SetDlgItemText(hwnd, IDC_TAB_AVATAR_SYSTEM_PROMPT, SettingsDialog::current_config->system_prompt.c_str());
}

static void tab_avatar_prompt_ok(HWND hwnd, const AppConfig& original_config) {
    (void)original_config; // These settings don't require restart
    if (!SettingsDialog::current_config) return;
    SettingsDialog::current_config->show_avatar = (IsDlgButtonChecked(hwnd, IDC_TAB_AVATAR_SHOW) == BST_CHECKED);
    SettingsDialog::current_config->avatar_type = (IsDlgButtonChecked(hwnd, IDC_TAB_AVATAR_IRONMAN) == BST_CHECKED) ? "iron-man" : "sonny";
    SettingsDialog::current_config->camera_feed_enabled = (IsDlgButtonChecked(hwnd, IDC_TAB_VISION_CAMERA_ENABLED) == BST_CHECKED);
    HWND hCameraCombo = GetDlgItem(hwnd, IDC_TAB_VISION_CAMERA_DEVICE);
    if (hCameraCombo) {
        int index = static_cast<int>(SendMessage(hCameraCombo, CB_GETCURSEL, 0, 0));
        if (index != CB_ERR && index > 0) { // index 0 is "Default Camera"
            char camera_buf[MAX_PATH];
            SendMessage(hCameraCombo, CB_GETLBTEXT, index, (LPARAM)camera_buf);
            SettingsDialog::current_config->camera_device_name = camera_buf;
        } else {
            SettingsDialog::current_config->camera_device_name = "";
        }
    }
    char buffer[MAX_PATH];
    GetDlgItemText(hwnd, IDC_TAB_AVATAR_SYSTEM_PROMPT, buffer, MAX_PATH);
    SettingsDialog::current_config->system_prompt = buffer;
}

static void tab_rag_engine_init(HWND hwnd) {
    if (!SettingsDialog::current_config) return;
    SetDlgItemText(hwnd, IDC_TAB_RAG_EMBEDDING_MODEL, SettingsDialog::current_config->rag_embedding_model.c_str());
    CheckDlgButton(hwnd, IDC_TAB_RAG_WAKE_WORD, SettingsDialog::current_config->wake_word_enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_TAB_RAG_DEBUG_MODE, SettingsDialog::current_config->debug_mode ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(hwnd, IDC_TAB_RAG_CONTEXT_BUDGET, std::to_string(SettingsDialog::current_config->rag_max_context_chars).c_str());
}

static void tab_rag_engine_ok(HWND hwnd, const AppConfig& original_config) {
    if (!SettingsDialog::current_config) return;
    char buffer[MAX_PATH];
    GetDlgItemText(hwnd, IDC_TAB_RAG_EMBEDDING_MODEL, buffer, MAX_PATH);
    std::string new_model = buffer;
    if (new_model != original_config.rag_embedding_model) {
        SettingsDialog::mark_restart_required();
    }
    SettingsDialog::current_config->rag_embedding_model = new_model;

    SettingsDialog::current_config->wake_word_enabled = (IsDlgButtonChecked(hwnd, IDC_TAB_RAG_WAKE_WORD) == BST_CHECKED);

    bool new_debug = (IsDlgButtonChecked(hwnd, IDC_TAB_RAG_DEBUG_MODE) == BST_CHECKED);
    if (new_debug != original_config.debug_mode) {
        SettingsDialog::mark_restart_required();
    }
    SettingsDialog::current_config->debug_mode = new_debug;

    char budget_buf[32] = {0};
    GetDlgItemText(hwnd, IDC_TAB_RAG_CONTEXT_BUDGET, budget_buf, sizeof(budget_buf));
    const int budget = atoi(budget_buf);
    if (budget >= 200) SettingsDialog::current_config->rag_max_context_chars = budget;
}

static void tab_vectors_init(HWND hwnd) {
    if (!SettingsDialog::current_config) return;
    CheckDlgButton(hwnd, IDC_TAB_VECTORS_ENABLED, SettingsDialog::current_config->vectors_enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(hwnd, IDC_TAB_VECTORS_ROOM, SettingsDialog::current_config->vectors_room.c_str());
    SetDlgItemText(hwnd, IDC_TAB_VECTORS_RELAY, SettingsDialog::current_config->vectors_rendezvous_url.c_str());
    populate_vectors_mode(GetDlgItem(hwnd, IDC_TAB_VECTORS_MODE), SettingsDialog::current_config->vectors_mode);
}

static void tab_vectors_ok(HWND hwnd, const AppConfig& original_config) {
    (void)original_config; // These settings don't require restart
    if (!SettingsDialog::current_config) return;
    char buffer[MAX_PATH];
    SettingsDialog::current_config->vectors_enabled = (IsDlgButtonChecked(hwnd, IDC_TAB_VECTORS_ENABLED) == BST_CHECKED);
    GetDlgItemText(hwnd, IDC_TAB_VECTORS_ROOM, buffer, MAX_PATH);
    SettingsDialog::current_config->vectors_room = buffer;
    GetDlgItemText(hwnd, IDC_TAB_VECTORS_RELAY, buffer, MAX_PATH);
    SettingsDialog::current_config->vectors_rendezvous_url = buffer;

    HWND hModeCombo = GetDlgItem(hwnd, IDC_TAB_VECTORS_MODE);
    if (hModeCombo) {
        const int mode_index = static_cast<int>(SendMessage(hModeCombo, CB_GETCURSEL, 0, 0));
        if (mode_index == 1) SettingsDialog::current_config->vectors_mode = "lan";
        else if (mode_index == 2) SettingsDialog::current_config->vectors_mode = "wan";
        else SettingsDialog::current_config->vectors_mode = "off";
    }
}

static void (*g_tab_init[5])(HWND) = {
    tab_ai_voice_init,
    tab_browser_stt_tts_init,
    tab_avatar_prompt_init,
    tab_rag_engine_init,
    tab_vectors_init
};

static void (*g_tab_ok[5])(HWND, const AppConfig&) = {
    tab_ai_voice_ok,
    tab_browser_stt_tts_ok,
    tab_avatar_prompt_ok,
    tab_rag_engine_ok,
    tab_vectors_ok
};

static INT_PTR CALLBACK tab_proc_ai_voice(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: tab_ai_voice_init(hwnd); return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_TAB_AI_VOICE_BROWSE_LLAMA) {
                SettingsDialog::browse_llama_model(hwnd);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static INT_PTR CALLBACK tab_proc_browser_stt_tts(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: tab_browser_stt_tts_init(hwnd); return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK tab_proc_avatar_prompt(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: tab_avatar_prompt_init(hwnd); return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK tab_proc_rag_engine(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: tab_rag_engine_init(hwnd); return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_TAB_RAG_BROWSE_MODEL) {
                SettingsDialog::browse_rag_embedding_model(hwnd);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static INT_PTR CALLBACK tab_proc_vectors(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: tab_vectors_init(hwnd); return TRUE;
    }
    return FALSE;
}

static void show_tab(int index) {
    if (index < 0 || index >= 5) return;
    for (int i = 0; i < 5; ++i) {
        if (g_tabs[i].hwnd) {
            ShowWindow(g_tabs[i].hwnd, i == index ? SW_SHOW : SW_HIDE);
        }
    }
    g_current_tab = index;
}

static void on_tab_change(HWND hwnd) {
    int new_tab = TabCtrl_GetCurSel(g_hwnd_tab_ctrl);
    if (new_tab != g_current_tab) {
        show_tab(new_tab);
    }
}

void SettingsDialog::browse_rag_embedding_model(HWND hwnd) {
    char current_dir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, current_dir);

    BROWSEINFO bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select sentence-transformers embedding model folder (e.g., all-MiniLM-L6-v2 from HuggingFace)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    if (pidl != 0) {
        char path[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, path)) {
            SetDlgItemText(hwnd, IDC_TAB_RAG_EMBEDDING_MODEL, path);
        }
        IMalloc* imalloc = 0;
        if (SUCCEEDED(SHGetMalloc(&imalloc))) {
            imalloc->Free(pidl);
            imalloc->Release();
        }
    }
    SetCurrentDirectoryA(current_dir);
}

INT_PTR CALLBACK SettingsDialog::dialog_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: {
            hwnd_dialog = hwnd;
            if (!current_config) return TRUE;

            INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES };
            InitCommonControlsEx(&icc);

            g_hwnd_tab_ctrl = GetDlgItem(hwnd, IDC_SETTINGS_TAB);
            if (!g_hwnd_tab_ctrl) return TRUE;

            TCITEM tie = {0};
            tie.mask = TCIF_TEXT;
            const char* tab_names[5] = { "AI & Voice", "Browser STT/TTS", "Avatar & Prompt", "RAG Engine", "Vectors" };
            int tab_dlg_ids[5] = { IDD_TAB_AI_VOICE, IDD_TAB_BROWSER_STT_TTS, IDD_TAB_AVATAR_PROMPT, IDD_TAB_RAG_ENGINE, IDD_TAB_VECTORS };
            DLGPROC tab_procs[5] = { tab_proc_ai_voice, tab_proc_browser_stt_tts, tab_proc_avatar_prompt, tab_proc_rag_engine, tab_proc_vectors };

            for (int i = 0; i < 5; ++i) {
                tie.pszText = const_cast<char*>(tab_names[i]);
                TabCtrl_InsertItem(g_hwnd_tab_ctrl, i, &tie);
                g_tabs[i].dlg_id = tab_dlg_ids[i];
                g_tabs[i].proc = tab_procs[i];
                g_tabs[i].hwnd = CreateDialogParam(GetModuleHandle(nullptr), MAKEINTRESOURCE(tab_dlg_ids[i]), hwnd, tab_procs[i], (LPARAM)current_config);
                if (g_tabs[i].hwnd) {
                    RECT rc;
                    GetClientRect(g_hwnd_tab_ctrl, &rc);
                    TabCtrl_AdjustRect(g_hwnd_tab_ctrl, FALSE, &rc);
                    SetWindowPos(g_tabs[i].hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
                    ShowWindow(g_tabs[i].hwnd, i == 0 ? SW_SHOW : SW_HIDE);
                }
            }

            show_tab(0);

            RECT rect;
            GetWindowRect(hwnd, &rect);
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            int screen_width = GetSystemMetrics(SM_CXSCREEN);
            int screen_height = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hwnd, nullptr, (screen_width - width) / 2, (screen_height - height) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            return TRUE;
        }

        case WM_NOTIFY: {
            LPNMHDR nmhdr = (LPNMHDR)lParam;
            if (nmhdr->hwndFrom == g_hwnd_tab_ctrl && nmhdr->code == TCN_SELCHANGE) {
                on_tab_change(hwnd);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Save original config to detect restart-required changes
                    AppConfig original_config = *current_config;
                    restart_required_ = false;

                    for (int i = 0; i < 5; ++i) {
                        if (g_tab_ok[i]) g_tab_ok[i](g_tabs[i].hwnd, original_config);
                    }
                    EndDialog(hwnd, IDOK);
                    if (restart_required_ && settings_restart_callback_) {
                        if (MessageBox(hwnd, "Some settings require restart to take effect. Restart the application now?", "Settings Changed", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            settings_restart_callback_();
                        }
                    }
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
        }

        case WM_DESTROY: {
            for (int i = 0; i < 5; ++i) {
                if (g_tabs[i].hwnd) DestroyWindow(g_tabs[i].hwnd);
                g_tabs[i].hwnd = nullptr;
            }
            break;
        }
    }
    return FALSE;
}

bool SettingsDialog::show_dialog(HWND parent, AppConfig& config, const std::vector<std::string>& available_voices) {
    current_config = &config;
    restart_required_ = false; // Reset flag before showing dialog
    if (available_voices.empty()) available_voices_ = get_available_voices();
    else available_voices_ = available_voices;

    INT_PTR result = DialogBoxParam(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDD_SETTINGS_TABBED), parent, dialog_proc, (LPARAM)current_config);

    current_config = nullptr;
    hwnd_dialog = nullptr;
    g_hwnd_tab_ctrl = nullptr;

    return (result == IDOK);
}

void SettingsDialog::browse_llama_model(HWND hwnd) {
    char current_dir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, current_dir);

    OPENFILENAME ofn = {};
    char filename[MAX_PATH] = "";
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "GGUF/Blob Files (*.gguf;*.blob)\0*.gguf;*.blob\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Select Llama Model";

    if (GetOpenFileName(&ofn)) {
        SetDlgItemText(hwnd, IDC_TAB_AI_VOICE_LLAMA_MODEL, filename);
    }
    SetCurrentDirectoryA(current_dir);
}

std::vector<std::string> SettingsDialog::get_available_voices() {
    return {"af_heart", "af_bella", "af_nicole", "af_sarah", "af_sky", "am_adam", "am_michael", "bf_emma", "bf_isabella", "bm_george", "bm_lewis"};
}

std::vector<std::string> SettingsDialog::get_available_cameras() {
    return Jarvis::CameraCapture::enumerate_cameras();
}

std::vector<std::string> SettingsDialog::get_browser_voices_from_server() {
    std::vector<std::string> voices;
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return voices;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return voices; }

    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1) {
        closesocket(sock); WSACleanup(); return voices;
    }

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(sock); WSACleanup(); return voices;
    }

    std::string request = "GET /api/voices HTTP/1.1\r\nHost: localhost:8080\r\nConnection: close\r\n\r\n";
    if (send(sock, request.c_str(), static_cast<int>(request.length()), 0) == SOCKET_ERROR) {
        closesocket(sock); WSACleanup(); return voices;
    }

    char buffer[4096];
    std::string response;
    int bytes_received;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;
    }

    closesocket(sock); WSACleanup();

    std::regex voice_regex("\"name\"\\s*:\\s*\"([^\"]+)\"");
    std::sregex_iterator it(response.begin(), response.end(), voice_regex);
    std::sregex_iterator end;
    while (it != end) {
        voices.push_back((*it)[1]);
        ++it;
    }
    return voices;
}

std::vector<std::pair<std::string, std::string>> SettingsDialog::get_supported_languages() {
    return {
        {"en", "English"}, {"ko", "Korean"}, {"ja", "Japanese"}, {"ar", "Arabic"},
        {"bg", "Bulgarian"}, {"cs", "Czech"}, {"da", "Danish"}, {"de", "German"},
        {"el", "Greek"}, {"es", "Spanish"}, {"et", "Estonian"}, {"fi", "Finnish"},
        {"fr", "French"}, {"hi", "Hindi"}, {"hr", "Croatian"}, {"hu", "Hungarian"},
        {"id", "Indonesian"}, {"it", "Italian"}, {"lt", "Lithuanian"}, {"lv", "Latvian"},
        {"nl", "Dutch"}, {"pl", "Polish"}, {"pt", "Portuguese"}, {"ro", "Romanian"},
        {"ru", "Russian"}, {"sk", "Slovak"}, {"sl", "Slovenian"}, {"sv", "Swedish"},
        {"tr", "Turkish"}, {"uk", "Ukrainian"}, {"vi", "Vietnamese"}
    };
}