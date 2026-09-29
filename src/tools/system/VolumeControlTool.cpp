#include "VolumeControlTool.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Need COM for Core Audio
#pragma comment(lib, "ole32.lib")

namespace Jarvis {

std::vector<ToolParameter> VolumeControlTool::getParameters() const {
    return {
        {"action", "string", "Action: 'get', 'set', 'mute', 'unmute', 'toggle_mute'", true, "get"},
        {"level",  "string", "Volume level 0-100 (only for 'set' action)", false, "50"}
    };
}

static IAudioEndpointVolume* get_endpoint_volume() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* volume = nullptr;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool com_init = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return nullptr;

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) return nullptr;

    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&volume);
    device->Release();
    if (FAILED(hr)) return nullptr;

    return volume;
}

ToolResult VolumeControlTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "get";

    // Normalize action
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    IAudioEndpointVolume* volume = get_endpoint_volume();
    if (!volume) {
        return {false, "", "Failed to access audio endpoint. Ensure audio device is connected."};
    }

    ToolResult result;

    if (action == "get") {
        float scalar = 0.0f;
        BOOL muted = FALSE;
        volume->GetMasterVolumeLevelScalar(&scalar);
        volume->GetMute(&muted);
        int pct = static_cast<int>(scalar * 100.0f);
        std::string state = muted ? " (muted)" : "";
        result = {true, "Volume is at " + std::to_string(pct) + "%" + state, ""};

    } else if (action == "set") {
        auto level_it = params.find("level");
        if (level_it == params.end()) {
            volume->Release();
            return {false, "", "Missing 'level' parameter for set action (0-100)"};
        }
        int level = 50;
        try { level = std::stoi(level_it->second); } catch (...) {}
        level = std::max(0, std::min(100, level));
        float scalar = level / 100.0f;
        volume->SetMasterVolumeLevelScalar(scalar, NULL);
        // Also unmute when setting volume
        volume->SetMute(FALSE, NULL);
        result = {true, "Volume set to " + std::to_string(level) + "%", ""};

    } else if (action == "mute") {
        volume->SetMute(TRUE, NULL);
        result = {true, "Audio muted", ""};

    } else if (action == "unmute") {
        volume->SetMute(FALSE, NULL);
        result = {true, "Audio unmuted", ""};

    } else if (action == "toggle_mute") {
        BOOL muted = FALSE;
        volume->GetMute(&muted);
        volume->SetMute(!muted, NULL);
        result = {true, muted ? "Audio unmuted" : "Audio muted", ""};

    } else {
        result = {false, "", "Unknown action: " + action + ". Use: get, set, mute, unmute, toggle_mute"};
    }

    volume->Release();
    return result;
}

} // namespace Jarvis
