#include "FileExplorerTool.h"
#include <iostream>
#include <filesystem>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlguid.h>
#include <exdisp.h>
#include <exdispid.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <regex>
#include <queue>
#include <set>
#include <thread>
#include <future>
#include "Logger.h"
#include "PathUtil.h"

// CSIDL_DOWNLOADS is not available in older Windows SDKs
#ifndef CSIDL_DOWNLOADS
#define CSIDL_DOWNLOADS 0x0005
#endif

namespace Jarvis {

// ============================================================================
// Forward declarations
// ============================================================================

// Helper: open a folder in the same Explorer window if one is already open,
// or open a new Explorer window if none is found.
// This prevents opening a new window every time a subfolder is navigated to.
static void open_explorer_window(const std::string& target_path) {
    // First, try to find an existing Explorer window and navigate it in-place
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        IShellWindows* pShellWindows = NULL;
        hr = CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, (void**)&pShellWindows);
        if (SUCCEEDED(hr) && pShellWindows) {
            // Get the PIDL for the target path
            PIDLIST_ABSOLUTE pidlTarget = NULL;
            hr = SHParseDisplayName(std::wstring(target_path.begin(), target_path.end()).c_str(), NULL, &pidlTarget, 0, NULL);
            if (SUCCEEDED(hr) && pidlTarget) {
                LONG count = 0;
                pShellWindows->get_Count(&count);
                for (LONG i = 0; i < count; i++) {
                    VARIANT vIndex;
                    VariantInit(&vIndex);
                    vIndex.vt = VT_I4;
                    vIndex.lVal = i;
                    IDispatch* pDisp = NULL;
                    hr = pShellWindows->Item(vIndex, &pDisp);
                    if (SUCCEEDED(hr) && pDisp) {
                        IWebBrowser2* pWebBrowser = NULL;
                        hr = pDisp->QueryInterface(IID_IWebBrowser2, (void**)&pWebBrowser);
                        if (SUCCEEDED(hr) && pWebBrowser) {
                            // Check if this window is a file explorer (not Internet Explorer)
                            VARIANT_BOOL isOffline;
                            pWebBrowser->get_Offline(&isOffline);
                            
                            // Get the location name to see if it's a local folder
                            BSTR bstrLocationName = NULL;
                            pWebBrowser->get_LocationName(&bstrLocationName);
                            
                            // Get the location URL
                            BSTR bstrLocationURL = NULL;
                            pWebBrowser->get_LocationURL(&bstrLocationURL);
                            
                            // Check if it's a file:// URL (local folder)
                            bool isFileExplorer = false;
                            if (bstrLocationURL) {
                                std::wstring url(bstrLocationURL);
                                if (url.find(L"file:///") == 0 || url.find(L"file:\\\\") == 0) {
                                    isFileExplorer = true;
                                }
                                SysFreeString(bstrLocationURL);
                            }
                            if (bstrLocationName) {
                                SysFreeString(bstrLocationName);
                            }
                            
                            if (isFileExplorer) {
                                // Navigate this Explorer window to the target folder
                                VARIANT vEmpty;
                                VariantInit(&vEmpty);
                                VARIANT vNavFlags;
                                VariantInit(&vNavFlags);
                                vNavFlags.vt = VT_I4;
                                vNavFlags.lVal = navNoHistory;  // Don't add to history since we manage it
                                
                                // Also include navBrowserBar to keep same view settings
                                vNavFlags.lVal |= navBrowserBar;
                                
                                VARIANT vTargetFrameName;
                                VariantInit(&vTargetFrameName);
                                vTargetFrameName.vt = VT_BSTR;
                                
                                // Prepare the target URL as a file:// URL
                                std::wstring targetUrl = L"file:///" + std::wstring(target_path.begin(), target_path.end());
                                // Replace backslashes with forward slashes for URL
                                size_t pos = 0;
                                while ((pos = targetUrl.find(L'\\', pos)) != std::wstring::npos) {
                                    targetUrl.replace(pos, 1, L"/");
                                    pos++;
                                }
                                
                                vTargetFrameName.bstrVal = SysAllocString(targetUrl.c_str());
                                
                                // Also include shell explorer navigation flag to properly open in the same window
                                VARIANT vPostData;
                                VariantInit(&vPostData);
                                VARIANT vHeaders;
                                VariantInit(&vHeaders);
                                
                                // Use Navigate2 with a PIDL for proper shell navigation (maintains same window)
                                SAFEARRAY* psa = SafeArrayCreateVector(VT_UI1, 0, ILGetSize(pidlTarget));
                                if (psa) {
                                    void* pData = NULL;
                                    hr = SafeArrayAccessData(psa, &pData);
                                    if (SUCCEEDED(hr)) {
                                        CopyMemory(pData, pidlTarget, ILGetSize(pidlTarget));
                                        SafeArrayUnaccessData(psa);
                                        
                                        VARIANT vPidl;
                                        VariantInit(&vPidl);
                                        vPidl.vt = VT_ARRAY | VT_UI1;
                                        vPidl.parray = psa;
                                        
                                        // Navigate2 with PIDL - this ensures the same window is reused
                                        // and the navigation integrates with Explorer's back/forward history
                                        pWebBrowser->Navigate2(&vPidl, &vNavFlags, &vEmpty, &vEmpty, &vEmpty);
                                        
                                        VariantClear(&vPidl);
                                    } else {
                                        SafeArrayDestroy(psa);
                                        psa = NULL;
                                    }
                                }
                                
                                // If SafeArray approach failed, fall back to URL navigation
                                if (!psa) {
                                    pWebBrowser->Navigate2(&vTargetFrameName, &vNavFlags, &vEmpty, &vPostData, &vHeaders);
                                }
                                
                                VariantClear(&vTargetFrameName);
                                VariantClear(&vNavFlags);
                                VariantClear(&vEmpty);
                                VariantClear(&vPostData);
                                VariantClear(&vHeaders);
                                
                                // Bring the window to the foreground
                                HWND hwnd = NULL;
                                pWebBrowser->get_HWND((SHANDLE_PTR*)&hwnd);
                                if (hwnd) {
                                    SetForegroundWindow(hwnd);
                                }
                                
                                pWebBrowser->Release();
                                pDisp->Release();
                                
                                // Free the PIDL
                                ILFree(pidlTarget);
                                pShellWindows->Release();
                                CoUninitialize();
                                return;  // Successfully navigated existing window
                            }
                            pWebBrowser->Release();
                        }
                        pDisp->Release();
                    }
                    VariantClear(&vIndex);
                }
                ILFree(pidlTarget);
            }
            pShellWindows->Release();
        }
        CoUninitialize();
    }

    // Fallback: open a new Explorer window
    ShellExecuteA(NULL, "open", target_path.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// ============================================================================
// Construction & Initialization
// ============================================================================

FileExplorerTool::FileExplorerTool()
    : initialized_(false), explorer_open_(false),
      fileIndex_(std::make_unique<FileIndex>()) {
    initialize();
    initialize_known_paths();
}

FileExplorerTool::~FileExplorerTool() {
    if (fileIndex_) {
        std::string indexPath = fileIndex_->getDefaultIndexPath();
        fileIndex_->saveIndex(indexPath);
        fileIndex_->stopIndexing();
    }
}

bool FileExplorerTool::initialize() {
    char home_path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, home_path) == S_OK) {
        current_directory_ = home_path;
        initialized_ = true;

        if (fileIndex_) {
            std::string indexPath = fileIndex_->getDefaultIndexPath();
            if (!fileIndex_->loadIndex(indexPath)) {
                LOG_FILEEXPLORER("[FileExplorer] No saved index found, starting fresh indexing");
                fileIndex_->startIndexing(current_directory_);
            } else {
                LOG_FILEEXPLORER("[FileExplorer] Index loaded from disk successfully");
            }
        }
        return true;
    }

    current_directory_ = pathutil::wide_to_utf8(std::filesystem::current_path().wstring());
    initialized_ = true;

    if (fileIndex_) {
        std::string indexPath = fileIndex_->getDefaultIndexPath();
        if (!fileIndex_->loadIndex(indexPath)) {
            fileIndex_->startIndexing(current_directory_);
        }
    }
    return true;
}

