#pragma once
#include "Tool.h"

namespace Jarvis {

class SpellingTool : public Tool {
public:
    std::string getName() const override { return "spell_word"; }
    std::string getDescription() const override {
        return "Spell out a word letter by letter. Requires 'word' param. Optional 'mode': 'letters' (default, e.g. S-O-N-N-Y) or 'nato' (NATO phonetic alphabet: Sierra Oscar November November Yankee).";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
