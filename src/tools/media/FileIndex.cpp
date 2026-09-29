#include "FileIndex.h"
#include <iostream>
#include <algorithm>
#include <windows.h>
#include <fstream>
#include <cstring>
#include "Logger.h"

namespace Jarvis {

FileIndex::FileIndex()
    : indexing_(false)
    , ready_(false)
    , stopRequested_(false)
    , fileCount_(0)
    , directoryCount_(0) {
}

FileIndex::~FileIndex() {
    stopIndexing();
}

void FileIndex::startIndexing(const std::string& rootPath) {
    if (indexing_) {
        return;
    }

    rootPath_ = rootPath;
    indexing_ = true;
    ready_ = false;
    stopRequested_ = false;
    fileCount_ = 0;
    directoryCount_ = 0;

    LOG_FILEINDEX("Starting indexing of: " + rootPath);

    // Start background indexing thread
    indexingThread_ = std::thread(&FileIndex::indexingThreadFunc, this);

    // Start filesystem watcher
    startFilesystemWatcher();
}

void FileIndex::stopIndexing() {
    if (!indexing_) {
        return;
    }

    stopRequested_ = true;

    if (indexingThread_.joinable()) {
        indexingThread_.join();
    }

    stopFilesystemWatcher();

    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }

    indexing_ = false;
    LOG_FILEINDEX("Indexing stopped");
}

bool FileIndex::isIndexing() const {
    return indexing_;
}

bool FileIndex::isReady() const {
    return ready_;
}

void FileIndex::indexingThreadFunc() {
    LOG_FILEINDEX("Indexing thread started");

    try {
        scanDirectory(rootPath_);
        ready_ = true;
        LOG_FILEINDEX("Indexing complete. Files: " + std::to_string(fileCount_) + ", Directories: " + std::to_string(directoryCount_));
    } catch (const std::exception& e) {
        LOG_FILEINDEX("Indexing error: " + std::string(e.what()));
    }
}

void FileIndex::scanDirectory(const std::filesystem::path& path) {
    if (stopRequested_) {
        return;
    }

    std::string pathStr = path.string();

    try {
        for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied)) {
            if (stopRequested_) {
                break;
            }

            try {
                std::string entryPath = entry.path().string();
                std::string entryName = entry.path().filename().string();

                FileMetadata metadata;
                metadata.path = entryPath;
                metadata.name = entryName;
                metadata.is_directory = entry.is_directory();
                metadata.size = metadata.is_directory ? 0 : entry.file_size();
                metadata.modified_time = entry.last_write_time();
                metadata.extension = entry.path().extension().string();

                // Add to index
                {
                    std::unique_lock<std::shared_mutex> lock(indexMutex_);
                    pathIndex_[entryPath] = metadata;

                    // Add to directory index
                    std::string parentPath = path.parent_path().string();
                    if (parentPath.empty()) {
                        parentPath = pathStr;
                    }
                    directoryIndex_[parentPath].push_back(entryPath);

                    // Add to trie for fuzzy search (lowercase)
                    std::string nameLower = entryName;
                    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                    nameTrie_.insert(nameLower, entryPath);

                    if (metadata.is_directory) {
                        directoryCount_++;
                        // Recursively scan subdirectories
                        scanDirectory(entry.path());
                    } else {
                        fileCount_++;
                    }
                }
            } catch (const std::filesystem::filesystem_error&) {
                // Skip files we can't access
                continue;
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LOG_FILEINDEX("Error scanning directory " + pathStr + ": " + std::string(e.what()));
    }
}

