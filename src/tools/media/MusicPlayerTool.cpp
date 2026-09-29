#include "MusicPlayerTool.h"
#include "Tool.h"
#include "Logger.h"
#include "PathUtil.h"
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <random>
#include <chrono>
#include <unordered_set>
#include <cctype>

#pragma comment(lib, "winmm.lib")

namespace Jarvis {
namespace fs = std::filesystem;

namespace {

const std::unordered_set<std::string>& audio_extensions() {
    static const std::unordered_set<std::string> exts = {
        ".mp3", ".wav", ".flac", ".aac", ".ogg", ".wma", ".m4a"
    };
    return exts;
}

const std::unordered_set<std::string>& video_extensions() {
    static const std::unordered_set<std::string> exts = {
        ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v"
    };
    return exts;
}

void fold_punctuation(std::string& s) {
    const std::pair<const char*, const char*> folds[] = {
        {"\uFF02", "\""}, {"\u201C", "\""}, {"\u201D", "\""},
        {"\uFF1A", ":"}, {"\uFF1B", ";"},
        {"\uFF08", "("}, {"\uFF09", ")"},
        {"\uFF0C", ","}, {"\uFF01", "!"}, {"\uFF1F", "?"},
        {"\uFF0E", "."},
        {"\u2018", "'"}, {"\u2019", "'"}, {"\uFF07", "'"},
        {"\u2013", "-"}, {"\u2014", "-"}, {"\uFF0D", "-"},
        {"\u3000", " "}, {"\u00A0", " "},
    };
    for (const auto& fold : folds) {
        size_t pos = 0;
        while ((pos = s.find(fold.first, pos)) != std::string::npos) {
            s.replace(pos, std::string(fold.first).size(), fold.second);
            pos += std::string(fold.second).size();
        }
    }
}

std::string normalize(const std::string& s) {
    std::string r = s;
    fold_punctuation(r);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });
    size_t start = r.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = r.find_last_not_of(" \t\r\n");
    return r.substr(start, end - start + 1);
}

bool is_stop_word(const std::string& w) {
    static const std::unordered_set<std::string> stops = {
        "the", "a", "an", "to", "of", "and", "for", "on", "in", "with",
        "at", "by", "is", "it", "feat", "ft", "ft.", "featuring"
    };
    return stops.count(w) > 0;
}

bool matches_by_ordered_tokens(const std::string& name, const std::string& search_term) {
    if (name.empty() || search_term.empty()) return false;
    std::istringstream iss(search_term);
    std::string word;
    size_t pos = 0;
    bool any_significant = false;
    while (iss >> word) {
        if (is_stop_word(word)) continue;
        any_significant = true;
        size_t found = name.find(word, pos);
        if (found == std::string::npos) return false;
        pos = found + word.length();
    }
    return any_significant;
}

std::string get_file_ext_lower(const std::wstring& path) {
    std::string ext = pathutil::wide_to_utf8(fs::path(path).extension().wstring());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });
    return ext;
}

} // anonymous namespace

MusicPlayerTool::MusicPlayerTool() {}

MusicPlayerTool::~MusicPlayerTool() {
    if (mci_open_) {
        mciSendStringA("stop music", NULL, 0, NULL);
        mciSendStringA("close music", NULL, 0, NULL);
        mci_open_ = false;
    }
}

std::vector<ToolParameter> MusicPlayerTool::getParameters() const {
    return {
        {"action", "string",
         "Action: 'play' (play song/movie or folder), 'pause', 'resume', 'stop', 'next', 'previous', "
         "'shuffle', 'queue', 'status', 'volume', 'seek', 'list_dirs'. REQUIRED for all calls.", true, "play"},
        {"path", "string",
         "Path to media file or folder (optional). If omitted, automatically searches registered libraries.",
         false, ""},
        {"search_term", "string",
         "Song title, movie name, or artist to search and play. Omit to play a RANDOM song from registered media libraries. "
         "Extract the ACTUAL title from user input. Do NOT use generic words like 'song', 'music', 'track', 'video', 'movie'. "
         "Example: 'play song on my own' -> search_term='on my own'. "
         "Example: 'play the movie titanic' -> search_term='titanic'. "
         "Example: 'play music' or 'play something' -> omit search_term to play random.",
         false, ""},
        {"volume", "string", "Volume level 0-100 (for 'volume' action).", false, ""},
        {"position", "string", "Seek position in seconds (for 'seek' action).", false, ""}
    };
}

