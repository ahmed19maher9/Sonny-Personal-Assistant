#include "CameraCaptureTool.h"
#include "CameraCapture.h"
#include "Logger.h"
#include "PathUtil.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace Jarvis {

namespace fs = std::filesystem;

namespace {

const char* kComponent = "CameraCaptureTool";

// The user talks, so "take a photo", "snap a picture" and "screenshot" all have
// to land on the same action.
std::string normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(c));
        else out += ' ';
    }
    // Collapse the runs of spaces the substitution above can produce.
    std::string collapsed;
    bool previous_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!previous_space) collapsed += c;
            previous_space = true;
        } else {
            collapsed += c;
            previous_space = false;
        }
    }
    return collapsed;
}

std::string param(const std::map<std::string, std::string>& params, const std::string& key,
                  const std::string& fallback = std::string()) {
    auto it = params.find(key);
    return (it == params.end()) ? fallback : it->second;
}

ToolResult ok(const std::string& output) { return {true, output, ""}; }
ToolResult fail(const std::string& error) { return {false, "", error}; }

// "open the folder where the pictures are" must not depend on the exact
// phrasing, so the folder and file kinds are matched on any keyword present.
std::string classify_kind(const std::string& text) {
    const std::string words = normalize(text);
    if (words.find("video") != std::string::npos || words.find("record") != std::string::npos ||
        words.find("clip") != std::string::npos || words.find("footage") != std::string::npos) {
        return "videos";
    }
    if (words.find("photo") != std::string::npos || words.find("picture") != std::string::npos ||
        words.find("image") != std::string::npos || words.find("snapshot") != std::string::npos) {
        return "photos";
    }
    return "all";
}

std::string action_hint() {
    return "Use action=photo to take a picture, action=start_recording then action=stop_recording "
           "to record video, action=open_folder to show the capture folder in Explorer, "
           "action=open_last to open the newest photo or video, action=status to check the state.";
}

ToolResult without_camera() {
    return fail("The camera is not available right now. " + action_hint());
}

bool open_in_shell(const std::string& utf8_path, const wchar_t* verb, std::string& out_error) {
    const std::wstring wide = pathutil::utf8_to_wide(utf8_path);
    const HINSTANCE result = ShellExecuteW(nullptr, verb, wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        out_error = "Windows refused to open " + utf8_path;
        return false;
    }
    return true;
}

// Newest file with one of EXTENSIONS inside `directory`.
std::string newest_file(const std::string& directory, const std::vector<std::wstring>& extensions) {
    std::error_code code;
    if (!fs::is_directory(pathutil::utf8_to_wide(directory), code)) return std::string();

    fs::path newest;
    std::filesystem::file_time_type newest_time;
    for (const fs::directory_entry& entry : fs::directory_iterator(pathutil::utf8_to_wide(directory), code)) {
        if (!entry.is_regular_file(code)) continue;
        std::wstring extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) continue;
        const std::filesystem::file_time_type modified = entry.last_write_time(code);
        if (newest.empty() || modified > newest_time) {
            newest = entry.path();
            newest_time = modified;
        }
    }
    return newest.empty() ? std::string() : pathutil::wide_to_utf8(newest.wstring());
}

} // namespace

std::string CameraCaptureTool::getDescription() const {
    // Kept deliberately short: this string and the parameter table are injected
    // into the system prompt on every turn, and the prompt was already sitting
    // close to the model's context limit.
    return "Take pictures and record video from the computer's camera. "
           "action=photo saves a still image. "
           "action=start_recording begins a video and action=stop_recording ends it and saves it. "
           "action=open_folder shows the capture folder and action=open_last opens the newest capture "
           "(kind=photos, videos or all). "
           "action=status reports whether a recording is running. "
           "Only one recording runs at a time, so always stop the previous one before starting a new one. "
           "A request to record or film means action=start_recording, not photo.";
}

std::vector<ToolParameter> CameraCaptureTool::getParameters() const {
    return {
        {"action", "string",
         "'photo', 'start_recording', 'stop_recording', 'open_folder', 'open_last' or 'status'. "
         "Any request to record or film means 'start_recording', not 'photo'.",
         true, "photo"},
        {"kind", "string", "For open_folder/open_last: 'photos', 'videos' or 'all'", false, "all"},
        {"format", "string", "For photo: 'png' (lossless) or 'jpg' (smaller file)", false, "png"},
        {"filename", "string", "Optional name for the capture, without extension", false, ""}
    };
}

