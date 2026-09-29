#pragma once
#include "Tool.h"
#include <string>
#include <vector>

namespace Jarvis {

class AddMediaDirectoryTool : public Tool {
public:
    std::string getName() const override { return "add_media_directory"; }
    std::string getDescription() const override {
        return "Registers a folder on the user's machine as a media library so its files can be "
               "found and played by the music_player tool. Use when the user says things like "
               "'add X as my music directory', 'add X as my movies folder', or 'add X to my media library'. "
               "Build 'path' from exactly what the user said, interpreting it as a Windows path. CRITICAL: a dictated path must include EVERY folder name the user said in 'path' - ending at the drive letter alone is incomplete. "
               "a spoken 'slash' or 'backslash' between names is the folder separator: "
               "'x slash y' (or 'x-slash-y') means X:\\y and 'x slash y slash z' means 'X:\\y\\z'. "
               "Arrow notation 'x -> y -> z' becomes 'x:\\y\\z'. A single name after a drive "
               "letter ('x y') is 'X:\\y'. "
               "Set 'type' to 'music' for audio libraries, 'video' for movie/TV libraries, or leave empty "
               "for general media. Supported: mp3 wav flac aac ogg wma m4a mp4 mkv avi mov wmv flv webm m4v. "
               "Never invent a drive letter or folder the user did not mention.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
