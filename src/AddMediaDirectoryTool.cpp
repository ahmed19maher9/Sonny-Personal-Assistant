#include "AddMediaDirectoryTool.h"
#include "Tool.h"
#include "RagEngine.h"
#include "Logger.h"
#include "PathUtil.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Jarvis {

namespace {

std::string trim_copy(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Translate spoken path dictation into canonical path text.
// When the user *speaks* a Windows path, the STT layer transcribes the word
// "slash" (or "backslash") literally, e.g. "e slash x" or "x-slash-y".
// Replace every such spoken separator word with the '/' character so the
// downstream drive-letter/colon normalization can finish the job.
// Handles: "slash", "backslash", "forward slash", "forward-slash",
// hyphen-separated variants like "x-slash-y", and extra spaces.
std::string replace_spoken_separators(std::string in) {
    const std::string low = lower_ascii(in);
    const char* words[] = {"forward slash", "forward-slash", "back slash",
                           "back-slash", "backslash", "slash"};
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        bool matched = false;
        // Skip separators around spoken words so "x - slash - y" works.
        for (const char* w : words) {
            const size_t wlen = strlen(w);
            if (low.compare(i, wlen, w) == 0) {
                // Word must end at a boundary (space, hyphen, or end).
                size_t after = i + wlen;
                if (after >= in.size() || in[after] == ' ' || in[after] == '-') {
                    // Swallow optional separator chars before the word.
                    while (!out.empty() && (out.back() == ' ' || out.back() == '-'))
                        out.pop_back();
                    out += '/';
                    // Skip trailing separator chars after the word.
                    while (after < in.size() && (in[after] == ' ' || in[after] == '-'))
                        after++;
                    i = after;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += in[i];
            i++;
        }
    }
    return out;
}

// Translate user-described paths to canonical Windows paths.
// Accepts: arrow notation, forward slashes, missing drive colons, stray quotes.
std::string normalize_directory(std::string in) {
    in = trim_copy(in);
    if (in.size() >= 2 && ((in.front() == '"' && in.back() == '"') ||
                            (in.front() == '\'' && in.back() == '\''))) {
        in = in.substr(1, in.size() - 2);
        in = trim_copy(in);
    }
    // Spoken dictation: "x slash y" / "x-slash-y" -> "x/y".
    in = replace_spoken_separators(in);
    const char* arrows[] = {" -> ", " ->", "-> ", " - > ", " => ", "=>", " > "};
    for (const char* a : arrows) {
        size_t pos;
        while ((pos = in.find(a)) != std::string::npos)
            in.replace(pos, strlen(a), "\\");
    }
    std::replace(in.begin(), in.end(), '/', '\\');
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && !out.empty() && out.back() == '\\') continue;
        out += in[i];
    }
    in = out;
    if (in.size() >= 2 && std::isalpha(static_cast<unsigned char>(in[0])) && in[1] == '\\')
        in.insert(1, 1, ':');
    while (!in.empty() && in.back() == '\\') in.pop_back();
    return in;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(::tolower(c)); });
    return s;
}

const std::unordered_set<std::string>& all_media_extensions() {
    static const std::unordered_set<std::string> exts = {
        ".mp3",".wav",".flac",".aac",".ogg",".wma",".m4a",
        ".mp4",".mkv",".avi",".mov",".wmv",".flv",".webm",".m4v"
    };
    return exts;
}

bool is_media_file(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(::tolower(c)); });
    return all_media_extensions().count(ext) > 0;
}