void FileIndex::startFilesystemWatcher() {
    watcherThread_ = std::thread([this]() {
        LOG_FILEINDEX("Filesystem watcher thread started");

        HANDLE hDir = CreateFileA(
            rootPath_.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            NULL
        );

        if (hDir == INVALID_HANDLE_VALUE) {
            LOG_FILEINDEX("Failed to open directory for watching: " + std::to_string(GetLastError()));
            return;
        }

        const DWORD BUFFER_SIZE = 1024 * 64; // 64KB buffer
        BYTE buffer[BUFFER_SIZE];
        DWORD bytesReturned;
        OVERLAPPED overlapped;
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        while (!stopRequested_) {
            BOOL success = ReadDirectoryChangesW(
                hDir,
                buffer,
                BUFFER_SIZE,
                TRUE, // Watch subdirectories
                FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytesReturned,
                &overlapped,
                NULL
            );

            if (!success) {
                DWORD error = GetLastError();
                if (error == ERROR_NOTIFY_ENUM_DIR) {
                    // Need to re-enumerate
                    continue;
                }
                LOG_FILEINDEX("ReadDirectoryChangesW failed: " + std::to_string(error));
                break;
            }

            // Wait for notification
            WaitForSingleObject(overlapped.hEvent, 1000);
            ResetEvent(overlapped.hEvent);

            if (GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
                FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

                while (true) {
                    std::wstring filename(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                    std::string filenameStr(filename.begin(), filename.end());
                    std::string fullPath = rootPath_ + "\\" + filenameStr;

                    switch (fni->Action) {
                        case FILE_ACTION_ADDED:
                        case FILE_ACTION_RENAMED_NEW_NAME: {
                            FileMetadata metadata;
                            metadata.path = fullPath;
                            metadata.name = filenameStr;
                            metadata.is_directory = std::filesystem::is_directory(fullPath);
                            if (!metadata.is_directory) {
                                metadata.size = std::filesystem::file_size(fullPath);
                            }
                            metadata.modified_time = std::filesystem::last_write_time(fullPath);
                            metadata.extension = std::filesystem::path(fullPath).extension().string();
                            addFile(metadata);
                            LOG_FILEINDEX("File added: " + fullPath);
                            break;
                        }
                        case FILE_ACTION_REMOVED:
                        case FILE_ACTION_RENAMED_OLD_NAME:
                            removeFile(fullPath);
                            LOG_FILEINDEX("File removed: " + fullPath);
                            break;
                        case FILE_ACTION_MODIFIED: {
                            FileMetadata metadata;
                            metadata.path = fullPath;
                            metadata.name = filenameStr;
                            metadata.is_directory = std::filesystem::is_directory(fullPath);
                            if (!metadata.is_directory) {
                                metadata.size = std::filesystem::file_size(fullPath);
                            }
                            metadata.modified_time = std::filesystem::last_write_time(fullPath);
                            metadata.extension = std::filesystem::path(fullPath).extension().string();
                            updateFile(metadata);
                            LOG_FILEINDEX("File modified: " + fullPath);
                            break;
                        }
                    }

                    if (fni->NextEntryOffset == 0) {
                        break;
                    }
                    fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset);
                }
            }
        }

        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        LOG_FILEINDEX("Filesystem watcher thread stopped");
    });
}

void FileIndex::stopFilesystemWatcher() {
    stopRequested_ = true;
    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }
}

std::vector<FileMetadata> FileIndex::listDirectory(const std::string& path) const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);

    auto it = directoryIndex_.find(path);
    if (it == directoryIndex_.end()) {
        return {};
    }

    std::vector<FileMetadata> result;
    for (const auto& childPath : it->second) {
        auto metaIt = pathIndex_.find(childPath);
        if (metaIt != pathIndex_.end()) {
            result.push_back(metaIt->second);
        }
    }

    return result;
}

std::vector<FileMetadata> FileIndex::searchFiles(const std::string& query) const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);

    std::vector<FileMetadata> result;
    std::string queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::tolower);

    for (const auto& [path, metadata] : pathIndex_) {
        std::string nameLower = metadata.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

        if (nameLower.find(queryLower) != std::string::npos) {
            result.push_back(metadata);
        }
    }

    return result;
}

std::vector<FileMetadata> FileIndex::fuzzySearch(const std::string& query) const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);

    // Convert query to lowercase for case-insensitive search
    std::string queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::tolower);

    // Use trie for fuzzy search
    std::vector<std::string> matchingPaths = nameTrie_.fuzzySearch(queryLower);

    // Convert paths to metadata
    std::vector<FileMetadata> results;
    for (const auto& path : matchingPaths) {
        auto it = pathIndex_.find(path);
        if (it != pathIndex_.end()) {
            results.push_back(it->second);
        }
    }

    return results;
}

bool FileIndex::fileExists(const std::string& path) const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);
    return pathIndex_.find(path) != pathIndex_.end();
}