bool MusicPlayerTool::is_audio_file(const std::wstring& path) {
    return audio_extensions().count(get_file_ext_lower(path)) > 0;
}

bool MusicPlayerTool::is_video_file(const std::wstring& path) {
    return video_extensions().count(get_file_ext_lower(path)) > 0;
}

bool MusicPlayerTool::is_media_file(const std::wstring& path) {
    std::string ext = get_file_ext_lower(path);
    return audio_extensions().count(ext) > 0 || video_extensions().count(ext) > 0;
}

bool MusicPlayerTool::mci_command(const std::string& cmd, std::string& error_out) {
    MCIERROR err = mciSendStringA(cmd.c_str(), NULL, 0, NULL);
    if (err != 0) {
        char err_str[256] = {};
        mciGetErrorStringA(err, err_str, sizeof(err_str));
        error_out = err_str;
        return false;
    }
    return true;
}

bool MusicPlayerTool::mci_command_w(const std::wstring& cmd, std::string& error_out) {
    MCIERROR err = mciSendStringW(cmd.c_str(), NULL, 0, NULL);
    if (err != 0) {
        wchar_t err_str_w[256] = {};
        mciGetErrorStringW(err, err_str_w, sizeof(err_str_w) / sizeof(wchar_t));
        error_out = pathutil::wide_to_utf8(err_str_w);
        return false;
    }
    return true;
}

bool MusicPlayerTool::set_volume(int volume) {
    if (!mci_open_) return false;
    int mci_volume = std::max(0, std::min(1000, volume * 10));
    std::string cmd = "setaudio music volume to " + std::to_string(mci_volume);
    std::string err;
    return mci_command(cmd, err);
}

long MusicPlayerTool::get_position_ms() const {
    if (!mci_open_) return 0;
    char buf[64] = {};
    mciSendStringA("status music position", buf, sizeof(buf) - 1, NULL);
    return std::atol(buf);
}

long MusicPlayerTool::get_duration_ms() const {
    if (!mci_open_) return 0;
    char buf[64] = {};
    mciSendStringA("status music length", buf, sizeof(buf) - 1, NULL);
    return std::atol(buf);
}

std::vector<std::wstring> MusicPlayerTool::find_media_files_cached(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto now = std::chrono::steady_clock::now();
    auto it = dir_cache_.find(dir);

    if (it != dir_cache_.end() && (now - it->second.timestamp) < CACHE_TTL) {
        return it->second.files;
    }

    std::vector<std::wstring> files;
    try {
        for (const auto& entry : fs::directory_iterator(fs::path(dir))) {
            try {
                if (!entry.is_regular_file()) continue;
                if (is_media_file(entry.path().wstring())) {
                    files.push_back(entry.path().wstring());
                }
            } catch (...) {}
        }
    } catch (...) {}

    std::sort(files.begin(), files.end());

    if (dir_cache_.size() >= MAX_CACHE_ENTRIES && dir_cache_.find(dir) == dir_cache_.end()) {
        auto oldest = dir_cache_.begin();
        for (auto cit = dir_cache_.begin(); cit != dir_cache_.end(); ++cit) {
            if (cit->second.timestamp < oldest->second.timestamp) {
                oldest = cit;
            }
        }
        dir_cache_.erase(oldest);
    }

    DirCacheEntry entry;
    entry.files = files;
    entry.timestamp = now;
    dir_cache_[dir] = std::move(entry);
    return files;
}