void FileExplorerTool::initialize_known_paths() {
    // Initialize known system paths with their aliases for natural language resolution.
    // Uses the W API + UTF-8 conversion: SHGetFolderPathA would emit ANSI-code-page
    // bytes that break for non-ASCII user profile names.
    auto shfolder_utf8 = [](int csidl) -> std::string {
        wchar_t wbuf[MAX_PATH];
        if (SHGetFolderPathW(NULL, csidl, NULL, 0, wbuf) != S_OK) return "";
        return pathutil::wide_to_utf8(wbuf);
    };

    known_system_paths_.clear();

    // User profile shortcuts
    std::string p = shfolder_utf8(CSIDL_PROFILE);
    if (!p.empty()) known_system_paths_.push_back({"home", p, {"home", "user", "profile", "~", "my home", "user folder"}});
    p = shfolder_utf8(CSIDL_DESKTOP);
    if (!p.empty()) known_system_paths_.push_back({"desktop", p, {"desktop", "my desktop", "desktop folder"}});
    p = shfolder_utf8(CSIDL_MYDOCUMENTS);
    if (!p.empty()) known_system_paths_.push_back({"documents", p, {"documents", "my documents", "docs", "my docs", "document folder"}});
    p = shfolder_utf8(CSIDL_MYPICTURES);
    if (!p.empty()) known_system_paths_.push_back({"pictures", p, {"pictures", "my pictures", "photos", "images", "pic"}});
    p = shfolder_utf8(CSIDL_MYMUSIC);
    if (!p.empty()) known_system_paths_.push_back({"music", p, {"music", "my music", "media", "audio", "tunes"}});
    p = shfolder_utf8(CSIDL_MYVIDEO);
    if (!p.empty()) known_system_paths_.push_back({"videos", p, {"videos", "my videos", "movies", "video"}});

    // Downloads
    p = shfolder_utf8(CSIDL_PROFILE);
    if (!p.empty()) {
        std::string dl = p + "\\Downloads";
        if (safe_fs_exists(dl)) {
            known_system_paths_.push_back({"downloads", dl, {"downloads", "download", "dl"}});
        }
    }

    // Additional system paths
    p = shfolder_utf8(CSIDL_SYSTEM);
    if (!p.empty()) known_system_paths_.push_back({"system32", p, {"system32", "system", "sys"}});
    p = shfolder_utf8(CSIDL_PROGRAM_FILES);
    if (!p.empty()) known_system_paths_.push_back({"program files", p, {"program files", "programs", "pf"}});
    p = shfolder_utf8(CSIDL_PROGRAM_FILES_COMMON);
    if (!p.empty()) known_system_paths_.push_back({"common files", p, {"common files", "common", "shared"}});
    p = shfolder_utf8(CSIDL_APPDATA);
    if (!p.empty()) known_system_paths_.push_back({"appdata", p, {"appdata", "roaming", "application data"}});
    p = shfolder_utf8(CSIDL_LOCAL_APPDATA);
    if (!p.empty()) known_system_paths_.push_back({"local appdata", p, {"local appdata", "local", "appdata local"}});
    p = shfolder_utf8(CSIDL_STARTUP);
    if (!p.empty()) known_system_paths_.push_back({"startup", p, {"startup", "start up"}});
    p = shfolder_utf8(CSIDL_FAVORITES);
    if (!p.empty()) known_system_paths_.push_back({"favorites", p, {"favorites", "favourites", "bookmarks"}});
    p = shfolder_utf8(CSIDL_RECENT);
    if (!p.empty()) known_system_paths_.push_back({"recent", p, {"recent", "recent items", "recent files"}});
    p = shfolder_utf8(CSIDL_FONTS);
    if (!p.empty()) known_system_paths_.push_back({"fonts", p, {"fonts", "font"}});
}

// ============================================================================
// Parameters & Execute
// ============================================================================

std::vector<ToolParameter> FileExplorerTool::getParameters() const {
    return {
        {"action", "string", "The action to perform: 'list', 'open', 'run', 'back', 'forward', 'suggest'", true, ""},
        {"path", "string", "The path, filename, or natural language description (e.g., 'my documents', 'downloads', 'desktop')", false, ""}
    };
}

ToolResult FileExplorerTool::execute(const std::map<std::string, std::string>& params) {
    auto action_it = params.find("action");
    if (action_it == params.end()) {
        return {false, "", "Missing required parameter: action"};
    }

    std::string action = action_it->second;
    std::string path = params.count("path") ? params.at("path") : "";

    LOG_FILEEXPLORER(std::string("[FileExplorer] Action: ") + action + ", Path: " + path);

    if (action == "list") {
        std::string result = list_directory(path.empty() ? current_directory_ : path);
        return {true, result, ""};
    } else if (action == "open") {
        if (path.empty()) {
            return {false, "", "Missing required parameter: path for open action"};
        }
        std::string result = navigate_to(path);
        return {true, result, ""};
    } else if (action == "back") {
        std::string result = go_back();
        return {true, result, ""};
    } else if (action == "forward") {
        std::string result = go_forward();
        return {true, result, ""};
    } else if (action == "run") {
        if (path.empty()) {
            return {false, "", "Missing required parameter: path for run action"};
        }
        std::string result = run_file(path);
        return {true, result, ""};
    } else if (action == "suggest") {
        auto suggestions = get_path_suggestions(path);
        if (suggestions.empty()) {
            return {true, "No suggestions found for: " + path, ""};
        }
        std::string result = "Suggestions for '" + path + "':\n";
        for (size_t i = 0; i < suggestions.size(); i++) {
            result += "  " + std::to_string(i + 1) + ". " + suggestions[i] + "\n";
        }
        return {true, result, ""};
    } else {
        return {false, "", "Unknown action: " + action + ". Valid actions: list, open, back, forward, run, suggest"};
    }
}