FileMetadata FileIndex::getMetadata(const std::string& path) const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);

    auto it = pathIndex_.find(path);
    if (it != pathIndex_.end()) {
        return it->second;
    }

    return FileMetadata();
}

void FileIndex::prefetchSubfolders(const std::string& path) {
    // Launch async task to prefetch subfolder contents
    std::thread([this, path]() {
        auto files = listDirectory(path);

        for (const auto& metadata : files) {
            if (metadata.is_directory && !stopRequested_) {
                // Pre-scan this subdirectory to populate the index
                scanDirectory(std::filesystem::path(metadata.path));
            }
        }
    }).detach();
}

void FileIndex::addFile(const FileMetadata& metadata) {
    std::unique_lock<std::shared_mutex> lock(indexMutex_);

    pathIndex_[metadata.path] = metadata;

    std::string parentPath = std::filesystem::path(metadata.path).parent_path().string();
    directoryIndex_[parentPath].push_back(metadata.path);

    if (metadata.is_directory) {
        directoryCount_++;
    } else {
        fileCount_++;
    }
}

void FileIndex::removeFile(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(indexMutex_);

    auto it = pathIndex_.find(path);
    if (it != pathIndex_.end()) {
        bool isDir = it->second.is_directory;
        pathIndex_.erase(it);

        std::string parentPath = std::filesystem::path(path).parent_path().string();
        auto dirIt = directoryIndex_.find(parentPath);
        if (dirIt != directoryIndex_.end()) {
            dirIt->second.erase(
                std::remove(dirIt->second.begin(), dirIt->second.end(), path),
                dirIt->second.end()
            );
        }

        if (isDir) {
            directoryCount_--;
        } else {
            fileCount_--;
        }
    }
}

void FileIndex::updateFile(const FileMetadata& metadata) {
    std::unique_lock<std::shared_mutex> lock(indexMutex_);
    pathIndex_[metadata.path] = metadata;
}

size_t FileIndex::getFileCount() const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);
    return fileCount_;
}

size_t FileIndex::getDirectoryCount() const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);
    return directoryCount_;
}

std::string FileIndex::getDefaultIndexPath() const {
    char modulePath[MAX_PATH];
    GetModuleFileNameA(NULL, modulePath, MAX_PATH);
    std::filesystem::path exePath(modulePath);
    std::filesystem::path cachePath = exePath.parent_path() / "file_index.dat";
    return cachePath.string();
}

bool FileIndex::saveIndex(const std::string& filePath) {
    std::ofstream outFile(filePath, std::ios::binary);
    if (!outFile.is_open()) {
        LOG_FILEINDEX("Failed to open file for saving: " + filePath);
        return false;
    }

    LOG_FILEINDEX("Saving index to: " + filePath);

    // Write header
    const char* magic = "FXDL";
    uint32_t version = 1;
    outFile.write(magic, 4);
    outFile.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Write root path
    uint32_t rootPathLen = static_cast<uint32_t>(rootPath_.length());
    outFile.write(reinterpret_cast<const char*>(&rootPathLen), sizeof(rootPathLen));
    outFile.write(rootPath_.c_str(), rootPathLen);

    // Write file count
    std::shared_lock<std::shared_mutex> lock(indexMutex_);
    uint64_t fileCount = pathIndex_.size();
    outFile.write(reinterpret_cast<const char*>(&fileCount), sizeof(fileCount));

    // Write each file entry
    for (const auto& [path, metadata] : pathIndex_) {
        // Write path
        uint32_t pathLen = static_cast<uint32_t>(path.length());
        outFile.write(reinterpret_cast<const char*>(&pathLen), sizeof(pathLen));
        outFile.write(path.c_str(), pathLen);

        // Write name
        uint32_t nameLen = static_cast<uint32_t>(metadata.name.length());
        outFile.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        outFile.write(metadata.name.c_str(), nameLen);

        // Write is_directory
        outFile.write(reinterpret_cast<const char*>(&metadata.is_directory), sizeof(metadata.is_directory));

        // Write size
        outFile.write(reinterpret_cast<const char*>(&metadata.size), sizeof(metadata.size));

        // Write extension
        uint32_t extLen = static_cast<uint32_t>(metadata.extension.length());
        outFile.write(reinterpret_cast<const char*>(&extLen), sizeof(extLen));
        outFile.write(metadata.extension.c_str(), extLen);

        // Write modified time (skip for simplicity - not critical for functionality)
        // We'll reconstruct it from filesystem on load if needed
        int64_t dummyTime = 0;
        outFile.write(reinterpret_cast<const char*>(&dummyTime), sizeof(dummyTime));
    }

    outFile.close();
    LOG_FILEINDEX("Index saved successfully. Files: " + std::to_string(fileCount));
    return true;
}

