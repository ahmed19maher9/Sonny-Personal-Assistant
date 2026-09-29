#pragma once

#include "Tool.h"
#include "FileIndex.h"
#include <string>
#include <map>
#include <chrono>
#include <memory>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <set>
#include <queue>
#include <functional>

namespace Jarvis {

// Cache entry with timestamp for LRU eviction
struct PathCacheEntry {
    std::string resolved_path;
    std::chrono::steady_clock::time_point last_access;
    int hit_count;
};

// Tracked user preference for smart suggestion
struct PathPreference {
    std::string path;
    std::string name;
    int access_count;
    std::chrono::system_clock::time_point last_accessed;
};

class FileExplorerTool : public Tool {
public:
    FileExplorerTool();
    ~FileExplorerTool() override;

    std::string getName() const override { return "file_explorer"; }
    std::string getDescription() const override {
        return "Intelligent file explorer with smart path resolution, fuzzy matching, "
               "and natural language understanding. Can list, open, navigate, and run files. "
               "Supports partial paths, environment variables, and smart suggestions.";
    }

    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
    
    // Get current directory for other tools to use
    std::string getCurrentDirectory() const { return current_directory_; }

private:
    // Core state
    std::string current_directory_;
    std::vector<std::string> navigation_history_;
    std::vector<std::string> forward_history_;
    bool initialized_;
    bool explorer_open_;
    std::unique_ptr<FileIndex> fileIndex_;

    // Intelligent path resolution cache
    mutable std::shared_mutex cache_mutex_;
    std::unordered_map<std::string, PathCacheEntry> resolution_cache_;
    static constexpr size_t MAX_CACHE_SIZE = 256;

    // User preference tracking for smart suggestions
    mutable std::shared_mutex prefs_mutex_;
    std::vector<PathPreference> path_preferences_;
    static constexpr size_t MAX_PREFERENCES = 100;

    // Known system paths for quick resolution
    struct KnownSystemPath {
        std::string name;
        std::string path;
        std::vector<std::string> aliases;
    };
    std::vector<KnownSystemPath> known_system_paths_;
    void initialize_known_paths();

    // Initialization
    bool initialize();

    // Core operations
    std::string list_directory(const std::string& path);
    std::string navigate_to(const std::string& path);
    std::string go_back();
    std::string go_forward();
    std::string run_file(const std::string& filename);

    // ============ INTELLIGENT PATH RESOLUTION ============

    // Main path resolution entry point (top-level)
    std::string resolve_special_path(const std::string& path);

    // Step 1: Expand environment variables (%VAR% -> value)
    std::string expand_environment_variables(const std::string& path);

    // Step 2: Resolve known system paths and aliases
    std::string resolve_known_system_path(const std::string& normalized);

    // Step 3: Handle relative paths and special tokens (., ..)
    std::string resolve_relative_path(const std::string& path);

    // Step 4: Smart fuzzy matching with Levenshtein distance
    struct FuzzyMatchResult {
        std::string path;
        std::string name;
        int levenshtein_distance;
        int substring_score;
        bool is_directory;
        bool is_preferred; // user has accessed this before
    };
    std::string resolve_fuzzy_match(const std::string& search_term, bool prefer_directories = false);
    std::vector<FuzzyMatchResult> get_fuzzy_matches(const std::string& search_term, int max_results = 10);
    int levenshtein_distance(const std::string& s1, const std::string& s2) const;

    // Step 5: Partial path completion (e.g., "doc/something" -> "Documents/something")
    std::string resolve_partial_path(const std::string& partial_path);

    // Step 6: Recursive search with scoring
    struct SearchResult {
        std::string path;
        std::string name;
        int score; // Higher = better match
        bool is_directory;
    };
    std::vector<SearchResult> recursive_search(const std::string& query, int max_depth = 3, int max_results = 10);
    void recursive_search_impl(const std::filesystem::path& dir, const std::string& query_lower,
                               std::vector<SearchResult>& results, int current_depth, int max_depth, int max_results);

    // ============ SMART SUGGESTIONS ============

    // Get smart path suggestions for a partial query
    std::vector<std::string> get_path_suggestions(const std::string& partial);

    // Track user path access for learning preferences
    void track_path_access(const std::string& path);

    // Get most frequently accessed paths
    std::vector<PathPreference> get_top_preferences(int count = 5) const;

    // Get recently accessed paths
    std::vector<std::string> get_recent_paths(int count = 5) const;

    // ============ PERFORMANCE OPTIMIZATIONS ============

    // Cache management
    std::string get_cached_resolution(const std::string& input);
    void set_cached_resolution(const std::string& input, const std::string& resolved);
    void evict_cache_if_needed();

    // Get all available drive letters
    std::vector<std::string> get_available_drives() const;

    // ============ UTILITY ============

    // Normalize for search (lowercase, strip certain chars)
    std::string normalize_for_search(const std::string& str) const;

    // Normalize for display (proper case, clean separators)
    std::string normalize_for_display(const std::string& str) const;

    // Check if a path is a valid directory
    bool is_valid_directory(const std::string& path) const;

    // URL decode
    std::string url_decode(const std::string& str) const;

    // Format file size for display
    std::string format_file_size(uint64_t bytes) const;

    // Get file extension category
    std::string get_file_category(const std::string& extension) const;

    // Safe filesystem exists check (handles long paths)
    bool safe_fs_exists(const std::string& path) const;

    // Safe filesystem is_directory check (handles long paths)
    bool safe_fs_is_directory(const std::string& path) const;

    // Prefix long paths with long path prefix for Windows API
    std::string normalize_path_for_api(const std::string& path) const;
};

} // namespace Jarvis