// ============================================================================
// CORE OPERATIONS
// ============================================================================

std::string FileExplorerTool::list_directory(const std::string& path) {
    std::string target_path = resolve_special_path(path);

    // Ensure we have a trailing backslash for proper directory listing
    if (!target_path.empty() && target_path.back() != '\\') {
        target_path += '\\';
    }
    // Remove the trailing backslash for consistency
    if (target_path.size() > 3 && target_path.back() == '\\') {
        target_path.pop_back();
    }

    std::string result;

    // Try to use the index first if it's ready
    if (fileIndex_ && fileIndex_->isReady()) {
        auto files = fileIndex_->listDirectory(target_path);
        if (!files.empty()) {
            result = "Contents of " + target_path + ":\n\n";

            // Sort: directories first, then by name
            std::sort(files.begin(), files.end(), [](const FileMetadata& a, const FileMetadata& b) {
                if (a.is_directory != b.is_directory) {
                    return a.is_directory > b.is_directory; // directories first
                }
                // Case-insensitive alphabetical sort
                std::string a_lower = a.name;
                std::string b_lower = b.name;
                std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), ::tolower);
                std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), ::tolower);
                return a_lower < b_lower;
            });

            // List directories first
            result += "Folders:\n";
            int dir_count = 0;
            for (const auto& metadata : files) {
                if (metadata.is_directory) {
                    result += "  [DIR] " + metadata.name + "\n";
                    dir_count++;
                }
            }

            // Then list files with size and type
            result += "\nFiles:\n";
            int file_count = 0;
            for (const auto& metadata : files) {
                if (!metadata.is_directory) {
                    std::string size_str = format_file_size(metadata.size);
                    std::string category = get_file_category(metadata.extension);
                    result += "  " + metadata.name + "  (" + size_str + ", " + category + ")\n";
                    file_count++;
                }
            }

            result += "\nTotal: " + std::to_string(dir_count) + " folders, " + std::to_string(file_count) + " files";
            return result;
        }
    }

    // Fallback to filesystem scan if index not ready or path not in index
    if (!safe_fs_exists(target_path)) {
        return "Error: Path does not exist: " + target_path;
    }

    if (!safe_fs_is_directory(target_path)) {
        return "Error: Not a directory: " + target_path;
    }

    result = "Contents of " + target_path + ":\n\n";

    // Collect entries first, then sort
    struct DirEntry {
        std::string name;
        bool is_dir;
        uint64_t size;
        std::string extension;
    };
    std::vector<DirEntry> entries;

    try {
        std::string api_path = normalize_path_for_api(target_path);
        for (const auto& entry : std::filesystem::directory_iterator(
                api_path,
                std::filesystem::directory_options::skip_permission_denied)) {
            DirEntry de;
            // Convert via UTF-16 -> UTF-8: .string() would use the ANSI code
            // page and THROW for names it cannot represent, aborting the
            // entire listing (one weird file used to kill the whole view).
            de.name = pathutil::wide_to_utf8(entry.path().filename().wstring());
            de.is_dir = entry.is_directory();
            de.extension = pathutil::wide_to_utf8(entry.path().extension().wstring());
            if (!de.is_dir) {
                try { de.size = entry.file_size(); } catch (...) { de.size = 0; }
            } else {
                de.size = 0;
            }
            entries.push_back(de);
        }
    } catch (const std::exception& e) {
        return "Error accessing directory: " + std::string(e.what());
    }

    // Sort: directories first, then by name
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        std::string a_lower = a.name, b_lower = b.name;
        std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), ::tolower);
        std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), ::tolower);
        return a_lower < b_lower;
    });

    result += "Folders:\n";
    int dir_count = 0, file_count = 0;
    for (const auto& e : entries) {
        if (e.is_dir) {
            result += "  [DIR] " + e.name + "\n";
            dir_count++;
        }
    }

    result += "\nFiles:\n";
    for (const auto& e : entries) {
        if (!e.is_dir) {
            result += "  " + e.name + "  (" + format_file_size(e.size) + ", " + get_file_category(e.extension) + ")\n";
            file_count++;
        }
    }

    result += "\nTotal: " + std::to_string(dir_count) + " folders, " + std::to_string(file_count) + " files";
    return result;
}

std::string FileExplorerTool::navigate_to(const std::string& path) {
    LOG_FILEEXPLORER(std::string("[FileExplorer] navigate_to called with: ") + path);
    std::string target_path = resolve_special_path(path);
    LOG_FILEEXPLORER(std::string("[FileExplorer] Resolved path: ") + target_path);

    if (!safe_fs_exists(target_path)) {
        return "Error: Path does not exist: " + target_path;
    }

    // If it's a file, run it instead of navigating
    if (!safe_fs_is_directory(target_path)) {
        LOG_FILEEXPLORER("[FileExplorer] Path is a file, running it instead");
        return run_file(target_path);
    }

    // Store the previous directory in navigation history
    if (current_directory_ != target_path) {
        navigation_history_.push_back(current_directory_);
        // Clear forward history when navigating to a new path
        forward_history_.clear();
    }
    current_directory_ = target_path;
    LOG_FILEEXPLORER(std::string("[FileExplorer] Current directory set to: ") + current_directory_);

    // Track this path access for learning preferences
    track_path_access(target_path);

    // Trigger predictive caching for subfolders
    if (fileIndex_ && fileIndex_->isReady()) {
        fileIndex_->prefetchSubfolders(target_path);
    }

    // Open folder in Explorer
    open_explorer_window(target_path);

    // Return a concise confirmation - NOT the full directory listing.
    // The full listing would be spoken aloud by TTS, which is undesirable.
    return "Opened: " + current_directory_;
}

std::string FileExplorerTool::go_back() {
    if (!navigation_history_.empty()) {
        std::string prev_directory = navigation_history_.back();
        navigation_history_.pop_back();
        // Save current directory to forward history
        forward_history_.push_back(current_directory_);
        LOG_FILEEXPLORER(std::string("[FileExplorer] Going back to: ") + prev_directory);

        open_explorer_window(prev_directory);
        current_directory_ = prev_directory;
        return "Went back to: " + current_directory_;
    }

    // Fallback: try parent path if no history
    std::filesystem::path p(current_directory_);
    if (p.has_parent_path()) {
        std::string parent_path = p.parent_path().string();
        LOG_FILEEXPLORER(std::string("[FileExplorer] Going back to parent: ") + parent_path);
        forward_history_.push_back(current_directory_);
        open_explorer_window(parent_path);
        current_directory_ = parent_path;
        return "Went back to: " + current_directory_;
    }
    return "Already at root directory: " + current_directory_;
}

std::string FileExplorerTool::go_forward() {
    if (!forward_history_.empty()) {
        std::string next_directory = forward_history_.back();
        forward_history_.pop_back();
        navigation_history_.push_back(current_directory_);
        LOG_FILEEXPLORER(std::string("[FileExplorer] Going forward to: ") + next_directory);

        open_explorer_window(next_directory);
        current_directory_ = next_directory;
        return "Went forward to: " + current_directory_;
    }
    return "No forward history available.";
}