bool FileIndex::loadIndex(const std::string& filePath) {
    std::ifstream inFile(filePath, std::ios::binary);
    if (!inFile.is_open()) {
        LOG_FILEINDEX("No saved index found at: " + filePath);
        return false;
    }

    LOG_FILEINDEX("Loading index from: " + filePath);

    // Read and verify header
    char magic[4];
    inFile.read(magic, 4);
    if (std::strncmp(magic, "FXDL", 4) != 0) {
        LOG_FILEINDEX("Invalid index file format");
        return false;
    }

    uint32_t version;
    inFile.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) {
        LOG_FILEINDEX("Unsupported index version: " + std::to_string(version));
        return false;
    }

    // Read root path
    uint32_t rootPathLen;
    inFile.read(reinterpret_cast<char*>(&rootPathLen), sizeof(rootPathLen));
    std::string savedRootPath(rootPathLen, '\0');
    inFile.read(&savedRootPath[0], rootPathLen);

    // Verify root path matches current
    if (savedRootPath != rootPath_) {
        LOG_FILEINDEX("Saved index root path mismatch. Saved: " + savedRootPath + ", Current: " + rootPath_);
        LOG_FILEINDEX("Skipping saved index, will re-index");
        return false;
    }

    // Clear existing index
    std::unique_lock<std::shared_mutex> lock(indexMutex_);
    pathIndex_.clear();
    directoryIndex_.clear();
    nameTrie_.clear();
    fileCount_ = 0;
    directoryCount_ = 0;

    // Read file count
    uint64_t fileCount;
    inFile.read(reinterpret_cast<char*>(&fileCount), sizeof(fileCount));

    // Read each file entry
    for (uint64_t i = 0; i < fileCount; i++) {
        FileMetadata metadata;

        // Read path
        uint32_t pathLen;
        inFile.read(reinterpret_cast<char*>(&pathLen), sizeof(pathLen));
        metadata.path.resize(pathLen);
        inFile.read(&metadata.path[0], pathLen);

        // Read name
        uint32_t nameLen;
        inFile.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        metadata.name.resize(nameLen);
        inFile.read(&metadata.name[0], nameLen);

        // Read is_directory
        inFile.read(reinterpret_cast<char*>(&metadata.is_directory), sizeof(metadata.is_directory));

        // Read size
        inFile.read(reinterpret_cast<char*>(&metadata.size), sizeof(metadata.size));

        // Read extension
        uint32_t extLen;
        inFile.read(reinterpret_cast<char*>(&extLen), sizeof(extLen));
        metadata.extension.resize(extLen);
        inFile.read(&metadata.extension[0], extLen);

        // Read modified time (skip for simplicity - not critical for functionality)
        int64_t dummyTime;
        inFile.read(reinterpret_cast<char*>(&dummyTime), sizeof(dummyTime));
        // Set to current time as placeholder
        metadata.modified_time = std::filesystem::file_time_type::clock::now();

        // Add to index
        pathIndex_[metadata.path] = metadata;

        // Add to directory index
        std::string parentPath = std::filesystem::path(metadata.path).parent_path().string();
        if (parentPath.empty()) {
            parentPath = rootPath_;
        }
        directoryIndex_[parentPath].push_back(metadata.path);

        // Add to trie for fuzzy search (lowercase)
        std::string nameLower = metadata.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        nameTrie_.insert(nameLower, metadata.path);

        if (metadata.is_directory) {
            directoryCount_++;
        } else {
            fileCount_++;
        }
    }

    inFile.close();
    ready_ = true;
    LOG_FILEINDEX("Index loaded successfully. Files: " + std::to_string(fileCount_) + ", Directories: " + std::to_string(directoryCount_));
    return true;
}

} // namespace Jarvis
