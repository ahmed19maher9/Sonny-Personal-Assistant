#include "SpellingTool.h"
#include <algorithm>
#include <sstream>
#include <map>
#include <cctype>

namespace Jarvis {

std::vector<ToolParameter> SpellingTool::getParameters() const {
    return {
        {"word", "string", "The word to spell out", true, ""},
        {"mode", "string", "Spelling mode: 'letters' (default, e.g. S-O-N-N-Y) or 'nato' (NATO phonetic alphabet)", false, "letters"}
    };
}

static const std::map<char, std::string> nato_alphabet = {
    {'a', "Alpha"},   {'b', "Bravo"},    {'c', "Charlie"}, {'d', "Delta"},
    {'e', "Echo"},    {'f', "Foxtrot"},  {'g', "Golf"},    {'h', "Hotel"},
    {'i', "India"},   {'j', "Juliet"},   {'k', "Kilo"},    {'l', "Lima"},
    {'m', "Mike"},    {'n', "November"}, {'o', "Oscar"},   {'p', "Papa"},
    {'q', "Quebec"},  {'r', "Romeo"},    {'s', "Sierra"},  {'t', "Tango"},
    {'u', "Uniform"}, {'v', "Victor"},   {'w', "Whiskey"}, {'x', "X-ray"},
    {'y', "Yankee"},  {'z', "Zulu"},
    {'0', "Zero"},    {'1', "One"},      {'2', "Two"},      {'3', "Three"},
    {'4', "Four"},    {'5', "Five"},     {'6', "Six"},      {'7', "Seven"},
    {'8', "Eight"},   {'9', "Nine"}
};

ToolResult SpellingTool::execute(const std::map<std::string, std::string>& params) {
    auto word_it = params.find("word");
    if (word_it == params.end() || word_it->second.empty()) {
        return {false, "", "Missing required parameter: word"};
    }

    std::string word = word_it->second;
    // Trim whitespace
    word.erase(0, word.find_first_not_of(" \t\n\r"));
    word.erase(word.find_last_not_of(" \t\n\r") + 1);

    auto mode_it = params.find("mode");
    std::string mode = (mode_it != params.end()) ? mode_it->second : "letters";
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);

    std::string result;

    if (mode == "nato") {
        result = word + " is spelled: ";
        for (size_t i = 0; i < word.size(); ++i) {
            char c = std::tolower(static_cast<unsigned char>(word[i]));
            auto it = nato_alphabet.find(c);
            if (it != nato_alphabet.end()) {
                if (i > 0) result += ", ";
                result += it->second;
            } else {
                if (i > 0) result += ", ";
                result += std::string(1, word[i]);
            }
        }
    } else {
        // Default: letter-by-letter with hyphens
        result = word + " is spelled: ";
        for (size_t i = 0; i < word.size(); ++i) {
            if (i > 0) result += "-";
            result += std::toupper(static_cast<unsigned char>(word[i]));
        }
    }

    return {true, result, ""};
}

} // namespace Jarvis