// Index media filenames into RAG for semantic search (capped at 5000 files).
// Each file stored as a lightweight user fact in category "media_index":
//   "MEDIA:<type>:<full_path>:<display_stem>"
int index_media_files(RagEngine* rag, const std::string& dir_utf8, const std::string& dir_type) {
    if (!rag) return 0;
    const fs::path dir(pathutil::utf8_to_wide(dir_utf8));
    if (!fs::exists(dir) || !fs::is_directory(dir)) return 0;
    int count = 0;
    constexpr int kMaxFiles = 5000;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(
                dir, fs::directory_options::skip_permission_denied)) {
            if (count >= kMaxFiles) break;
            try {
                if (!entry.is_regular_file()) continue;
                if (!is_media_file(entry.path())) continue;
                std::string filename = pathutil::wide_to_utf8(entry.path().filename().wstring());
                std::string filepath = pathutil::wide_to_utf8(entry.path().wstring());
                std::string stem = filename;
                size_t dot = stem.rfind('.');
                if (dot != std::string::npos) stem = stem.substr(0, dot);
                for (char& c : stem) if (c=='_'||c=='.'||c=='-') c=' ';
                std::string fact = "MEDIA:" + dir_type + ":" + filepath + ":" + stem;
                rag->teach(fact, "media_index");
                count++;
            } catch (...) { continue; }
        }
    } catch (...) {}
    return count;
}

} // anonymous namespace

std::vector<ToolParameter> AddMediaDirectoryTool::getParameters() const {
    return {
        {"path", "string",
         "Full Windows path of the folder to register. Arrow notation 'x -> y -> z' becomes 'x:\\y\\z'.",
         true, ""},
        {"type", "string",
         "Media type: 'music' for y/audio, 'video' for movies/TV, 'media' for mixed (default).",
         false, "media"}
    };
}

ToolResult AddMediaDirectoryTool::execute(const std::map<std::string, std::string>& params) {
    std::string raw;
    for (const char* key : {"path", "directory", "folder"}) {
        auto it = params.find(key);
        if (it != params.end() && !it->second.empty()) { raw = it->second; break; }
    }

    // Resolve media type
    std::string media_type = "media";
    auto type_it = params.find("type");
    if (type_it != params.end() && !type_it->second.empty()) {
        media_type = lower_ascii(trim_copy(type_it->second));
        if (media_type=="movie"||media_type=="movies"||media_type=="film"||
            media_type=="films"||media_type=="tv"||media_type=="shows") media_type = "video";
        if (media_type=="audio"||media_type=="y"||media_type=="song") media_type = "music";
        if (media_type!="music"&&media_type!="video") media_type = "media";
    }

    const std::string path = normalize_directory(raw);
    if (path.empty())
        return {false, "", "No media directory provided. Ask the user which folder to add."};

    LOG_TOOL("add_media_directory: validating \"" + path + "\" (type=" + media_type + ")");

    std::error_code ec;
    const fs::path wpath = fs::path(pathutil::utf8_to_wide(path));
    if (!fs::exists(wpath, ec) || !fs::is_directory(wpath, ec)) {
        LOG_WARN("TOOL", "Media directory does not exist: " + path);
        return {false, "", "The folder \"" + path + "\" does not exist. Please check the path."};
    }

    ToolRegistry& registry = ToolRegistry::getInstance();
    RagEngine* rag = registry.getRagEngine();
    if (!rag)
        return {false, "", "The knowledge base is not available."};

    // Idempotent: don't register the same folder twice
    const std::string path_lower = lower_ascii(path);
    for (const std::string& existing : registry.getMediaDirectories()) {
        if (lower_ascii(existing) == path_lower)
            return {true, "\"" + path + "\" is already in your media library.", ""};
    }

    // Store typed fact: "Media directory [type]: path"
    const std::string fact = "Media directory [" + media_type + "]: " + path;
    if (!rag->teach(fact, "media_directories"))
        return {false, "", "Failed to save the media directory to the knowledge base."};

    // Index media filenames for semantic search
    int indexed = index_media_files(rag, path, media_type);

    LOG_SUCCESS("TOOL", "Media library registered: " + path +
                " (" + media_type + ", " + std::to_string(indexed) + " files indexed)");

    std::string type_label = (media_type=="music") ? "music" :
                             (media_type=="video") ? "movie/video" : "media";
    std::string response = "Done! I've registered \"" + path + "\" as your " + type_label + " library";
    if (indexed > 0) {
        response += " and indexed " + std::to_string(indexed) + " file" + (indexed==1?"":"s");
        if (indexed >= 5000) response += " (capped at 5,000)";
    }
    response += ". Just ask me to play anything from it.";
    return {true, response, ""};
}

} // namespace Jarvis
