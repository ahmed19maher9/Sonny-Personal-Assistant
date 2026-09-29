#include "WifiTool.h"
#include <windows.h>
#include <wlanapi.h>
#include <iostream>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "wlanapi.lib")

namespace Jarvis {

std::vector<ToolParameter> WifiTool::getParameters() const {
    return {
        {"action", "string", "Action: 'current' (show connected WiFi) or 'list' (list available networks)", true, "current"}
    };
}

static std::string signal_quality_to_bars(ULONG quality) {
    if (quality >= 80) return "excellent";
    if (quality >= 60) return "good";
    if (quality >= 40) return "fair";
    if (quality >= 20) return "weak";
    return "very weak";
}

ToolResult WifiTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    std::string action = (action_it != params.end()) ? action_it->second : "current";
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    HANDLE hClient = NULL;
    DWORD dwMaxClient = 2;
    DWORD dwCurVersion = 0;

    DWORD ret = WlanOpenHandle(dwMaxClient, NULL, &dwCurVersion, &hClient);
    if (ret != ERROR_SUCCESS) {
        return {false, "", "Failed to open WLAN handle. WiFi may not be available."};
    }

    PWLAN_INTERFACE_INFO_LIST pIfList = NULL;
    ret = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (ret != ERROR_SUCCESS || pIfList == NULL || pIfList->dwNumberOfItems == 0) {
        WlanCloseHandle(hClient, NULL);
        return {false, "", "No WiFi interfaces found."};
    }

    GUID ifGuid = pIfList->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(pIfList);

    if (action == "current") {
        PWLAN_CONNECTION_ATTRIBUTES pConnAttr = NULL;
        DWORD dataSize = 0;
        WLAN_OPCODE_VALUE_TYPE opCode;

        ret = WlanQueryInterface(hClient, &ifGuid, wlan_intf_opcode_current_connection,
                                 NULL, &dataSize, (PVOID*)&pConnAttr, &opCode);

        if (ret != ERROR_SUCCESS || pConnAttr == NULL) {
            WlanCloseHandle(hClient, NULL);
            return {true, "Not connected to any WiFi network.", ""};
        }

        // Extract SSID
        WLAN_ASSOCIATION_ATTRIBUTES& assoc = pConnAttr->wlanAssociationAttributes;
        std::string ssid(reinterpret_cast<char*>(assoc.dot11Ssid.ucSSID), assoc.dot11Ssid.uSSIDLength);
        ULONG quality = assoc.wlanSignalQuality;

        std::string result = "Connected to WiFi: " + ssid +
                             " (Signal: " + signal_quality_to_bars(quality) +
                             ", " + std::to_string(quality) + "%)";

        WlanFreeMemory(pConnAttr);
        WlanCloseHandle(hClient, NULL);
        return {true, result, ""};

    } else if (action == "list") {
        // Scan first (non-blocking trigger)
        WlanScan(hClient, &ifGuid, NULL, NULL, NULL);
        Sleep(500); // Brief wait for scan

        PWLAN_AVAILABLE_NETWORK_LIST pNetworkList = NULL;
        ret = WlanGetAvailableNetworkList(hClient, &ifGuid, 0, NULL, &pNetworkList);

        if (ret != ERROR_SUCCESS || pNetworkList == NULL) {
            WlanCloseHandle(hClient, NULL);
            return {false, "", "Failed to get available network list."};
        }

        std::string result = "Available WiFi networks (" + std::to_string(pNetworkList->dwNumberOfItems) + "):\n";
        int shown = 0;
        for (DWORD i = 0; i < pNetworkList->dwNumberOfItems && shown < 10; i++) {
            WLAN_AVAILABLE_NETWORK& net = pNetworkList->Network[i];
            if (net.dot11Ssid.uSSIDLength == 0) continue;

            std::string ssid(reinterpret_cast<char*>(net.dot11Ssid.ucSSID), net.dot11Ssid.uSSIDLength);
            ULONG quality = net.wlanSignalQuality;
            bool connected = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
            bool secure = (net.bSecurityEnabled != 0);

            result += std::to_string(shown + 1) + ". " + ssid +
                      " (" + signal_quality_to_bars(quality) + ", " +
                      (secure ? "secured" : "open") +
                      (connected ? ", connected" : "") + ")\n";
            shown++;
        }

        WlanFreeMemory(pNetworkList);
        WlanCloseHandle(hClient, NULL);
        return {true, result, ""};
    }

    WlanCloseHandle(hClient, NULL);
    return {false, "", "Unknown action: " + action + ". Use 'current' or 'list'."};
}

} // namespace Jarvis