std::vector<std::wstring> MusicPlayerTool::find_media_files_recursive(const std::wstring& dir, int max_depth) {
    std::vector<std::wstring> results;
    if (max_depth <= 0) return results;

    try {
        for (const auto& entry : fs::directory_iterator(fs::path(dir), fs::directory_options::skip_permission_denied)) {
            try {
                if (entry.is_regular_file()) {
                    if (is_media_file(entry.path().wstring())) {
                        results.push_back(entry.path().wstring());
                    }
                } else if (entry.is_directory()) {
                    auto sub = find_media_files_recursive(entry.path().wstring(), max_depth - 1);
                    results.insert(results.end(), sub.begin(), sub.end());
                }
            } catch (...) {}
        }
    } catch (...) {}

    return results;
}

int MusicPlayerTool::levenshtein_distance(const std::string& s1, const std::string& s2) {
    size_t m = s1.length();
    size_t n = s2.length();
    if (m > n + 3 || n > m + 3) return 4;

    std::vector<int> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= m; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= n; j++) {
            int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

int MusicPlayerTool::score_match(const std::string& name_no_ext, const std::string& search_lower) const {
    if (search_lower.empty() || name_no_ext.empty()) return -1;
    if (name_no_ext == search_lower) return 1000;
    if (name_no_ext.find(search_lower) == 0) return 800 + (int)search_lower.length();
    if (name_no_ext.find(search_lower) != std::string::npos) return 500 + (int)search_lower.length();

    {
        size_t pos = 0;
        while ((pos = name_no_ext.find(search_lower, pos)) != std::string::npos) {
            bool word_start = (pos == 0) || (name_no_ext[pos - 1] == ' ' || name_no_ext[pos - 1] == '-' || name_no_ext[pos - 1] == '_');
            bool word_end = (pos + search_lower.length() >= name_no_ext.length()) ||
                            (name_no_ext[pos + search_lower.length()] == ' ' ||
                             name_no_ext[pos + search_lower.length()] == '-' ||
                             name_no_ext[pos + search_lower.length()] == '_');
            if (word_start && word_end) return 700 + (int)search_lower.length();
            pos += search_lower.length();
        }
    }

    if (matches_by_ordered_tokens(name_no_ext, search_lower)) return 450 + (int)search_lower.length();

    int lev = levenshtein_distance(name_no_ext, search_lower);
    if (lev <= 2) return 300 - lev * 50;

    return -1;
}

std::wstring MusicPlayerTool::find_media_file_by_name(const std::wstring& dir, const std::string& search_term) {
    std::string search_lower = normalize(search_term);
    if (search_lower.empty()) return L"";

    auto files = find_media_files_cached(dir);
    struct Scored { std::wstring path; int score; };
    std::vector<Scored> candidates;
    candidates.reserve(files.size());

    for (const auto& file : files) {
        std::string filename = pathutil::wide_to_utf8(fs::path(file).filename().wstring());
        std::string filename_lower = normalize(filename);
        size_t dot = filename_lower.rfind('.');
        std::string name_no_ext = (dot != std::string::npos) ? filename_lower.substr(0, dot) : filename_lower;

        int score = score_match(name_no_ext, search_lower);
        if (score < 0 && filename_lower == search_lower) score = 950;
        if (score >= 0) candidates.push_back({file, score});
    }

    if (candidates.empty()) return L"";
    std::sort(candidates.begin(), candidates.end(), [](const Scored& a, const Scored& b) {
        return a.score > b.score;
    });
    return candidates[0].path;
}

ToolResult MusicPlayerTool::play_file(const std::wstring& file_to_play, const std::wstring& parent_dir) {
    if (file_to_play.empty() || !fs::exists(fs::path(file_to_play))) {
        return {false, "", "Media file not found: " + pathutil::wide_to_utf8(file_to_play)};
    }

    std::string filename = pathutil::wide_to_utf8(fs::path(file_to_play).filename().wstring());

    // Video file playback via system player (ShellExecuteW)
    if (is_video_file(file_to_play)) {
        if (mci_open_) {
            mciSendStringA("stop music", NULL, 0, NULL);
            mciSendStringA("close music", NULL, 0, NULL);
            mci_open_ = false;
        }
        HINSTANCE result = ShellExecuteW(NULL, L"open", file_to_play.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((intptr_t)result <= 32) {
            return {false, "", "Failed to launch system player for: " + filename};
        }
        current_file_ = pathutil::wide_to_utf8(file_to_play);
        return {true, "Now playing video: " + filename + " (opened in system player)", ""};
    }

    // Audio file playback via MCI
    if (mci_open_) {
        mciSendStringA("stop music", NULL, 0, NULL);
        mciSendStringA("close music", NULL, 0, NULL);
        mci_open_ = false;
    }

    std::wstring open_cmd = L"open \"" + file_to_play + L"\" alias music";
    std::string err;
    if (!mci_command_w(open_cmd, err)) {
        return {false, "", "Failed to open audio file: " + err};
    }

    mci_open_ = true;
    current_file_ = pathutil::wide_to_utf8(file_to_play);
    if (!parent_dir.empty()) {
        current_dir_ = pathutil::wide_to_utf8(parent_dir);
    } else {
        current_dir_ = pathutil::wide_to_utf8(fs::path(file_to_play).parent_path().wstring());
    }

    if (!mci_command("play music", err)) {
        mciSendStringA("close music", NULL, 0, NULL);
        mci_open_ = false;
        return {false, "", "Failed to play audio: " + err};
    }

    return {true, "Now playing: " + filename, ""};
}

ToolResult MusicPlayerTool::execute(const std::map<std::string, std::string>& params) {
    auto get = [&](const std::string& k) {
        auto it = params.find(k);
        return it != params.end() ? it->second : "";
    };

    std::string action = get("action");
    if (action.empty()) {
        return {false, "", "Missing required parameter: action (play, pause, resume, stop, next, previous, shuffle, queue, status, volume, seek, list_dirs)."};
    }
    std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });

    if (action == "stop") {
        if (mci_open_) {
            mciSendStringA("stop music", NULL, 0, NULL);
            mciSendStringA("close music", NULL, 0, NULL);
            mci_open_ = false;
            current_file_ = "";
        }
        return {true, "Playback stopped.", ""};

    } else if (action == "pause") {
        if (!mci_open_) return {false, "", "No music is currently playing."};
        std::string err;
        if (!mci_command("pause music", err)) return {false, "", "Failed to pause: " + err};
        return {true, "Music paused.", ""};

    } else if (action == "resume") {
        if (!mci_open_) return {false, "", "No music is loaded. Use 'play' first."};
        std::string err;
        if (!mci_command("resume music", err)) return {false, "", "Failed to resume: " + err};
        return {true, "Music resumed.", ""};

    } else if (action == "volume") {
        if (!mci_open_) return {false, "", "No music is playing."};
        std::string vol_str = get("volume");
        if (vol_str.empty()) return {false, "", "Missing required parameter: volume (0-100)."};
        int volume = std::atoi(vol_str.c_str());
        if (volume < 0 || volume > 100) return {false, "", "Volume must be between 0 and 100."};
        if (!set_volume(volume)) return {false, "", "Failed to set volume."};
        return {true, "Volume set to " + std::to_string(volume) + "%.", ""};

    } else if (action == "seek") {
        if (!mci_open_) return {false, "", "No music is playing."};
        std::string pos_str = get("position");
        if (pos_str.empty()) return {false, "", "Missing required parameter: position (seconds)."};
        long seconds = std::atol(pos_str.c_str());
        if (seconds < 0) return {false, "", "Position must be non-negative."};
        long duration = get_duration_ms();
        if (duration > 0 && seconds * 1000 > duration) return {false, "", "Position exceeds track duration."};
        std::string cmd = "seek music to " + std::to_string(seconds * 1000);
        std::string err;
        if (!mci_command(cmd, err)) return {false, "", "Failed to seek: " + err};
        mci_command("play music", err);
        return {true, "Seeked to " + std::to_string(seconds) + " seconds.", ""};

    } else if (action == "status") {
        if (!mci_open_ || current_file_.empty()) {
            std::lock_guard<std::mutex> lock(playlist_mutex_);
            if (!playlist_.empty()) {
                return {true, "No media actively playing. Playlist has " +
                              std::to_string(playlist_.size()) + " tracks queued.", ""};
            }
            return {true, "No music or media is currently playing.", ""};
        }
        char status_buf[128] = {};
        mciSendStringA("status music mode", status_buf, sizeof(status_buf) - 1, NULL);
        std::string status = status_buf;
        std::string filename = pathutil::wide_to_utf8(
            fs::path(pathutil::utf8_to_wide(current_file_)).filename().wstring());

        long pos = get_position_ms();
        long dur = get_duration_ms();
        std::ostringstream ss;
        ss << "Status: " << status << " — " << filename;
        if (dur > 0) {
            long pos_sec = pos / 1000, dur_sec = dur / 1000;
            ss << " (" << pos_sec / 60 << ":" << (pos_sec % 60 < 10 ? "0" : "") << pos_sec % 60
               << " / " << dur_sec / 60 << ":" << (dur_sec % 60 < 10 ? "0" : "") << dur_sec % 60 << ")";
        }
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (!playlist_.empty()) {
            ss << " [Track " << (playlist_index_ + 1) << " of " << playlist_.size() << "]";
        }
        return {true, ss.str(), ""};

    } else if (action == "list_dirs") {
        auto dirs = Jarvis::ToolRegistry::getInstance().getMediaDirectories();
        if (dirs.empty()) return {true, "No media libraries are registered.", ""};
        std::string out = "Registered media libraries (" + std::to_string(dirs.size()) + "):\n";
        for (size_t i = 0; i < dirs.size(); ++i) {
            out += "  " + std::to_string(i + 1) + ". " + dirs[i] + "\n";
        }
        return {true, out, ""};

    } else if (action == "next") {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (playlist_.empty()) {
            std::wstring next_rand = find_random_in_media_libraries();
            if (next_rand.empty()) return {false, "", "Playlist is empty and no media libraries found."};
            return play_file(next_rand);
        }
        playlist_index_ = (playlist_index_ + 1) % playlist_.size();
        return play_file(playlist_[playlist_index_]);

    } else if (action == "previous") {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (playlist_.empty()) return {false, "", "No playlist active."};
        playlist_index_ = (playlist_index_ == 0) ? playlist_.size() - 1 : playlist_index_ - 1;
        return play_file(playlist_[playlist_index_]);

    } else if (action == "shuffle") {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (playlist_.empty()) {
            // Build playlist from all registered libraries
            auto dirs = Jarvis::ToolRegistry::getInstance().getMediaDirectories();
            for (const auto& d : dirs) {
                auto files = find_media_files_cached(pathutil::utf8_to_wide(d));
                playlist_.insert(playlist_.end(), files.begin(), files.end());
            }
        }
        if (playlist_.empty()) return {false, "", "No media files available to shuffle."};
        auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        std::mt19937 rng(static_cast<unsigned int>(seed));
        std::shuffle(playlist_.begin(), playlist_.end(), rng);
        playlist_index_ = 0;
        shuffle_mode_ = true;
        return play_file(playlist_[0]);

    } else if (action == "queue") {
        std::string search_term = get("search_term");
        std::string path_str = get("path");
        std::wstring target;
        if (!path_str.empty()) {
            target = pathutil::utf8_to_wide(path_str);
        } else if (!search_term.empty()) {
            target = find_in_media_libraries(search_term);
        }
        if (target.empty() || !fs::exists(fs::path(target))) {
            return {false, "", "Could not find media item to queue."};
        }
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        playlist_.push_back(target);
        std::string fn = pathutil::wide_to_utf8(fs::path(target).filename().wstring());
        return {true, "Added to queue: " + fn + " (Queue length: " + std::to_string(playlist_.size()) + ")", ""};

    } else if (action == "play") {
        std::string path_str = get("path");
        std::string search_term = get("search_term");
        
        // Require search_term when no explicit path is provided AND user wants specific song
        // Allow empty search_term to play random song from media libraries
        if (path_str.empty() && search_term.empty()) {
            // Check if there are registered media libraries to play from
            auto dirs = Jarvis::ToolRegistry::getInstance().getMediaDirectories();
            if (dirs.empty()) {
                return {false, "", "Missing required parameter: search_term (song/movie title to play). Example: search_term='everything at once'. Also no media libraries are registered."};
            }
            // Will play random song from media libraries below
        }
        
        std::wstring path;
        if (!path_str.empty()) path = pathutil::utf8_to_wide(path_str);

        std::wstring file_to_play;
        std::string path_utf8 = pathutil::wide_to_utf8(path);
        bool lib_resolved = false;

        // 1) Search in registered media libraries first if no valid explicit path
        if (path.empty() || !fs::exists(fs::path(path))) {
            LOG_TOOL("No explicit path provided or path does not exist, searching media libraries");
            std::wstring lib_file = !search_term.empty()
                ? find_in_media_libraries(search_term)
                : find_random_in_media_libraries();
            if (!lib_file.empty()) {
                file_to_play = lib_file;
                path = fs::path(lib_file).parent_path().wstring();
                path_utf8 = pathutil::wide_to_utf8(path);
                lib_resolved = true;
            } else {
                LOG_TOOL("Media library search failed, will fall back to Windows Music folder");
            }
        }

        // 2) Fall back to user's Windows Music folder if still empty
        if (!lib_resolved && (path.empty() || !fs::exists(fs::path(path)))) {
            LOG_TOOL("Falling back to Windows Music folder");
            wchar_t music_path[MAX_PATH];
            if (SHGetFolderPathW(NULL, CSIDL_MYMUSIC, NULL, 0, music_path) == S_OK) {
                path = music_path;
                path_utf8 = pathutil::wide_to_utf8(path);
                LOG_TOOL("Using Windows Music folder: " + path_utf8);
            }
        }

        if (path.empty()) {
            return {false, "", "No path specified and could not find a registered media library."};
        }

        // 3) Resolve within directory if not resolved
        if (!lib_resolved) {
            if (fs::is_directory(fs::path(path))) {
                if (!search_term.empty()) {
                    file_to_play = find_media_file_by_name(path, search_term);
                    if (file_to_play.empty()) {
                        file_to_play = find_best_media_match_recursive(path, search_term);
                    }
                    if (file_to_play.empty()) {
                        file_to_play = find_in_media_libraries(search_term);
                        if (!file_to_play.empty()) {
                            path = fs::path(file_to_play).parent_path().wstring();
                            path_utf8 = pathutil::wide_to_utf8(path);
                        }
                    }
                    if (file_to_play.empty()) {
                        std::string suggestions;
                        auto alts = suggest_alternatives(path, search_term, 3);
                        if (!alts.empty()) {
                            suggestions = " Similar titles: ";
                            for (size_t i = 0; i < alts.size(); ++i) {
                                suggestions += "\"" + alts[i] + "\"";
                                if (i + 1 < alts.size()) suggestions += ", ";
                            }
                        }
                        return {false, "", "Could not find any song or video matching \"" + search_term
                                         + "\" in: " + path_utf8 + "." + suggestions};
                    }
                } else {
                    auto files = find_media_files_cached(path);
                    if (files.empty()) {
                        return {false, "", "No media files found in: " + path_utf8};
                    }
                    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
                    std::mt19937 rng(static_cast<unsigned int>(seed));
                    std::uniform_int_distribution<int> dist(0, (int)files.size() - 1);
                    file_to_play = files[dist(rng)];
                }
            } else {
                file_to_play = path;
            }
        }

        // Set playlist to directory contents for seamless next/prev
        if (fs::exists(fs::path(path)) && fs::is_directory(fs::path(path))) {
            auto dir_files = find_media_files_cached(path);
            if (!dir_files.empty()) {
                std::lock_guard<std::mutex> lock(playlist_mutex_);
                playlist_ = dir_files;
                auto it = std::find(playlist_.begin(), playlist_.end(), file_to_play);
                if (it != playlist_.end()) {
                    playlist_index_ = std::distance(playlist_.begin(), it);
                } else {
                    playlist_.insert(playlist_.begin(), file_to_play);
                    playlist_index_ = 0;
                }
            }
        }

        return play_file(file_to_play, path);
    }

    return {false, "", "Unknown action: " + action + ". Use: play, pause, resume, stop, next, previous, shuffle, queue, status, volume, seek, list_dirs."};
}

