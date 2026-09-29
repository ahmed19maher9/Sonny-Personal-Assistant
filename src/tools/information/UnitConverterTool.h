#pragma once
#include "Tool.h"

namespace Jarvis {

class UnitConverterTool : public Tool {
public:
    std::string getName() const override { return "unit_converter"; }
    std::string getDescription() const override {
        return "Convert between units. Requires 'value' (number), 'from' (unit), 'to' (unit). "
               "Supports: km/miles/feet/meters/cm/inches, kg/pounds/grams/ounces, "
               "celsius/fahrenheit/kelvin, liters/gallons/ml, mph/kph, bytes/kb/mb/gb.";
    }
    std::vector<ToolParameter> getParameters() const override;
    ToolResult execute(const std::map<std::string, std::string>& params) override;
};

} // namespace Jarvis
