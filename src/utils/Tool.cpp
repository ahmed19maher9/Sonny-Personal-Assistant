#include "Tool.h"
#include "RagEngine.h"
#include "Logger.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <map>
#include <algorithm>

namespace Jarvis {

ToolRegistry& ToolRegistry::getInstance() {
    static ToolRegistry instance;
    return instance;
}

void ToolRegistry::registerTool(const std::string& name, std::unique_ptr<Tool> tool) {
    tools_[name] = std::move(tool);
    LOG_DEBUG_COMPONENT("Tool", "Registered: " + name);
}

Tool* ToolRegistry::getTool(const std::string& name) {
    auto it = tools_.find(name);
    if (it != tools_.end()) {
        return it->second.get();
    }
    return nullptr;
}

std::vector<std::string> ToolRegistry::getAllToolNames() const {
    std::vector<std::string> names;
    for (const auto& pair : tools_) {
        names.push_back(pair.first);
    }
    return names;
}

std::string ToolRegistry::getToolSchemas() const {
    std::string schemas = "[";
    bool first = true;
    
    for (const auto& pair : tools_) {
        if (!first) {
            schemas += ",";
        }
        first = false;
        
        const Tool* tool = pair.second.get();
        schemas += "{";
        schemas += "\"name\":\"" + tool->getName() + "\",";
        schemas += "\"description\":\"" + tool->getDescription() + "\",";
        schemas += "\"parameters\":{";
        schemas += "\"type\":\"object\",";
        schemas += "\"properties\":{";
        
        auto params = tool->getParameters();
        bool first_param = true;
        for (const auto& param : params) {
            if (!first_param) {
                schemas += ",";
            }
            first_param = false;
            
            schemas += "\"" + param.name + "\":{";
            schemas += "\"type\":\"" + param.type + "\",";
            schemas += "\"description\":\"" + param.description + "\"";
            if (!param.default_value.empty()) {
                schemas += ",\"default\":\"" + param.default_value + "\"";
            }
            schemas += "}";
        }
        
        schemas += "},";
        schemas += "\"required\":[";
        bool first_required = true;
        for (const auto& param : params) {
            if (param.required) {
                if (!first_required) {
                    schemas += ",";
                }
                first_required = false;
                schemas += "\"" + param.name + "\"";
            }
        }
        schemas += "]";
        schemas += "}}";
    }
    
    schemas += "]";
    return schemas;
}

ToolResult ToolRegistry::executeTool(const std::string& name, const std::map<std::string, std::string>& params) {
    Tool* tool = getTool(name);
    if (!tool) {
        LOG_ERROR("TOOL", "Unknown tool: " + name);
        return {false, "", "Tool not found: " + name};
    }

    // Compact, logger-formatted tool invocation (user + developer info).
    std::ostringstream call;
    call << "-> " << name << "(";
    size_t i = 0;
    for (const auto& p : params) {
        if (i++ > 0) call << ", ";
        call << p.first << "=\"" << p.second << "\"";
    }
    call << ")";
    LOG_TOOL(call.str());

    auto t0 = std::chrono::high_resolution_clock::now();
    ToolResult result = tool->execute(params);
    auto t1 = std::chrono::high_resolution_clock::now();
    long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (result.success) {
        LOG_TOOL("OK " + name + " completed in " + std::to_string(elapsed_ms) + " ms");
        // Log the result for list_media_library specifically so user can see it
        if (name == "list_media_library" && !result.output.empty()) {
            LOG_TOOL(result.output);
        }
    } else {
        LOG_ERROR("TOOL", name + " failed in " + std::to_string(elapsed_ms) + " ms: " + result.error);
    }

    // The full payload stays available to developers at DEBUG level; truncated
    // so tool output (search results, file listings) never floods the console.
    std::string detail = result.success ? result.output : result.error;
    const size_t kMaxOutput = 600;
    if (detail.size() > kMaxOutput) {
        detail = detail.substr(0, kMaxOutput) + "... (+" + std::to_string(detail.size() - kMaxOutput) + " chars)";
    }
    LOG_TOOL_DEBUG(name + " output: " + detail);

    return result;
}

// ---- Media directory helpers -----------------------------------------------
//
// Facts are stored in RAG category "media_directories" in two formats:
//   Legacy:  "Media directory: <path>"
//   Typed:   "Media directory [music]: <path>"
//            "Media directory [video]: <path>"
//            "Media directory [media]: <path>"
// ---------------------------------------------------------------------------

namespace {

// Unescape a JSON string value; pos points right after the opening quote.
// Advances pos past the closing quote and returns the unescaped string.
std::string unescape_json_str(const std::string& json, size_t& pos) {
    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char e = json[pos++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b': case 'f': break;
                default:   out += e;   break;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

struct MediaDirEntry {
    std::string path;
    std::string type; // "music", "video", "media"
};

// Parse all media directory facts from RAG into typed entries.
std::vector<MediaDirEntry> parse_media_dir_facts(RagEngine* rag) {
    std::vector<MediaDirEntry> entries;
    if (!rag) return entries;

    const std::string json = rag->remember("media_directories");
    const std::string kFactKey = "\"fact\":\"";
    const std::string kTypedPrefix  = "Media directory [";
    const std::string kLegacyPrefix = "Media directory: ";

    size_t pos = 0;
    while ((pos = json.find(kFactKey, pos)) != std::string::npos) {
        pos += kFactKey.size();
        std::string fact = unescape_json_str(json, pos);

        MediaDirEntry e;
        if (fact.rfind(kTypedPrefix, 0) == 0) {
            size_t bracket_end = fact.find(']', kTypedPrefix.size());
            if (bracket_end != std::string::npos && bracket_end + 2 < fact.size()) {
                e.type = fact.substr(kTypedPrefix.size(), bracket_end - kTypedPrefix.size());
                e.path = fact.substr(bracket_end + 2); // skip "]: "
                // Trim leading/trailing whitespace
                size_t start = e.path.find_first_not_of(" \t\r\n");
                if (start != std::string::npos) {
                    size_t end = e.path.find_last_not_of(" \t\r\n");
                    e.path = e.path.substr(start, end - start + 1);
                }
            }
        } else if (fact.rfind(kLegacyPrefix, 0) == 0) {
            e.path = fact.substr(kLegacyPrefix.size());
            // Trim leading/trailing whitespace
            size_t start = e.path.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                size_t end = e.path.find_last_not_of(" \t\r\n");
                e.path = e.path.substr(start, end - start + 1);
            }
            e.type = "media";
        }

        if (!e.path.empty()) {
            std::replace(e.path.begin(), e.path.end(), '/', '\\');
            // Collapse multiple consecutive backslashes into single backslash
            size_t write_pos = 0;
            bool prev_was_backslash = false;
            for (size_t read_pos = 0; read_pos < e.path.size(); ++read_pos) {
                if (e.path[read_pos] == '\\') {
                    if (!prev_was_backslash) {
                        e.path[write_pos++] = '\\';
                        prev_was_backslash = true;
                    }
                } else {
                    e.path[write_pos++] = e.path[read_pos];
                    prev_was_backslash = false;
                }
            }
            e.path.resize(write_pos);
            // Remove trailing backslash
            while (e.path.size() > 1 && e.path.back() == '\\') e.path.pop_back();
            // Deduplicate by path (case-insensitive on Windows)
            bool dup = false;
            for (const auto& ex : entries) {
                if (_stricmp(ex.path.c_str(), e.path.c_str()) == 0) { dup = true; break; }
            }
            if (!dup) entries.push_back(e);
        }
    }
    return entries;
}

} // anonymous namespace

std::vector<std::string> ToolRegistry::getMediaDirectories() const {
    auto entries = parse_media_dir_facts(rag_engine_);
    std::vector<std::string> dirs;
    dirs.reserve(entries.size());
    for (const auto& e : entries) dirs.push_back(e.path);
    return dirs;
}

std::vector<std::string> ToolRegistry::getMediaDirectoriesByType(const std::string& type) const {
    auto entries = parse_media_dir_facts(rag_engine_);
    std::vector<std::string> dirs;
    for (const auto& e : entries) {
        // "media" type = all; typed match is case-insensitive
        if (type.empty() ||
            _stricmp(e.type.c_str(), type.c_str()) == 0 ||
            _stricmp(e.type.c_str(), "media") == 0) {
            dirs.push_back(e.path);
        }
    }
    return dirs;
}

} // namespace Jarvis