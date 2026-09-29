#include "FirstRunWizard.h"
#include "ConfigManager.h"
#include "resource_ids.h"
#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
#include <iostream>
#include <fstream>

AppConfig* FirstRunWizard::current_config_ = nullptr;
HWND FirstRunWizard::hwnd_dialog_ = nullptr;

static std::vector<std::string> get_available_voices() {
    return {"af_heart", "af_bella", "af_nicole", "af_sarah", "af_sky", "am_adam", "am_michael", "bf_emma", "bf_isabella", "bm_george", "bm_lewis"};
}

std::vector<std::pair<std::string, std::string>> FirstRunWizard::get_supported_languages() {
    return {
        {"en", "English"},
        {"ko", "Korean"},
        {"ja", "Japanese"},
        {"ar", "Arabic"},
        {"bg", "Bulgarian"},
        {"cs", "Czech"},
        {"da", "Danish"},
        {"de", "German"},
        {"el", "Greek"},
        {"es", "Spanish"},
        {"et", "Estonian"},
        {"fi", "Finnish"},
        {"fr", "French"},
        {"hi", "Hindi"},
        {"hr", "Croatian"},
        {"hu", "Hungarian"},
        {"id", "Indonesian"},
        {"it", "Italian"},
        {"lt", "Lithuanian"},
        {"lv", "Latvian"},
        {"nl", "Dutch"},
        {"pl", "Polish"},
        {"pt", "Portuguese"},
        {"ro", "Romanian"},
        {"ru", "Russian"},
        {"sk", "Slovak"},
        {"sl", "Slovenian"},
        {"sv", "Swedish"},
        {"tr", "Turkish"},
        {"uk", "Ukrainian"},
        {"vi", "Vietnamese"}
    };
}

void FirstRunWizard::browse_model_path(HWND hwnd) {
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
        SetDlgItemText(hwnd, IDC_WIZARD_MODEL, filename);
    }

    SetCurrentDirectoryA(current_dir);
}

INT_PTR CALLBACK FirstRunWizard::dialog_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG:
            FirstRunWizard::hwnd_dialog_ = hwnd;
            {
                HWND hModel = GetDlgItem(hwnd, IDC_WIZARD_MODEL);
                if (hModel) SetDlgItemText(hwnd, IDC_WIZARD_MODEL, current_config_->llama_model_path.c_str());

                HWND hVoice = GetDlgItem(hwnd, IDC_WIZARD_VOICE);
                if (hVoice) {
                    auto voices = get_available_voices();
                    for (const auto& voice : voices) {
                        SendMessage(hVoice, CB_ADDSTRING, 0, (LPARAM)voice.c_str());
                    }
                    int idx = (int)SendMessage(hVoice, CB_FINDSTRINGEXACT, -1, (LPARAM)current_config_->kokoro_voice.c_str());
                    if (idx != CB_ERR) SendMessage(hVoice, CB_SETCURSEL, idx, 0);
                    else SendMessage(hVoice, CB_SETCURSEL, 5, 0);
                }

                if (current_config_->avatar_type == "iron-man") {
                    CheckRadioButton(hwnd, IDC_WIZARD_AVATAR_SONNY, IDC_WIZARD_AVATAR_IRONMAN, IDC_WIZARD_AVATAR_IRONMAN);
                } else {
                    CheckRadioButton(hwnd, IDC_WIZARD_AVATAR_SONNY, IDC_WIZARD_AVATAR_IRONMAN, IDC_WIZARD_AVATAR_SONNY);
                }

                HWND hLang = GetDlgItem(hwnd, IDC_WIZARD_LANGUAGE);
                if (hLang) {
                    auto langs = get_supported_languages();
                    for (const auto& lang : langs) {
                        SendMessage(hLang, CB_ADDSTRING, 0, (LPARAM)lang.second.c_str());
                    }
                    int lang_idx = 0;
                    for (size_t i = 0; i < langs.size(); ++i) {
                        if (langs[i].first == current_config_->language) {
                            lang_idx = (int)i;
                            break;
                        }
                    }
                    SendMessage(hLang, CB_SETCURSEL, lang_idx, 0);
                }
            }
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    {
                        char buffer[MAX_PATH] = {0};
                        GetDlgItemText(hwnd, IDC_WIZARD_MODEL, buffer, MAX_PATH);
                        current_config_->llama_model_path = buffer;

                        HWND hVoice = GetDlgItem(hwnd, IDC_WIZARD_VOICE);
                        if (hVoice) {
                            int idx = (int)SendMessage(hVoice, CB_GETCURSEL, 0, 0);
                            if (idx != CB_ERR) {
                                SendMessage(hVoice, CB_GETLBTEXT, idx, (LPARAM)buffer);
                                current_config_->kokoro_voice = buffer;
                            }
                        }

                        if (IsDlgButtonChecked(hwnd, IDC_WIZARD_AVATAR_IRONMAN) == BST_CHECKED) {
                            current_config_->avatar_type = "iron-man";
                        } else {
                            current_config_->avatar_type = "sonny";
                        }

                        HWND hLang = GetDlgItem(hwnd, IDC_WIZARD_LANGUAGE);
                        if (hLang) {
                            int lang_idx = (int)SendMessage(hLang, CB_GETCURSEL, 0, 0);
                            if (lang_idx != CB_ERR) {
                                auto langs = get_supported_languages();
                                if (lang_idx >= 0 && lang_idx < (int)langs.size()) {
                                    current_config_->language = langs[lang_idx].first;
                                }
                            }
                        }
                    }
                    EndDialog(hwnd, IDOK);
                    return TRUE;

                case IDC_WIZARD_SKIP:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;

                case IDC_WIZARD_BROWSE_MODEL:
                    browse_model_path(hwnd);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

bool FirstRunWizard::show_dialog(HWND parent, AppConfig& config) {
    current_config_ = &config;

    INT_PTR result = DialogBoxParam(
        GetModuleHandle(nullptr),
        MAKEINTRESOURCE(IDD_FIRST_RUN_WIZARD),
        parent,
        dialog_proc,
        0
    );

    current_config_ = nullptr;
    hwnd_dialog_ = nullptr;

    return (result == IDOK);
}
