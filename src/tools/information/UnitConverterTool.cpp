#include "UnitConverterTool.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <map>
#include <iostream>

namespace Jarvis {

std::vector<ToolParameter> UnitConverterTool::getParameters() const {
    return {
        {"value", "string", "The numeric value to convert", true, ""},
        {"from",  "string", "The source unit (e.g. 'km', 'kg', 'celsius', 'mph')", true, ""},
        {"to",    "string", "The target unit (e.g. 'miles', 'pounds', 'fahrenheit', 'kph')", true, ""}
    };
}

static std::string normalize_unit(const std::string& u) {
    std::string s = u;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    // Remove trailing 's' for plurals (except specific units)
    static const std::map<std::string, std::string> aliases = {
        // Length
        {"kilometer", "km"}, {"kilometers", "km"}, {"kilometre", "km"}, {"km", "km"},
        {"mile", "miles"}, {"miles", "miles"}, {"mi", "miles"},
        {"meter", "m"}, {"meters", "m"}, {"metre", "m"}, {"m", "m"},
        {"centimeter", "cm"}, {"centimeters", "cm"}, {"cm", "cm"},
        {"millimeter", "mm"}, {"millimeters", "mm"}, {"mm", "mm"},
        {"foot", "feet"}, {"feet", "feet"}, {"ft", "feet"},
        {"inch", "inches"}, {"inches", "inches"}, {"in", "inches"}, {"\"", "inches"},
        {"yard", "yards"}, {"yards", "yards"}, {"yd", "yards"},
        // Weight / Mass
        {"kilogram", "kg"}, {"kilograms", "kg"}, {"kilo", "kg"}, {"kilos", "kg"}, {"kg", "kg"},
        {"gram", "g"}, {"grams", "g"}, {"g", "g"},
        {"milligram", "mg"}, {"milligrams", "mg"}, {"mg", "mg"},
        {"pound", "pounds"}, {"pounds", "pounds"}, {"lbs", "pounds"}, {"lb", "pounds"},
        {"ounce", "ounces"}, {"ounces", "ounces"}, {"oz", "ounces"},
        {"ton", "tons"}, {"tons", "tons"}, {"tonne", "tonnes"}, {"tonnes", "tonnes"},
        // Temperature
        {"celsius", "celsius"}, {"centigrade", "celsius"}, {"c", "celsius"},
        {"fahrenheit", "fahrenheit"}, {"f", "fahrenheit"},
        {"kelvin", "kelvin"}, {"k", "kelvin"},
        // Speed
        {"mph", "mph"}, {"miles per hour", "mph"},
        {"kph", "kph"}, {"kmh", "kph"}, {"km/h", "kph"}, {"kilometers per hour", "kph"},
        {"mps", "mps"}, {"m/s", "mps"}, {"meters per second", "mps"},
        {"knot", "knots"}, {"knots", "knots"},
        // Volume
        {"liter", "liters"}, {"liters", "liters"}, {"litre", "liters"}, {"litres", "liters"}, {"l", "liters"}, {"lt", "liters"},
        {"milliliter", "ml"}, {"milliliters", "ml"}, {"ml", "ml"},
        {"gallon", "gallons"}, {"gallons", "gallons"}, {"gal", "gallons"},
        {"cup", "cups"}, {"cups", "cups"},
        {"fluid ounce", "fl_oz"}, {"fluid ounces", "fl_oz"}, {"fl oz", "fl_oz"}, {"fl_oz", "fl_oz"},
        // Digital storage
        {"byte", "bytes"}, {"bytes", "bytes"}, {"b", "bytes"},
        {"kilobyte", "kb"}, {"kilobytes", "kb"}, {"kb", "kb"},
        {"megabyte", "mb"}, {"megabytes", "mb"}, {"mb", "mb"},
        {"gigabyte", "gb"}, {"gigabytes", "gb"}, {"gb", "gb"},
        {"terabyte", "tb"}, {"terabytes", "tb"}, {"tb", "tb"},
    };

    auto it = aliases.find(s);
    if (it != aliases.end()) return it->second;
    return s;
}

ToolResult UnitConverterTool::execute(const std::map<std::string, std::string>& params) {
    auto val_it = params.find("value");
    auto from_it = params.find("from");
    auto to_it = params.find("to");

    if (val_it == params.end() || from_it == params.end() || to_it == params.end()) {
        return {false, "", "Missing parameters. Required: value, from, to."};
    }

    double value = 0.0;
    try { value = std::stod(val_it->second); }
    catch (...) { return {false, "", "Invalid numeric value: " + val_it->second}; }

    std::string from = normalize_unit(from_it->second);
    std::string to   = normalize_unit(to_it->second);

    double result = 0.0;
    bool converted = false;
    std::string unit_label = to;

    // ── LENGTH ──────────────────────────────────────────────
    // Convert everything to meters, then to target
    static const std::map<std::string, double> to_meters = {
        {"km", 1000.0}, {"miles", 1609.344}, {"m", 1.0}, {"cm", 0.01},
        {"mm", 0.001}, {"feet", 0.3048}, {"inches", 0.0254}, {"yards", 0.9144}
    };

    if (to_meters.count(from) && to_meters.count(to)) {
        double in_meters = value * to_meters.at(from);
        result = in_meters / to_meters.at(to);
        converted = true;
    }

    // ── WEIGHT ──────────────────────────────────────────────
    // Convert to grams
    static const std::map<std::string, double> to_grams = {
        {"kg", 1000.0}, {"g", 1.0}, {"mg", 0.001},
        {"pounds", 453.592}, {"ounces", 28.3495}, {"tons", 907185.0}, {"tonnes", 1000000.0}
    };

    if (!converted && to_grams.count(from) && to_grams.count(to)) {
        double in_grams = value * to_grams.at(from);
        result = in_grams / to_grams.at(to);
        converted = true;
    }

    // ── TEMPERATURE ─────────────────────────────────────────
    if (!converted && (from == "celsius" || from == "fahrenheit" || from == "kelvin")) {
        double celsius = value;
        if (from == "fahrenheit") celsius = (value - 32.0) * 5.0 / 9.0;
        else if (from == "kelvin") celsius = value - 273.15;

        if (to == "celsius") result = celsius;
        else if (to == "fahrenheit") result = celsius * 9.0 / 5.0 + 32.0;
        else if (to == "kelvin") result = celsius + 273.15;
        else { return {false, "", "Unknown temperature unit: " + to}; }
        converted = true;
    }

    // ── SPEED ────────────────────────────────────────────────
    // Convert to m/s
    static const std::map<std::string, double> to_mps = {
        {"mph", 0.44704}, {"kph", 0.277778}, {"mps", 1.0}, {"knots", 0.514444}
    };

    if (!converted && to_mps.count(from) && to_mps.count(to)) {
        double in_mps = value * to_mps.at(from);
        result = in_mps / to_mps.at(to);
        converted = true;
    }

    // ── VOLUME ───────────────────────────────────────────────
    // Convert to milliliters
    static const std::map<std::string, double> to_ml = {
        {"liters", 1000.0}, {"ml", 1.0}, {"gallons", 3785.41},
        {"cups", 236.588}, {"fl_oz", 29.5735}
    };

    if (!converted && to_ml.count(from) && to_ml.count(to)) {
        double in_ml = value * to_ml.at(from);
        result = in_ml / to_ml.at(to);
        converted = true;
    }

    // ── DIGITAL STORAGE ─────────────────────────────────────
    static const std::map<std::string, double> to_bytes = {
        {"bytes", 1.0}, {"kb", 1024.0}, {"mb", 1048576.0},
        {"gb", 1073741824.0}, {"tb", 1099511627776.0}
    };

    if (!converted && to_bytes.count(from) && to_bytes.count(to)) {
        double in_bytes = value * to_bytes.at(from);
        result = in_bytes / to_bytes.at(to);
        converted = true;
    }

    if (!converted) {
        return {false, "", "Cannot convert from '" + from_it->second + "' to '" + to_it->second + "'. Unsupported or mismatched unit types."};
    }

    // Format result nicely
    std::ostringstream oss;
    oss << std::fixed;
    // Use up to 4 significant decimal places, remove trailing zeros
    if (std::abs(result) >= 1000.0) oss << std::setprecision(2);
    else if (std::abs(result) >= 1.0) oss << std::setprecision(4);
    else oss << std::setprecision(6);
    oss << result;

    std::string res_str = oss.str();
    // Remove trailing zeros after decimal
    if (res_str.find('.') != std::string::npos) {
        res_str.erase(res_str.find_last_not_of('0') + 1);
        if (res_str.back() == '.') res_str.pop_back();
    }

    std::string answer = std::to_string((long long)value == value ? (long long)value : (double)value);
    // Build clean input string
    std::ostringstream input_oss;
    if (value == (long long)value) input_oss << (long long)value;
    else { input_oss << std::fixed << std::setprecision(4) << value; }
    std::string input_str = input_oss.str();

    return {true, input_str + " " + from_it->second + " = " + res_str + " " + to_it->second, ""};
}

} // namespace Jarvis