std::string FileExplorerTool::run_file(const std::string& filename) {
    // Wide-backed path: file names the ANSI code page cannot represent
    // (fullwidth quotes etc.) are handled exactly, never thrown away.
    std::filesystem::path file_path;

    // First, try to resolve the filename intelligently
    std::string resolved = resolve_special_path(filename);

    // Check if resolved path exists
    if (safe_fs_exists(resolved)) {
        file_path = std::filesystem::path(pathutil::utf8_to_wide(resolved));
    } else {
        // Search in current directory - try exact match (wide, UTF-8-safe)
        std::error_code ec;
        file_path = std::filesystem::path(pathutil::utf8_to_wide(current_directory_))
                  / pathutil::utf8_to_wide(filename);
        if (!std::filesystem::exists(file_path, ec)) {
            // No exact match found, try case-insensitive substring search in current directory
            std::string filename_lower = filename;
            std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

            std::wstring best_match;
            int best_score = 0;

            try {
                std::filesystem::path curdir(pathutil::utf8_to_wide(current_directory_));
                for (const auto& entry : std::filesystem::directory_iterator(
                        curdir,
                        std::filesystem::directory_options::skip_permission_denied)) {
                    if (entry.is_regular_file()) {
                        // UTF-16 -> UTF-8 for comparison (never .string(): it
                        // throws on names the ANSI code page cannot encode)
                        std::string entry_name = pathutil::wide_to_utf8(entry.path().filename().wstring());
                        std::string entry_name_lower = entry_name;
                        std::transform(entry_name_lower.begin(), entry_name_lower.end(), entry_name_lower.begin(), ::tolower);

                        int score = 0;
                        // Check for exact match
                        if (entry_name_lower == filename_lower) {
                            score = 100;
                        }
                        // Check for starts-with match
                        else if (entry_name_lower.find(filename_lower) == 0) {
                            score = 50 + (int)filename_lower.length();
                        }
                        // Check for substring match
                        else if (entry_name_lower.find(filename_lower) != std::string::npos) {
                            score = (int)filename_lower.length();
                        }

                        if (score > best_score) {
                            best_score = score;
                            best_match = entry.path().wstring();
                        }
                    }
                }
            } catch (const std::exception& e) {
                return "Error accessing directory: " + std::string(e.what());
            }

            if (!best_match.empty()) {
                file_path = std::filesystem::path(best_match);
                LOG_FILEEXPLORER(std::string("[FileExplorer] Fuzzy matched file: ") + filename
                                 + " -> " + pathutil::wide_to_utf8(file_path.wstring()));
            } else {
                return "Error: File not found: " + filename;
            }
        }
    }

    std::error_code exists_ec;
    if (!std::filesystem::exists(file_path, exists_ec)) {
        return "Error: File not found: " + pathutil::wide_to_utf8(file_path.wstring());
    }

    // Use ShellExecuteW so names outside the ANSI code page open correctly
    HINSTANCE result = ShellExecuteW(NULL, L"open", file_path.c_str(), NULL, NULL, SW_SHOWNORMAL);

    if ((intptr_t)result <= 32) {
        return "Error: Failed to open file: " + pathutil::wide_to_utf8(file_path.wstring());
    }

    std::string opened_utf8 = pathutil::wide_to_utf8(file_path.wstring());
    track_path_access(opened_utf8);
    return "Opened: " + opened_utf8;
}

// ============================================================================
// INTELLIGENT PATH RESOLUTION - Multi-stage pipeline
// ============================================================================

std::string FileExplorerTool::resolve_special_path(const std::string& path) {
    if (path.empty()) {
        return current_directory_;
    }

    // Handle "." and ".." before cache check since they are relative to current directory
    if (path == "." || path == ".\\") {
        return current_directory_;
    }
    if (path == ".." || path == "..\\") {
        std::filesystem::path p(current_directory_);
        if (p.has_parent_path()) {
            return p.parent_path().string();
        }
        return current_directory_;
    }

    // Step 0: Check cache first
    std::string cached = get_cached_resolution(path);
    if (!cached.empty()) {
        LOG_FILEEXPLORER(std::string("[FileExplorer] Cache hit for: ") + path + " -> " + cached);
        return cached;
    }

    // Step 1: Expand environment variables
    std::string expanded = expand_environment_variables(path);
    if (expanded != path && safe_fs_exists(expanded)) {
        set_cached_resolution(path, expanded);
        return expanded;
    }

    // Remove leading separators
    std::string clean_path = expanded;
    while (!clean_path.empty() && (clean_path[0] == '/' || clean_path[0] == '\\')) {
        clean_path = clean_path.substr(1);
    }

    // Step 1a: Check for single-letter drive (e.g., "c", "d", "e") BEFORE relative path check
    std::string clean_path_lower = clean_path;
    std::transform(clean_path_lower.begin(), clean_path_lower.end(), clean_path_lower.begin(), ::tolower);
    if (clean_path_lower.length() == 1 && clean_path_lower[0] >= 'a' && clean_path_lower[0] <= 'z') {
        std::string drive_root = std::string(1, toupper(clean_path_lower[0])) + ":\\";
        if (safe_fs_exists(drive_root)) {
            set_cached_resolution(path, drive_root);
            return drive_root;
        }
    }

    // Step 1b: Check if it's an absolute path that exists
    if (safe_fs_exists(clean_path)) {
        set_cached_resolution(path, clean_path);
        return clean_path;
    }

    // Step 1c: Check if it's a relative path from current directory
    std::filesystem::path test_rel = std::filesystem::path(current_directory_) / clean_path;
    if (safe_fs_exists(test_rel.string())) {
        set_cached_resolution(path, test_rel.string());
        return test_rel.string();
    }

    // Step 2: Resolve known system paths and aliases
    std::string known = resolve_known_system_path(clean_path);
    if (!known.empty()) {
        set_cached_resolution(path, known);
        return known;
    }

    // Step 3: Handle relative paths and special tokens (., ..)
    std::string relative = resolve_relative_path(clean_path);
    if (!relative.empty()) {
        set_cached_resolution(path, relative);
        return relative;
    }

    // Step 4: Try partial path completion (e.g., "doc/something" -> "Documents/something")
    std::string partial = resolve_partial_path(clean_path);
    if (!partial.empty()) {
        set_cached_resolution(path, partial);
        return partial;
    }

    // Step 5: Smart fuzzy matching with Levenshtein distance
    std::string fuzzy = resolve_fuzzy_match(clean_path);
    if (!fuzzy.empty()) {
        set_cached_resolution(path, fuzzy);
        return fuzzy;
    }

    // Step 6: Check all available drives
    auto drives = get_available_drives();
    std::string drive_search = clean_path;
    std::transform(drive_search.begin(), drive_search.end(), drive_search.begin(), ::tolower);
    for (const auto& drive : drives) {
        std::string drive_lower = drive;
        std::transform(drive_lower.begin(), drive_lower.end(), drive_lower.begin(), ::tolower);
        if (drive_lower.find(drive_search) != std::string::npos ||
            drive_search.find(drive_lower) != std::string::npos) {
            set_cached_resolution(path, drive);
            return drive;
        }
    }

    // Step 7: Recursive search with depth limit
    auto results = recursive_search(clean_path, 3, 5);
    if (!results.empty()) {
            LOG_FILEEXPLORER(std::string("[FileExplorer] Recursive search found: ") + results[0].path + " (score: " + std::to_string(results[0].score) + ")");
        set_cached_resolution(path, results[0].path);
        return results[0].path;
    }

    // Return as-is (might be a valid path we couldn't verify)
    LOG_FILEEXPLORER(std::string("[FileExplorer] Path could not be resolved intelligently, returning as-is: ") + clean_path);
    return clean_path;
}