std::wstring MusicPlayerTool::find_in_media_libraries(const std::string& search_term) {
    if (search_term.empty()) return L"";
    auto dirs = Jarvis::ToolRegistry::getInstance().getMediaDirectories();
    LOG_TOOL("Searching for \"" + search_term + "\" in " + std::to_string(dirs.size()) + " registered media libraries");
    for (const std::string& dir_utf8 : dirs) {
        LOG_TOOL("  Checking library: " + dir_utf8);
        std::wstring dir = pathutil::utf8_to_wide(dir_utf8);
        if (!fs::exists(fs::path(dir)) || !fs::is_directory(fs::path(dir))) {
            LOG_TOOL("    Library path does not exist or is not a directory");
            continue;
        }
        std::wstring file = find_media_file_by_name(dir, search_term);
        if (file.empty()) file = find_best_media_match_recursive(dir, search_term);
        if (!file.empty()) {
            LOG_TOOL("    Found match: " + pathutil::wide_to_utf8(file));
            return file;
        }
        LOG_TOOL("    No match found in this library");
    }
    LOG_TOOL("No match found in any media library");
    return L"";
}

std::wstring MusicPlayerTool::find_random_in_media_libraries() {
    auto dirs = Jarvis::ToolRegistry::getInstance().getMediaDirectories();
    if (dirs.empty()) return L"";

    std::vector<std::wstring> all_files;
    for (const std::string& dir_utf8 : dirs) {
        std::wstring dir = pathutil::utf8_to_wide(dir_utf8);
        if (!fs::exists(fs::path(dir)) || !fs::is_directory(fs::path(dir))) continue;
        auto files = find_media_files_cached(dir);
        if (files.empty()) files = find_media_files_recursive(dir, 3);
        all_files.insert(all_files.end(), files.begin(), files.end());
    }

    if (all_files.empty()) return L"";

    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 rng(static_cast<unsigned int>(seed));
    std::uniform_int_distribution<size_t> dist(0, all_files.size() - 1);
    return all_files[dist(rng)];
}

