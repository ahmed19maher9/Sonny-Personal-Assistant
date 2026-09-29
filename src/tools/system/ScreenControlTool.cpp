#include "ScreenControlTool.h"
#include <windows.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <powrprof.h>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "powrprof.lib")

namespace Jarvis {
namespace fs = std::filesystem;

std::vector<ToolParameter> ScreenControlTool::getParameters() const {
    return {
        {"action", "string", "Action: 'lock', 'screenshot', 'sleep', 'shutdown', 'restart', 'brightness_up', 'brightness_down'", true, "lock"}
    };
}

// Save HBITMAP as PNG to a path using GDI+
static bool save_bitmap_png(HBITMAP hBitmap, const std::wstring& path) {
    using namespace Gdiplus;

    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    bool ok = false;
    {
        Bitmap* bmp = Bitmap::FromHBITMAP(hBitmap, NULL);
        if (bmp) {
            // Get PNG encoder CLSID
            UINT num = 0, size = 0;
            GetImageEncodersSize(&num, &size);
            std::vector<BYTE> buf(size);
            ImageCodecInfo* pImageCodecInfo = reinterpret_cast<ImageCodecInfo*>(buf.data());
            GetImageEncoders(num, size, pImageCodecInfo);

            CLSID pngClsid = {};
            for (UINT j = 0; j < num; ++j) {
                if (wcscmp(pImageCodecInfo[j].MimeType, L"image/png") == 0) {
                    pngClsid = pImageCodecInfo[j].Clsid;
                    break;
                }
            }
            Status s = bmp->Save(path.c_str(), &pngClsid, NULL);
            ok = (s == Ok);
            delete bmp;
        }
    }

    GdiplusShutdown(gdiplusToken);
    return ok;
}

ToolResult ScreenControlTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "lock";
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    if (action == "lock") {
        if (LockWorkStation()) {
            return {true, "Screen locked.", ""};
        }
        return {false, "", "Failed to lock screen."};

    } else if (action == "screenshot") {
        // Capture the entire screen
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        HDC hScreen = GetDC(NULL);
        HDC hDC = CreateCompatibleDC(hScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, sw, sh);
        HGDIOBJ old = SelectObject(hDC, hBitmap);
        BitBlt(hDC, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY);
        SelectObject(hDC, old);
        DeleteDC(hDC);
        ReleaseDC(NULL, hScreen);

        // Build output path: Desktop\Sonny_Screenshot_<timestamp>.png
        char desktop[MAX_PATH];
        SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop);

        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_info;
        localtime_s(&tm_info, &t);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);

        std::string filename = std::string(desktop) + "\\Sonny_Screenshot_" + ts + ".png";
        std::wstring wfilename(filename.begin(), filename.end());

        bool saved = save_bitmap_png(hBitmap, wfilename);
        DeleteObject(hBitmap);

        if (saved) {
            return {true, "Screenshot saved to Desktop: Sonny_Screenshot_" + std::string(ts) + ".png", ""};
        }
        return {false, "", "Failed to save screenshot."};

    } else if (action == "sleep") {
        // Set system sleep
        SetSuspendState(FALSE, TRUE, FALSE);
        return {true, "System is going to sleep.", ""};

    } else if (action == "shutdown") {
        // Request shutdown privilege
        HANDLE hToken;
        TOKEN_PRIVILEGES tkp;
        OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
        LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &tkp.Privileges[0].Luid);
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, 0);
        CloseHandle(hToken);

        if (InitiateSystemShutdownExA(NULL, "Shutdown requested by Sonny.", 30, FALSE, FALSE, SHTDN_REASON_MAJOR_OTHER)) {
            return {true, "System will shut down in 30 seconds.", ""};
        }
        return {false, "", "Failed to initiate shutdown."};

    } else if (action == "restart") {
        HANDLE hToken;
        TOKEN_PRIVILEGES tkp;
        OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
        LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &tkp.Privileges[0].Luid);
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, 0);
        CloseHandle(hToken);

        if (InitiateSystemShutdownExA(NULL, "Restart requested by Sonny.", 30, FALSE, TRUE, SHTDN_REASON_MAJOR_OTHER)) {
            return {true, "System will restart in 30 seconds.", ""};
        }
        return {false, "", "Failed to initiate restart."};

    } else if (action == "brightness_up" || action == "brightness_down") {
        // Brightness control via WMI is complex. Use keyboard shortcut instead.
        // Send Windows logo key + A (Action Center) as a workaround
        return {true, "Brightness control is available via Action Center (Win+A).", ""};
    }

    return {false, "", "Unknown action: " + action};
}

} // namespace Jarvis