// Step 1: Expand environment variables
std::string FileExplorerTool::expand_environment_variables(const std::string& path) {
    std::string result = path;
    size_t start_pos = 0;

    while ((start_pos = result.find('%', start_pos)) != std::string::npos) {
        size_t end_pos = result.find('%', start_pos + 1);
        if (end_pos == std::string::npos) break;

        std::string var_name = result.substr(start_pos + 1, end_pos - start_pos - 1);
        char env_value[MAX_PATH] = {0};
        DWORD env_size = GetEnvironmentVariableA(var_name.c_str(), env_value, MAX_PATH);

        if (env_size > 0 && env_size < MAX_PATH) {
            result.replace(start_pos, end_pos - start_pos + 1, env_value);
            start_pos += env_size;
        } else {
            start_pos = end_pos + 1;
        }
    }

    return result;
}

// Step 2: Resolve known system paths
std::string FileExplorerTool::resolve_known_system_path(const std::string& normalized) {
    std::string search_term = normalize_for_search(normalized);

    // Check for exact match with known paths
    for (const auto& known : known_system_paths_) {
        // Check name
        if (normalize_for_search(known.name) == search_term) {
            return known.path;
        }
        // Check aliases
        for (const auto& alias : known.aliases) {
            if (normalize_for_search(alias) == search_term) {
                return known.path;
            }
        }
    }

    // Check for fuzzy match with aliases (Levenshtein distance <= 2)
    for (const auto& known : known_system_paths_) {
        std::string known_norm = normalize_for_search(known.name);
        if (levenshtein_distance(known_norm, search_term) <= 2) {
            return known.path;
        }
        for (const auto& alias : known.aliases) {
            std::string alias_norm = normalize_for_search(alias);
            if (levenshtein_distance(alias_norm, search_term) <= 2) {
                return known.path;
            }
        }
    }

    // Check for drive letters (e.g., "c:", "d:", "c drive", "partition c")
    std::string clean_path_lower = normalized;
    std::transform(clean_path_lower.begin(), clean_path_lower.end(), clean_path_lower.begin(), ::tolower);

    // "partition X" or "drive X" patterns
    std::regex drive_pattern(R"((partition|drive)\s+([a-z]))", std::regex_constants::icase);
    std::smatch drive_match;
    if (std::regex_search(normalized, drive_match, drive_pattern)) {
        char drive_letter = toupper(drive_match[2].str()[0]);
        std::string drive_root = std::string(1, drive_letter) + ":\\";
        if (safe_fs_exists(drive_root)) {
            return drive_root;
        }
    }

    // Single letter drive (e.g., "c", "d", "e")
    if (clean_path_lower.length() == 1 && clean_path_lower[0] >= 'a' && clean_path_lower[0] <= 'z') {
        std::string drive_root = std::string(1, toupper(clean_path_lower[0])) + ":\\";
        if (safe_fs_exists(drive_root)) {
            return drive_root;
        }
    }

    // "X:" or "X:\" pattern
    if (clean_path_lower.length() >= 2 && clean_path_lower[0] >= 'a' && clean_path_lower[0] <= 'z' && clean_path_lower[1] == ':') {
        std::string drive_root = std::string(1, toupper(clean_path_lower[0])) + ":\\";
        if (safe_fs_exists(drive_root)) {
            return drive_root;
        }
    }

    return "";
}

// Step 3: Handle relative paths
std::string FileExplorerTool::resolve_relative_path(const std::string& path) {
    // Handle ".." (parent directory)
    if (path == ".." || path == "../" || path == "..\\") {
        std::filesystem::path p(current_directory_);
        if (p.has_parent_path()) {
            return p.parent_path().string();
        }
        return current_directory_;
    }

    // Handle "." (current directory)
    if (path == "." || path == "./" || path == ".\\" || path.empty()) {
        return current_directory_;
    }

    // Handle relative paths starting with ".." (e.g., "../sibling")
    if (path.find("..") == 0) {
        std::filesystem::path p(current_directory_);
        std::string remainder = path.substr(2);
        // Remove leading separators
        while (!remainder.empty() && (remainder[0] == '/' || remainder[0] == '\\')) {
            remainder = remainder.substr(1);
        }
        if (p.has_parent_path()) {
            std::string combined = (p.parent_path() / remainder).string();
            if (safe_fs_exists(combined)) {
                return combined;
            }
        }
    }

    // Check if it's a relative path from current directory
    std::filesystem::path test_path = std::filesystem::path(current_directory_) / path;
    if (safe_fs_exists(test_path.string())) {
        return test_path.string();
    }

    return "";
}

// Step 4: Smart fuzzy matching with Levenshtein distance
std::string FileExplorerTool::resolve_fuzzy_match(const std::string& search_term, bool prefer_directories) {
    std::vector<FuzzyMatchResult> matches = get_fuzzy_matches(search_term);

    if (matches.empty()) {
        return "";
    }

    // Sort by composite score (lower Levenshtein = better, higher substring = better, preferred = bonus)
    std::sort(matches.begin(), matches.end(), [](const FuzzyMatchResult& a, const FuzzyMatchResult& b) {
        // Calculate composite scores (lower = better)
        int score_a = a.levenshtein_distance * 10 - a.substring_score - (a.is_preferred ? 5 : 0);
        int score_b = b.levenshtein_distance * 10 - b.substring_score - (b.is_preferred ? 5 : 0);
        return score_a < score_b;
    });

    // Log the top matches
    LOG_FILEEXPLORER(std::string("[FileExplorer] Fuzzy matches for '") + search_term + "':");
    for (size_t i = 0; i < std::min(matches.size(), size_t(5)); i++) {
        std::ostringstream oss;
        oss << "  [" << i << "] " << matches[i].name
            << " (lev=" << matches[i].levenshtein_distance
            << ", sub=" << matches[i].substring_score
            << ", dir=" << matches[i].is_directory
            << ", pref=" << matches[i].is_preferred << ")";
        LOG_FILEEXPLORER(oss.str());
    }

    // Return the best match
    if (prefer_directories) {
        // Prefer directories among top matches
        for (const auto& match : matches) {
            if (match.is_directory) {
                return match.path;
            }
        }
    }

    return matches[0].path;
}

