#pragma once
#include "Tool.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace Jarvis {

class MusicPlayerTool : public Tool {
public:
    MusicPlayerTool();
    ~MusicPlayerTool();

    std::string getName() const override { return "music_player"; }
    std::string getDescription() const override {
        return "Plays music, audio, videos, and movies. "
               "Actions: 'play' (play song/movie or folder), 'pause', 'resume', 'stop', 'next', 'previous', "
               "'shuffle', 'queue', 'status', 'volume', 'seek', 'list_dirs'. "
               "For 'play': Extract the ACTUAL song/movie title from user input and set it as search_term. "
               "Do NOT use generic words like 'song', 'music', 'track', 'video', 'movie' as search_term. "
               "Example: 'play song on my own' -> search_term='on my own'. "
               "Example: 'play the movie titanic' -> search_term='titanic'. "
               "CRITICAL: Only pass 'path' if user explicitly names a specific folder/file path. "
               "If user says 'from my local machine' or 'from my library', DO NOT pass 'path' - omit it. "
               "The tool automatically searches registered media libraries when path is omitted. "
               "Audio uses built-in MCI player. Video formats (mp4, mkv, avi, mov, wmv, flv, webm, m4v) use system player.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;

private:
    struct DirCacheEntry {
        std::vector<std::wstring> files;
        std::chrono::steady_clock::time_point timestamp;
    };

    bool mci_open_ = false;
    std::string current_file_;   // UTF-8 display/status
    std::string current_dir_;    // UTF-8 display

    // Playlist state
    std::vector<std::wstring> playlist_;
    size_t playlist_index_ = 0;
    std::mutex playlist_mutex_;
    bool shuffle_mode_ = false;

    // Directory listing cache
    std::unordered_map<std::wstring, DirCacheEntry> dir_cache_;
    std::mutex cache_mutex_;
    static constexpr size_t MAX_CACHE_ENTRIES = 32;
    static constexpr auto CACHE_TTL = std::chrono::seconds(30);

    // Playback helper
    ToolResult play_file(const std::wstring& file_to_play, const std::wstring& parent_dir = L"");

    // Media file discovery
    std::vector<std::wstring> find_media_files_cached(const std::wstring& dir);
    std::wstring find_media_file_by_name(const std::wstring& dir, const std::string& search_term);
    int score_match(const std::string& name_no_ext, const std::string& search_lower) const;
    std::vector<std::wstring> find_media_files_recursive(const std::wstring& dir, int max_depth = 3);
    std::wstring find_in_media_libraries(const std::string& search_term);
    std::wstring find_random_in_media_libraries();
    std::wstring find_best_media_match_recursive(const std::wstring& dir, const std::string& search_term);
    std::vector<std::string> suggest_alternatives(const std::wstring& dir, const std::string& search_term, size_t max_count = 3);

    static int levenshtein_distance(const std::string& s1, const std::string& s2);
    static bool is_media_file(const std::wstring& path);
    static bool is_video_file(const std::wstring& path);
    static bool is_audio_file(const std::wstring& path);

    // MCI helpers
    bool mci_command(const std::string& cmd, std::string& error_out);
    bool mci_command_w(const std::wstring& cmd, std::string& error_out);
    bool set_volume(int volume);
    long get_position_ms() const;
    long get_duration_ms() const;
};

} // namespace Jarvis
