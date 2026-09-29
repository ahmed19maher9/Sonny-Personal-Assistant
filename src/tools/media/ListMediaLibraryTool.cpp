#include "ListMediaLibraryTool.h"
#include "Tool.h"
#include "Logger.h"
#include "PathUtil.h"
#include "MediaUtil.h"
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Jarvis {

namespace {

const std::unordered_set<std::string>& media_exts() {
    static const std::unordered_set<std::string> e = {
        ".mp3",".wav",".flac",".aac",".ogg",".wma",".m4a",
        ".mp4",".mkv",".avi",".mov",".wmv",".flv",".webm",".m4v"
    };
    return e;
}

bool is_media(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(::tolower(c)); });
    return media_exts().count(ext) > 0;
}

size_t count_media(const std::wstring& dir, size_t cap=50000) {
    size_t n=0;
    try {
        for (const auto& e : fs::recursive_directory_iterator(
                fs::path(dir), fs::directory_options::skip_permission_denied)) {
            try { if (e.is_regular_file() && is_media(e.path()) && ++n>=cap) break; } catch(...) {}
        }
    } catch(...) {}
    return n;
}

std::vector<fs::path> collect_media(const std::wstring& dir, size_t max=2000) {
    std::vector<fs::path> v;
    try {
        for (const auto& e : fs::recursive_directory_iterator(
                fs::path(dir), fs::directory_options::skip_permission_denied)) {
            try {
                if (e.is_regular_file() && is_media(e.path())) {
                    v.push_back(e.path());
                    if (v.size()>=max) break;
                }
            } catch(...) {}
        }
    } catch(...) {}
    return v;
}

int score_name(const std::string& name_lower, const std::string& search_lower) {
    if (name_lower==search_lower) return 1000;
    if (name_lower.find(search_lower)==0) return 800;
    if (name_lower.find(search_lower)!=std::string::npos) return 500;
    std::istringstream iss(search_lower);
    std::string word;
    int hits=0, total=0;
    while (iss>>word) {
        if (word.size()<2) continue;
        total++;
        if (name_lower.find(word)!=std::string::npos) hits++;
    }
    if (total>0 && hits==total) return 400;
    if (total>0 && hits>0)     return 200;
    return -1;
}

} // anonymous namespace

std::vector<ToolParameter> ListMediaLibraryTool::getParameters() const {
    return {
        {"action","string",
         "Action: 'libraries' (list all registered paths), 'contents' (list files in a dir), "
         "'search' (fuzzy-search across all libraries).",
         true, "libraries"},
        {"path","string","For 'contents': the library path to list. Empty = all libraries.",false,""},
        {"search_term","string","For 'search': the title, artist, or keyword to find.",false,""},
        {"type","string","Optional filter: 'music', 'video', 'media' (all).",false,""}
    };
}

ToolResult ListMediaLibraryTool::execute(const std::map<std::string, std::string>& params) {
    auto get=[&](const std::string& k){ auto it=params.find(k); return it!=params.end()?it->second:""; };
    std::string action = lower_ascii(get("action")); if(action.empty()) action="libraries";
    std::string path_utf8   = get("path");
    std::string search_term = get("search_term");

    ToolRegistry& registry = ToolRegistry::getInstance();

    // ---- libraries --------------------------------------------------------
    if (action=="libraries") {
        auto dirs = registry.getMediaDirectories();
        if (dirs.empty())
            return {true, "No media libraries registered yet. "
                    "Tell me something like 'add C:\\Music as my music library' to get started.", ""};
        std::ostringstream out;
        out << "Registered media libraries (" << dirs.size() << "):\n";
        for (size_t i=0; i<dirs.size(); ++i) {
            size_t cnt = count_media(pathutil::utf8_to_wide(dirs[i]));
            out << "  " << (i+1) << ". " << dirs[i]
                << " (" << cnt << " file" << (cnt==1?"":"s") << ")\n";
        }
        return {true, out.str(), ""};
    }

    // ---- contents ---------------------------------------------------------
    if (action=="contents") {
        std::vector<std::string> dirs;
        if (!path_utf8.empty()) dirs.push_back(path_utf8);
        else dirs = registry.getMediaDirectories();
        if (dirs.empty())
            return {true,"No media libraries registered. Add one with 'add_media_directory'.",""}; 

        std::ostringstream out;
        size_t total=0;
        for (const auto& dir : dirs) {
            std::wstring wd = pathutil::utf8_to_wide(dir);
            if (!fs::exists(fs::path(wd))) {
                out << "[" << dir << " - folder not found]\n"; continue;
            }
            auto files = collect_media(wd, 200);
            out << "\n" << dir << " (" << files.size() << " files):\n";
            for (const auto& f : files) {
                out << "  - " << pathutil::wide_to_utf8(f.filename().wstring()) << "\n";
                if (++total>=500) { out << "  ... (more files omitted)\n"; break; }
            }
            if (total>=500) break;
        }
        if (out.str().empty()) return {true,"No media files found in the registered libraries.",""}; 
        return {true, out.str(), ""};
    }

    // ---- search -----------------------------------------------------------
    if (action=="search") {
        if (search_term.empty()) return {false,"","Provide a search_term to search for."};
        auto dirs = registry.getMediaDirectories();
        if (dirs.empty())
            return {true,"No media libraries registered. Add one with 'add_media_directory'.",""}; 
        std::string sl = lower_ascii(search_term);
        struct Hit { std::string display, path; int score; };
        std::vector<Hit> hits;
        for (const auto& dir : dirs) {
            auto files = collect_media(pathutil::utf8_to_wide(dir), 5000);
            for (const auto& f : files) {
                std::string fn  = pathutil::wide_to_utf8(f.filename().wstring());
                std::string fnl = lower_ascii(fn);
                size_t dot = fnl.rfind('.');
                std::string stem = (dot!=std::string::npos)?fnl.substr(0,dot):fnl;
                for (char& c:stem) if(c=='_'||c=='-'||c=='.') c=' ';
                int s = score_name(stem, sl);
                if (s<0) s = score_name(fnl, sl);
                if (s>=0) hits.push_back({fn, pathutil::wide_to_utf8(f.wstring()), s});
            }
        }
        if (hits.empty())
            return {true,"No media files matching \""+search_term+"\" found in your libraries.",""}; 
        std::sort(hits.begin(),hits.end(),[](const Hit&a,const Hit&b){return a.score>b.score;});
        constexpr size_t kMax=15;
        std::ostringstream out;
        out << "Found " << hits.size() << " match" << (hits.size()==1?"":"es")
            << " for \"" << search_term << "\"";
        if (hits.size()>kMax) out<<" (top "<<kMax<<" shown)";
        out<<":\n";
        for (size_t i=0; i<std::min(hits.size(),kMax); ++i)
            out<<"  "<<(i+1)<<". "<<hits[i].display<<"\n     "<<hits[i].path<<"\n";
        return {true, out.str(), ""};
    }

    return {false,"","Unknown action '"+action+"'. Use: libraries, contents, search."};
}

} // namespace Jarvis
