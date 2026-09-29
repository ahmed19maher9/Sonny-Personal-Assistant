#pragma once

#include "Tool.h"

namespace Jarvis {

// ---------------------------------------------------------------------------
// BrowserTool — the LLM's front door to Sonny's Chromium automation
// (BrowserServiceImpl + CDP). One tool, one `action` verb:
//
//   open / new_tab / close_tab / tabs / switch_tab   page lifecycle
//   read / find / summarize / eval                   inspection
//   click / fill / scroll / key / actions            interaction
//   screenshot / pdf / wait / back / forward / reload
//   fields / autofill                                form intelligence
//
// The JSON the service returns is compacted before it reaches the model so a
// page digest never floods the context window.
// ---------------------------------------------------------------------------
class BrowserTool : public Tool {
public:
    std::string getName() const override { return "browser"; }
    std::string getDescription() const override;
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
