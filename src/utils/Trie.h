#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace Jarvis {

// Trie node for fast prefix-based searching
struct TrieNode {
    std::unordered_map<char, std::shared_ptr<TrieNode>> children;
    std::vector<std::string> filePaths; // Store file paths that match this prefix
    bool isEndOfWord;
};

// Trie data structure for Spotlight-style fuzzy search
class Trie {
public:
    Trie();
    ~Trie();

    // Insert a word into the trie
    void insert(const std::string& word, const std::string& filePath);

    // Search for words with given prefix
    std::vector<std::string> searchPrefix(const std::string& prefix) const;

    // Fuzzy search - find words that contain the query as a substring
    std::vector<std::string> fuzzySearch(const std::string& query) const;

    // Clear the trie
    void clear();

private:
    std::shared_ptr<TrieNode> root_;

    // Helper for recursive prefix search
    void collectAllPaths(std::shared_ptr<TrieNode> node, std::vector<std::string>& results) const;

    // Helper for fuzzy search
    void fuzzySearchHelper(std::shared_ptr<TrieNode> node, const std::string& currentWord,
                           const std::string& query, std::vector<std::string>& results) const;

};

} // namespace Jarvis
