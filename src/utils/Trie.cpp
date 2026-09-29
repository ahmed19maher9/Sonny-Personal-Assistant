#include "Trie.h"
#include <algorithm>

namespace Jarvis {

Trie::Trie() : root_(std::make_shared<TrieNode>()) {
    root_->isEndOfWord = false;
}

Trie::~Trie() {
    clear();
}

void Trie::insert(const std::string& word, const std::string& filePath) {
    std::shared_ptr<TrieNode> current = root_;

    for (char c : word) {
        if (current->children.find(c) == current->children.end()) {
            current->children[c] = std::make_shared<TrieNode>();
        }
        current = current->children[c];
        // Add file path to each node along the path for prefix matching
        current->filePaths.push_back(filePath);
    }

    current->isEndOfWord = true;
}

std::vector<std::string> Trie::searchPrefix(const std::string& prefix) const {
    std::shared_ptr<TrieNode> current = root_;

    for (char c : prefix) {
        if (current->children.find(c) == current->children.end()) {
            return {}; // Prefix not found
        }
        current = current->children[c];
    }

    // Collect all file paths from this node and its children
    std::vector<std::string> results;
    collectAllPaths(current, results);
    return results;
}

std::vector<std::string> Trie::fuzzySearch(const std::string& query) const {
    std::vector<std::string> results;

    // For fuzzy search, we need to find all words that contain the query
    // This is a simplified implementation - a more advanced one would use
    // Levenshtein distance or other fuzzy matching algorithms
    std::string queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::tolower);

    // Search through all nodes for matches
    std::shared_ptr<TrieNode> current = root_;
    fuzzySearchHelper(current, "", queryLower, results);

    return results;
}

void Trie::fuzzySearchHelper(std::shared_ptr<TrieNode> node, const std::string& currentWord,
                             const std::string& query, std::vector<std::string>& results) const {

    if (!node) return;

    // Check if current word contains the query
    std::string currentWordLower = currentWord;
    std::transform(currentWordLower.begin(), currentWordLower.end(), currentWordLower.begin(), ::tolower);

    if (currentWordLower.find(query) != std::string::npos && !currentWord.empty()) {
        // Add all file paths at this node
        for (const auto& path : node->filePaths) {
            results.push_back(path);
        }
    }

    // Recursively search children
    for (const auto& [c, child] : node->children) {
        fuzzySearchHelper(child, currentWord + c, query, results);
    }
}

void Trie::collectAllPaths(std::shared_ptr<TrieNode> node, std::vector<std::string>& results) const {
    if (!node) return;

    // Add all file paths at this node
    for (const auto& path : node->filePaths) {
        results.push_back(path);
    }

    // Recursively collect from children
    for (const auto& [c, child] : node->children) {
        collectAllPaths(child, results);
    }
}

void Trie::clear() {
    root_ = std::make_shared<TrieNode>();
    root_->isEndOfWord = false;
}

} // namespace Jarvis
