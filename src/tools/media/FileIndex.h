#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include "Trie.h"

namespace Jarvis {

// Metadata for a single file/directory
struct FileMetadata {
    std::string path;
    std::string name;
    bool is_directory;
    uint64_t size;
    std::filesystem::file_time_type modified_time;
    std::string extension;

    FileMetadata() : is_directory(false), size(0) {}
};

// In-memory file system index
class FileIndex {
public:
    FileIndex();
    ~FileIndex();

    // Index management
    void startIndexing(const std::string& rootPath);
    void stopIndexing();
    bool isIndexing() const;
    bool isReady() const;

    // Index persistence
    bool saveIndex(const std::string& filePath);
    bool loadIndex(const std::string& filePath);
    std::string getDefaultIndexPath() const;

    // Query operations (thread-safe reads)
    std::vector<FileMetadata> listDirectory(const std::string& path) const;
    std::vector<FileMetadata> searchFiles(const std::string& query) const;
    std::vector<FileMetadata> fuzzySearch(const std::string& query) const; // Trie-based fuzzy search
    bool fileExists(const std::string& path) const;
    FileMetadata getMetadata(const std::string& path) const;

    // Predictive caching
    void prefetchSubfolders(const std::string& path);

    // Update operations (thread-safe writes)
    void addFile(const FileMetadata& metadata);
    void removeFile(const std::string& path);
    void updateFile(const FileMetadata& metadata);

    // Statistics
    size_t getFileCount() const;
    size_t getDirectoryCount() const;

private:
    // Background indexing thread
    void indexingThreadFunc();
    void scanDirectory(const std::filesystem::path& path);
    void startFilesystemWatcher();
    void stopFilesystemWatcher();

    // Data structures
    std::unordered_map<std::string, FileMetadata> pathIndex_;  // Path -> Metadata
    std::unordered_map<std::string, std::vector<std::string>> directoryIndex_;  // Directory path -> List of child paths
    Trie nameTrie_;  // Trie for fast name-based fuzzy search

    // Thread safety
    mutable std::shared_mutex indexMutex_;
    std::thread indexingThread_;
    std::thread watcherThread_;

    // State
    std::string rootPath_;
    bool indexing_;
    bool ready_;
    bool stopRequested_;

    // Statistics
    size_t fileCount_;
    size_t directoryCount_;
};

} // namespace Jarvis