std::vector<FileExplorerTool::FuzzyMatchResult> FileExplorerTool::get_fuzzy_matches(const std::string& search_term, int max_results) {
    std::vector<FuzzyMatchResult> results;
    std::string search_lower = normalize_for_search(search_term);

    if (search_lower.empty()) {
        return results;
    }

    // Get user preferences for bonus scoring
    auto prefs = get_top_preferences(10);
    std::set<std::string> pref_paths;
    for (const auto& p : prefs) {
        pref_paths.insert(p.path);
    }

    // Search in current directory using index
    if (fileIndex_ && fileIndex_->isReady()) {
        auto entries = fileIndex_->listDirectory(current_directory_);

        for (const auto& metadata : entries) {
            std::string entry_lower = normalize_for_search(metadata.name);
            int lev_dist = levenshtein_distance(entry_lower, search_lower);
            int sub_score = 0;

            // Calculate substring score
            if (entry_lower.find(search_lower) != std::string::npos) {
                sub_score = (int)search_lower.length() * 2;
            } else if (search_lower.find(entry_lower) != std::string::npos) {
                sub_score = (int)entry_lower.length();
            }
            // Check for prefix match
            else if (entry_lower.find(search_lower) == 0) {
                sub_score = (int)search_lower.length() * 3;
            }
            // Check for character-by-character subsequence match
            else {
                int subseq = 0;
                size_t ei = 0, si = 0;
                while (ei < entry_lower.length() && si < search_lower.length()) {
                    if (entry_lower[ei] == search_lower[si]) {
                        subseq++;
                        si++;
                    }
                    ei++;
                }
                if (si == search_lower.length()) {
                    sub_score = subseq * 2;
                }
            }

            // Only include if it's a reasonable match
            if (lev_dist <= 3 || sub_score > 0) {
                FuzzyMatchResult match;
                match.path = metadata.path;
                match.name = metadata.name;
                match.levenshtein_distance = lev_dist;
                match.substring_score = sub_score;
                match.is_directory = metadata.is_directory;
                match.is_preferred = (pref_paths.find(metadata.path) != pref_paths.end());
                results.push_back(match);
            }
        }
    }

    // Sort by composite score
    std::sort(results.begin(), results.end(), [](const FuzzyMatchResult& a, const FuzzyMatchResult& b) {
        int score_a = a.levenshtein_distance * 10 - a.substring_score - (a.is_preferred ? 5 : 0);
        int score_b = b.levenshtein_distance * 10 - b.substring_score - (b.is_preferred ? 5 : 0);
        return score_a < score_b;
    });

    // Return top results
    if ((int)results.size() > max_results) {
        results.resize(max_results);
    }

    return results;
}

int FileExplorerTool::levenshtein_distance(const std::string& s1, const std::string& s2) const {
    size_t m = s1.length();
    size_t n = s2.length();

    // Quick optimizations
    if (m == 0) return (int)n;
    if (n == 0) return (int)m;
    if (abs((int)m - (int)n) > 3) {
        // If lengths differ by more than 3, it's likely a bad match
        // But still compute for short strings
        if (m > 5 && n > 5) return 4; // return a high value
    }

    // Use two-row optimization (only keep current and previous row)
    std::vector<int> prev(n + 1), curr(n + 1);

    for (size_t j = 0; j <= n; j++) {
        prev[j] = (int)j;
    }

    for (size_t i = 1; i <= m; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= n; j++) {
            int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            curr[j] = std::min({
                prev[j] + 1,       // deletion
                curr[j - 1] + 1,   // insertion
                prev[j - 1] + cost // substitution
            });
        }
        std::swap(prev, curr);
    }

    return prev[n];
}

// Step 5: Partial path completion
std::string FileExplorerTool::resolve_partial_path(const std::string& partial_path) {
    // Split the partial path into components
    std::string sep = "/\\";
    std::vector<std::string> components;
    std::string current;
    for (char c : partial_path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        components.push_back(current);
    }

    if (components.empty()) {
        return "";
    }

    // Start from current directory
    std::filesystem::path resolved = current_directory_;

    for (size_t i = 0; i < components.size(); i++) {
        const std::string& component = components[i];
        std::string comp_lower = component;
        std::transform(comp_lower.begin(), comp_lower.end(), comp_lower.begin(), ::tolower);

        // Handle ".." and "." in the middle of the path
        if (component == "..") {
            if (resolved.has_parent_path()) {
                resolved = resolved.parent_path();
            }
            continue;
        }
        if (component == ".") {
            continue;
        }

        // Try exact match first
        std::filesystem::path exact = resolved / component;
        if (safe_fs_exists(exact.string())) {
            resolved = exact;
            continue;
        }

        // Try case-insensitive search in the current resolved directory
        bool found = false;
        try {
            std::string api_resolved = normalize_path_for_api(resolved.string());
            for (const auto& entry : std::filesystem::directory_iterator(
                    api_resolved,
                    std::filesystem::directory_options::skip_permission_denied)) {
                std::string entry_name = entry.path().filename().string();
                std::string entry_lower = entry_name;
                std::transform(entry_lower.begin(), entry_lower.end(), entry_lower.begin(), ::tolower);

                if (entry_lower == comp_lower) {
                    resolved = entry.path();
                    found = true;
                    break;
                }
            }
        } catch (...) {
            // If we can't access this directory, stop here
            break;
        }

        if (!found) {
            // Try fuzzy match for this component
            if (fileIndex_ && fileIndex_->isReady()) {
                auto entries = fileIndex_->listDirectory(resolved.string());
                int best_score = 0;
                std::string best_match;
                for (const auto& metadata : entries) {
                    std::string entry_lower = metadata.name;
                    std::transform(entry_lower.begin(), entry_lower.end(), entry_lower.begin(), ::tolower);
                    int lev = levenshtein_distance(entry_lower, comp_lower);
                    if (lev <= 2 && (3 - lev) > best_score) {
                        best_score = 3 - lev;
                        best_match = metadata.path;
                    }
                }
                if (!best_match.empty()) {
                    resolved = best_match;
                    found = true;
                    continue;
                }
            }
            // If we can't find this component, stop
            return "";
        }
    }

    if (safe_fs_exists(resolved.string())) {
        return resolved.string();
    }

    return "";
}

// Step 6: Recursive search with scoring
std::vector<FileExplorerTool::SearchResult> FileExplorerTool::recursive_search(
    const std::string& query, int max_depth, int max_results) {

    std::vector<SearchResult> results;
    std::string query_lower = query;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    // Start from current directory, search recursively
    try {
        recursive_search_impl(current_directory_, query_lower, results, 0, max_depth, max_results);
    } catch (const std::exception& e) {
        std::cerr << "[FileExplorer] Recursive search error: " << e.what() << std::endl;
    }

    // Sort by score (higher = better)
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.score > b.score;
    });

    if ((int)results.size() > max_results) {
        results.resize(max_results);
    }

    return results;
}