std::wstring MusicPlayerTool::find_best_media_match_recursive(const std::wstring& dir, const std::string& search_term) {
    auto recursive_files = find_media_files_recursive(dir, 3);
    if (recursive_files.empty()) return L"";

    std::string search_lower = normalize(search_term);
    std::wstring best;
    int best_score = -1;
    for (const auto& f : recursive_files) {
        std::string name = normalize(pathutil::wide_to_utf8(fs::path(f).filename().wstring()));
        size_t dot = name.rfind('.');
        std::string name_no_ext = (dot != std::string::npos) ? name.substr(0, dot) : name;
        int score = score_match(name_no_ext, search_lower);
        if (score > best_score) {
            best_score = score;
            best = f;
        }
    }
    return best;
}

std::vector<std::string> MusicPlayerTool::suggest_alternatives(const std::wstring& dir,
                                                               const std::string& search_term,
                                                               size_t max_count) {
    std::vector<std::string> out;
    std::string search_lower = normalize(search_term);
    if (search_lower.empty()) return out;

    std::vector<std::string> tokens;
    std::string cur;
    for (char c : search_lower) {
        if (c == ' ' || c == '-' || c == '_' || c == ',' || c == '.') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (tokens.empty()) return out;

    for (const auto& f : find_media_files_cached(dir)) {
        std::string name = pathutil::wide_to_utf8(fs::path(f).filename().wstring());
        std::string norm = normalize(name);
        size_t dot = norm.rfind('.');
        if (dot != std::string::npos) norm = norm.substr(0, dot);

        bool shares = false;
        for (const auto& t : tokens) {
            if (t.length() >= 3 && norm.find(t) != std::string::npos) { shares = true; break; }
        }
        if (shares) {
            out.push_back(name);
            if (out.size() >= max_count) break;
        }
    }
    return out;
}

} // namespace Jarvis
