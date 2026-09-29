#pragma once

#include <string>
#include <map>
#include <functional>
#include <memory>
#include <vector>

namespace Jarvis {

// Forward declaration - the registry can hold the RAG knowledge base so
// tools can read/write persistent user knowledge (media libraries, facts).
class RagEngine;

// Tool parameter structure
struct ToolParameter {
    std::string name;
    std::string type;  // "string", "int", "bool", etc.
    std::string description;
    bool required;
    std::string default_value;
};

// Tool execution result
struct ToolResult {
    bool success;
    std::string output;
    std::string error;
};

// Base class for all tools
class Tool {
public:
    virtual ~Tool() = default;
    
    // Get tool metadata
    virtual std::string getName() const = 0;
    virtual std::string getDescription() const = 0;
    virtual std::vector<ToolParameter> getParameters() const = 0;
    
    // Execute the tool
    virtual ToolResult execute(const std::map<std::string, std::string>& params) = 0;
};

// Tool registry to manage all available tools
class ToolRegistry {
public:
    static ToolRegistry& getInstance();
    
    // Register a tool
    void registerTool(const std::string& name, std::unique_ptr<Tool> tool);
    
    // Get a tool by name
    Tool* getTool(const std::string& name);
    
    // Get all registered tool names
    std::vector<std::string> getAllToolNames() const;
    
    // Get tool metadata for all tools (for LLM function calling)
    std::string getToolSchemas() const;
    
    // Attach the RAG knowledge base (set once after AssistantOrchestrator::initialize)
    void setRagEngine(RagEngine* rag) { rag_engine_ = rag; }
    RagEngine* getRagEngine() const { return rag_engine_; }

    // Windows folders the user registered as media libraries via the
    // add_media_directory tool (stored in RAG - never hardcoded).
    std::vector<std::string> getMediaDirectories() const;
    std::vector<std::string> getMediaDirectoriesByType(const std::string& type) const;

    // Execute a tool by name
    ToolResult executeTool(const std::string& name, const std::map<std::string, std::string>& params);
    
private:
    ToolRegistry() = default;
    std::map<std::string, std::unique_ptr<Tool>> tools_;
    RagEngine* rag_engine_ = nullptr;
};

} // namespace Jarvis