void FileExplorerTool::recursive_search_impl(
    const std::filesystem::path& dir, const std::string& query_lower,
    std::vector<SearchResult>& results, int current_depth, int max_depth, int max_results) {

    if (current_depth >= max_depth) return;
    if ((int)results.size() >= max_results) return;

    try {
        std::string api_dir = normalize_path_for_api(dir.string());
        for (const auto& entry : std::filesystem::directory_iterator(
                api_dir,
                std::filesystem::directory_options::skip_permission_denied)) {
            std::string name = entry.path().filename().string();
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            int score = 0;

            // Exact match = highest score
            if (name_lower == query_lower) {
                score = 100;
            }
            // Starts with query
            else if (name_lower.find(query_lower) == 0) {
                score = 80 + (int)query_lower.length();
            }
            // Contains query
            else if (name_lower.find(query_lower) != std::string::npos) {
                score = 50 + (int)query_lower.length();
            }
            // Contains all characters in order (subsequence)
            else {
                size_t qi = 0, ni = 0;
                while (qi < query_lower.length() && ni < name_lower.length()) {
                    if (name_lower[ni] == query_lower[qi]) {
                        qi++;
                    }
                    ni++;
                }
                if (qi == query_lower.length()) {
                    score = 20 + (int)query_lower.length();
                }
            }

            // Bonus for exact match in any component (e.g., filename matches perfectly)
            if (score >= 50) {
                // Depth bonus: files closer to root are preferred
                score += std::max(0, (max_depth - current_depth) * 5);

                SearchResult sr;
                sr.path = entry.path().string();
                sr.name = name;
                sr.score = score;
                sr.is_directory = entry.is_directory();
                results.push_back(sr);
            }

            // Recurse into subdirectories
            if (entry.is_directory() && current_depth + 1 < max_depth && (int)results.size() < max_results) {
                recursive_search_impl(entry.path(), query_lower, results, current_depth + 1, max_depth, max_results);
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Skip directories we can't access
    }
}

// ============================================================================
// SMART SUGGESTIONS
// ============================================================================

std::vector<std::string> FileExplorerTool::get_path_suggestions(const std::string& partial) {
    std::vector<std::string> suggestions;
    std::string search_lower = normalize_for_search(partial);

    if (search_lower.empty()) {
        // Return recent paths and top preferences
        auto recent = get_recent_paths(5);
        for (const auto& r : recent) {
            suggestions.push_back(r);
        }
        auto prefs = get_top_preferences(5);
        for (const auto& p : prefs) {
            if (std::find(suggestions.begin(), suggestions.end(), p.path) == suggestions.end()) {
                suggestions.push_back(p.path);
            }
        }
        return suggestions;
    }

    // 1. Known system paths
    for (const auto& known : known_system_paths_) {
        std::string known_norm = normalize_for_search(known.name);
        if (known_norm.find(search_lower) != std::string::npos ||
            levenshtein_distance(known_norm, search_lower) <= 2) {
            suggestions.push_back(known.path + " (" + known.name + ")");
        }
        for (const auto& alias : known.aliases) {
            std::string alias_norm = normalize_for_search(alias);
            if (alias_norm.find(search_lower) != std::string::npos ||
                levenshtein_distance(alias_norm, search_lower) <= 2) {
                if (std::find(suggestions.begin(), suggestions.end(), known.path + " (" + known.name + ")") == suggestions.end()) {
                    suggestions.push_back(known.path + " (" + known.name + ")");
                }
            }
        }
    }

    // 2. Fuzzy matches from current directory
    if (fileIndex_ && fileIndex_->isReady()) {
        auto fuzzy_matches = get_fuzzy_matches(partial, 5);
        for (const auto& match : fuzzy_matches) {
            std::string suggestion = match.path;
            if (std::find(suggestions.begin(), suggestions.end(), suggestion) == suggestions.end()) {
                suggestions.push_back(suggestion);
            }
        }
    }

    // 3. User preferences
    auto prefs = get_top_preferences(10);
    for (const auto& p : prefs) {
        std::string pref_norm = normalize_for_search(p.name);
        if (pref_norm.find(search_lower) != std::string::npos ||
            p.path.find(search_lower) != std::string::npos) {
            if (std::find(suggestions.begin(), suggestions.end(), p.path) == suggestions.end()) {
                suggestions.push_back(p.path);
            }
        }
    }

    // 4. Available drives
    auto drives = get_available_drives();
    for (const auto& drive : drives) {
        std::string drive_lower = drive;
        std::transform(drive_lower.begin(), drive_lower.end(), drive_lower.begin(), ::tolower);
        if (drive_lower.find(search_lower) != std::string::npos) {
            suggestions.push_back(drive);
        }
    }

    // Limit suggestions
    if (suggestions.size() > 10) {
        suggestions.resize(10);
    }

    return suggestions;
}

void FileExplorerTool::track_path_access(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(prefs_mutex_);

    // Check if this path already exists in preferences
    for (auto& pref : path_preferences_) {
        if (pref.path == path) {
            pref.access_count++;
            pref.last_accessed = std::chrono::system_clock::now();
            return;
        }
    }

    // Add new preference
    PathPreference pref;
    pref.path = path;
    pref.name = std::filesystem::path(path).filename().string();
    pref.access_count = 1;
    pref.last_accessed = std::chrono::system_clock::now();
    path_preferences_.push_back(pref);

    // Evict oldest if over limit
    if (path_preferences_.size() > MAX_PREFERENCES) {
        path_preferences_.erase(
            std::min_element(path_preferences_.begin(), path_preferences_.end(),
                [](const PathPreference& a, const PathPreference& b) {
                    return a.last_accessed < b.last_accessed;
                })
        );
    }
}

std::vector<PathPreference> FileExplorerTool::get_top_preferences(int count) const {
    std::shared_lock<std::shared_mutex> lock(prefs_mutex_);

    std::vector<PathPreference> sorted = path_preferences_;
    std::sort(sorted.begin(), sorted.end(), [](const PathPreference& a, const PathPreference& b) {
        // Sort by access count (descending), then by recency (descending)
        if (a.access_count != b.access_count) {
            return a.access_count > b.access_count;
        }
        return a.last_accessed > b.last_accessed;
    });

    if ((int)sorted.size() > count) {
        sorted.resize(count);
    }
    return sorted;
}

std::vector<std::string> FileExplorerTool::get_recent_paths(int count) const {
    std::shared_lock<std::shared_mutex> lock(prefs_mutex_);

    std::vector<PathPreference> sorted = path_preferences_;
    std::sort(sorted.begin(), sorted.end(), [](const PathPreference& a, const PathPreference& b) {
        return a.last_accessed > b.last_accessed;
    });

    std::vector<std::string> recent;
    for (const auto& p : sorted) {
        recent.push_back(p.path);
        if ((int)recent.size() >= count) break;
    }
    return recent;
}

// ============================================================================
// PERFORMANCE OPTIMIZATIONS - Cache
// ============================================================================

std::string FileExplorerTool::get_cached_resolution(const std::string& input) {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);

    auto it = resolution_cache_.find(input);
    if (it != resolution_cache_.end()) {
        // Update access time and hit count
        it->second.last_access = std::chrono::steady_clock::now();
        it->second.hit_count++;
        return it->second.resolved_path;
    }
    return "";
}

void FileExplorerTool::set_cached_resolution(const std::string& input, const std::string& resolved) {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);

    PathCacheEntry entry;
    entry.resolved_path = resolved;
    entry.last_access = std::chrono::steady_clock::now();
    entry.hit_count = 1;
    resolution_cache_[input] = entry;

    evict_cache_if_needed();
}