ToolResult CameraCaptureTool::execute(const std::map<std::string, std::string>& params) {
    const std::string words = normalize(param(params, "action", "photo"));
    LOG_DEBUG_COMPONENT(kComponent, "action: " + words);

    // Only the capture actions need the camera; the folder actions must keep
    // working when no webcam is connected at all.
    const bool needs_camera = (words.find("photo") != std::string::npos ||
                               words.find("picture") != std::string::npos ||
                               words.find("snapshot") != std::string::npos ||
                               words.find("screenshot") != std::string::npos ||
                               words.find("record") != std::string::npos);

    CameraCapture* camera = CameraCapture::active();
    if (needs_camera && !camera) return without_camera();

    const bool wants_stop = (words.find("stop") != std::string::npos ||
                            words.find("end") != std::string::npos ||
                            words.find("finish") != std::string::npos ||
                            words.find("done") != std::string::npos);
    const bool wants_start = (words.find("start") != std::string::npos ||
                             words.find("begin") != std::string::npos ||
                             words.find("record") != std::string::npos);

    if (words.find("status") != std::string::npos) {
        std::string output = "Photos are stored in " + CameraCapture::photos_dir() +
                             " and videos in " + CameraCapture::videos_dir() + ". ";
        if (camera && camera->is_recording()) {
            output += "A recording is running (" +
                      std::to_string((int)camera->recording_seconds()) + "s so far) and will be saved as " +
                      camera->recording_path() + ".";
        } else {
            output += "No recording is running.";
        }
        return ok(output);
    }

    if (words.find("open") != std::string::npos) {
        const std::string kind = classify_kind(param(params, "kind", "all"));
        const bool is_folder = (words.find("folder") != std::string::npos ||
                                words.find("directory") != std::string::npos ||
                                words.find("explorer") != std::string::npos ||
                                words.find("where") != std::string::npos ||
                                kind == "all");

        if (is_folder) {
            const std::string directory = (kind == "photos") ? CameraCapture::photos_dir()
                                         : (kind == "videos") ? CameraCapture::videos_dir()
                                                              : CameraCapture::capture_root_dir();
            std::string error;
            if (!CameraCapture::ensure_directory(directory, error)) return fail(error);
            if (!open_in_shell(directory, L"open", error)) return fail(error);
            return ok("Opened the capture folder: " + directory);
        }

        std::string error;
        std::string file;
        if (kind == "photos") {
            file = newest_file(CameraCapture::photos_dir(), {L".png", L".jpg", L".jpeg"});
        } else if (kind == "videos") {
            file = newest_file(CameraCapture::videos_dir(), {L".avi", L".mp4"});
        } else {
            file = newest_file(CameraCapture::photos_dir(), {L".png", L".jpg", L".jpeg"});
            if (file.empty()) file = newest_file(CameraCapture::videos_dir(), {L".avi", L".mp4"});
        }
        if (file.empty()) {
            return fail("There is nothing captured yet to open. " + action_hint());
        }
        if (!open_in_shell(file, L"open", error)) return fail(error);
        return ok("Opened " + file);
    }

    if (wants_stop) {
        if (!camera) return without_camera();
        std::string path;
        std::string error;
        if (!camera->stop_recording(path, error)) {
            return fail("Could not stop the recording: " + error);
        }
        std::error_code code;
        const uintmax_t bytes = fs::exists(pathutil::utf8_to_wide(path), code)
                                    ? fs::file_size(pathutil::utf8_to_wide(path), code) : 0;
        return ok("Recording stopped and saved as " + path +
                  (bytes > 0 ? " (" + std::to_string(bytes / 1024) + " KB)." : "."));
    }

    if (wants_start) {
        if (!camera) return without_camera();
        std::string error;
        if (!CameraCapture::ensure_directory(CameraCapture::videos_dir(), error)) return fail(error);

        // A dictated name is stripped down to characters that are safe in a
        // file name before it is appended to the video folder.
        std::string clean;
        for (char c : param(params, "filename")) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') clean += c;
            else if (c == ' ' && !clean.empty() && clean.back() != '_') clean += '_';
        }
        std::string path = clean.empty()
                               ? CameraCapture::videos_dir() + "\\Sonny_Video.avi"
                               : CameraCapture::videos_dir() + "\\" + clean + ".avi";
        if (fs::exists(pathutil::utf8_to_wide(path))) {
            // Never clobber an existing recording.
            path = CameraCapture::make_capture_path(CameraCapture::videos_dir(), "Sonny_Video", "avi");
        }

        if (!camera->start_recording(path, error)) {
            return fail("Could not start recording: " + error);
        }
        return ok("Recording started. Say 'stop recording' to save it as " + path);
    }

    // Default: take a picture.
    if (!camera) return without_camera();
    const std::string format = normalize(param(params, "format", "png"));
    const bool jpeg = (format == "jpg" || format == "jpeg");
    std::string path;
    std::string error;
    if (!camera->save_photo(CameraCapture::photos_dir(), jpeg ? "jpg" : "png", path, error)) {
        return fail("Could not take a picture: " + error);
    }
    return ok("Picture saved as " + path);
}

} // namespace Jarvis