void FileExplorerTool::evict_cache_if_needed() {
    if (resolution_cache_.size() <= MAX_CACHE_SIZE) {
        return;
    }

    // LRU eviction: remove oldest entries
    std::vector<std::pair<std::string, PathCacheEntry>> entries(
        resolution_cache_.begin(), resolution_cache_.end());

    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) {
            return a.second.last_access < b.second.last_access;
        });

    // Remove the oldest 25% of entries
    size_t to_remove = resolution_cache_.size() - MAX_CACHE_SIZE;
    for (size_t i = 0; i < to_remove && i < entries.size(); i++) {
        resolution_cache_.erase(entries[i].first);
    }
}

std::vector<std::string> FileExplorerTool::get_available_drives() const {
    std::vector<std::string> drives;
    DWORD drive_mask = GetLogicalDrives();
    for (char c = 'A'; c <= 'Z'; c++) {
        if (drive_mask & 1) {
            std::string drive = std::string(1, c) + ":\\";
            drives.push_back(drive);
        }
        drive_mask >>= 1;
    }
    return drives;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

std::string FileExplorerTool::normalize_for_search(const std::string& str) const {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    result.erase(std::remove_if(result.begin(), result.end(), [](char c) {
        return !std::isalnum(static_cast<unsigned char>(c)) && c != ' ';
    }), result.end());
    return result;
}

std::string FileExplorerTool::normalize_for_display(const std::string& str) const {
    std::string result = str;
    // Replace backslashes with forward slashes for display
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool FileExplorerTool::is_valid_directory(const std::string& path) const {
    return safe_fs_exists(path) && safe_fs_is_directory(path);
}

std::string FileExplorerTool::url_decode(const std::string& str) const {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            std::string hex = str.substr(i + 1, 2);
            char c = static_cast<char>(std::stoi(hex, nullptr, 16));
            result += c;
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string FileExplorerTool::format_file_size(uint64_t bytes) const {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream ss;
    if (unit_index == 0) {
        ss << static_cast<int>(size) << " " << units[unit_index];
    } else {
        ss << std::fixed << std::setprecision(1) << size << " " << units[unit_index];
    }
    return ss.str();
}

std::string FileExplorerTool::get_file_category(const std::string& extension) const {
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Remove leading dot
    if (!ext.empty() && ext[0] == '.') {
        ext = ext.substr(1);
    }

    static const std::unordered_map<std::string, std::string> categories = {
        // Documents
        {"txt", "Text"}, {"md", "Markdown"}, {"rtf", "Rich Text"},
        {"pdf", "PDF"}, {"doc", "Word"}, {"docx", "Word"},
        {"xls", "Excel"}, {"xlsx", "Excel"}, {"csv", "CSV"},
        {"ppt", "PowerPoint"}, {"pptx", "PowerPoint"},
        {"odt", "OpenDocument"}, {"ods", "OpenDocument"},

        // Code
        {"cpp", "C++ Source"}, {"c", "C Source"}, {"h", "C/C++ Header"},
        {"hpp", "C++ Header"}, {"cs", "C# Source"}, {"java", "Java Source"},
        {"py", "Python"}, {"js", "JavaScript"}, {"ts", "TypeScript"},
        {"html", "HTML"}, {"css", "CSS"}, {"json", "JSON"},
        {"xml", "XML"}, {"yaml", "YAML"}, {"yml", "YAML"},
        {"sql", "SQL"}, {"sh", "Shell Script"}, {"bat", "Batch"},
        {"ps1", "PowerShell"}, {"rs", "Rust"}, {"go", "Go"},
        {"rb", "Ruby"}, {"php", "PHP"}, {"swift", "Swift"},

        // Images
        {"jpg", "Image"}, {"jpeg", "Image"}, {"png", "Image"},
        {"gif", "Image"}, {"bmp", "Image"}, {"svg", "SVG Image"},
        {"webp", "Image"}, {"ico", "Icon"}, {"tiff", "Image"},
        {"tif", "Image"}, {"raw", "Raw Image"},

        // Audio
        {"mp3", "Audio"}, {"wav", "Audio"}, {"flac", "Audio"},
        {"ogg", "Audio"}, {"wma", "Audio"}, {"aac", "Audio"},
        {"m4a", "Audio"}, {"opus", "Audio"},

        // Video
        {"mp4", "Video"}, {"avi", "Video"}, {"mkv", "Video"},
        {"mov", "Video"}, {"wmv", "Video"}, {"flv", "Video"},
        {"webm", "Video"}, {"m4v", "Video"},

        // Archives
        {"zip", "Archive"}, {"rar", "Archive"}, {"7z", "Archive"},
        {"tar", "Archive"}, {"gz", "GZip"}, {"bz2", "BZip2"},
        {"xz", "XZ Archive"}, {"iso", "Disk Image"},

        // Executables
        {"exe", "Executable"}, {"msi", "Installer"}, {"dll", "DLL"},
        {"com", "Executable"}, {"app", "Application"},

        // Config
        {"ini", "Config"}, {"cfg", "Config"}, {"conf", "Config"},
        {"env", "Environment"}, {"log", "Log"},
    };

    auto it = categories.find(ext);
    if (it != categories.end()) {
        return it->second;
    }

    if (ext.length() > 0) {
        return ext + " File";
    }
    return "Unknown";
}

bool FileExplorerTool::safe_fs_exists(const std::string& path) const {
    try {
        // Input is UTF-8 (LLM output / our own listings); convert via UTF-16
        // so non-ANSI names resolve correctly. A narrow fs::path would
        // interpret the bytes as ANSI and silently miss the file.
        std::filesystem::path p(pathutil::utf8_to_wide(normalize_path_for_api(path)));
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    } catch (...) {
        return false;
    }
}

bool FileExplorerTool::safe_fs_is_directory(const std::string& path) const {
    try {
        std::filesystem::path p(pathutil::utf8_to_wide(normalize_path_for_api(path)));
        std::error_code ec;
        return std::filesystem::is_directory(p, ec);
    } catch (...) {
        return false;
    }
}

std::string FileExplorerTool::normalize_path_for_api(const std::string& path) const {
    // Handle long paths (> 260 chars) by prefixing with extended-length path marker
    if (path.length() >= MAX_PATH && path.find("\\\\?\\") != 0) {
        std::string normalized = path;
        // Replace forward slashes with backslashes
        std::replace(normalized.begin(), normalized.end(), '/', '\\');
        return "\\\\?\\" + normalized;
    }
    return path;
}

} // namespace Jarvis
