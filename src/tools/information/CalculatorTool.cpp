#define _USE_MATH_DEFINES
#include <cmath>
#include "CalculatorTool.h"
#include <iostream>
#include <sstream>
#include <stack>
#include <cctype>
#include <algorithm>
#include <regex>
#include <map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Jarvis {

std::vector<ToolParameter> CalculatorTool::getParameters() const {
    return {
        {"expression", "string", "The mathematical expression to evaluate (e.g., '2 + 3 * 4')", true, ""}
    };
}

// Helper: apply operator with proper precedence
static double apply_op(double a, double b, char op) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return a / b;
        case '^': return std::pow(a, b);
        default: return 0;
    }
}

// Helper: get operator precedence
static int precedence(char op) {
    if (op == '+' || op == '-') return 1;
    if (op == '*' || op == '/') return 2;
    if (op == '^') return 3;
    return 0;
}

ToolResult CalculatorTool::execute(const std::map<std::string, std::string>& params) {
    auto it = params.find("expression");
    if (it == params.end()) {
        return {false, "", "Missing required parameter: expression"};
    }
    
    std::string expr = it->second;

    // ── Pre-process: handle named functions and constants ──────────────────
    // Lowercase for function matching
    std::string proc = expr;
    
    // Replace named constants
    {
        std::regex pi_re(R"(\bpi\b)", std::regex_constants::icase);
        proc = std::regex_replace(proc, pi_re, "3.14159265358979");
        std::regex e_re(R"(\be\b)");
        proc = std::regex_replace(proc, e_re, "2.71828182845905");
    }

    // Replace named functions: sqrt(x), sin(x), cos(x), tan(x), log(x), log10(x), abs(x), floor(x), ceil(x)
    auto apply_named_fn = [](const std::string& expr_in) -> std::string {
        std::string s = expr_in;
        struct FnInfo { std::string name; std::function<double(double)> fn; };
        std::vector<FnInfo> fns = {
            {"sqrt",  [](double x){ return std::sqrt(x); }},
            {"sin",   [](double x){ return std::sin(x * M_PI / 180.0); }},  // degrees
            {"cos",   [](double x){ return std::cos(x * M_PI / 180.0); }},
            {"tan",   [](double x){ return std::tan(x * M_PI / 180.0); }},
            {"log",   [](double x){ return std::log(x); }},
            {"log10", [](double x){ return std::log10(x); }},
            {"abs",   [](double x){ return std::abs(x); }},
            {"floor", [](double x){ return std::floor(x); }},
            {"ceil",  [](double x){ return std::ceil(x); }},
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& fn_info : fns) {
                size_t pos = 0;
                while ((pos = s.find(fn_info.name + "(", pos)) != std::string::npos) {
                    size_t arg_start = pos + fn_info.name.size() + 1;
                    // Find matching close paren
                    int depth = 1;
                    size_t p = arg_start;
                    while (p < s.size() && depth > 0) {
                        if (s[p] == '(') depth++;
                        else if (s[p] == ')') depth--;
                        if (depth > 0) p++;
                    }
                    if (depth != 0) { pos++; continue; }
                    std::string arg_str = s.substr(arg_start, p - arg_start);
                    try {
                        double arg = std::stod(arg_str);
                        double result = fn_info.fn(arg);
                        std::string result_str = std::to_string(result);
                        s = s.substr(0, pos) + result_str + s.substr(p + 1);
                        changed = true;
                        break;
                    } catch (...) { pos++; }
                }
                if (changed) break;
            }
        }
        return s;
    };

    proc = apply_named_fn(proc);

    // Replace % operator: "x% of y" → x/100*y, or standalone "x%" → x/100
    {
        std::regex pct_of_re(R"((\d+\.?\d*)\s*%\s*of\s*(\d+\.?\d*))");
        std::smatch m;
        while (std::regex_search(proc, m, pct_of_re)) {
            double a = std::stod(m[1].str());
            double b = std::stod(m[2].str());
            double result = (a / 100.0) * b;
            proc = m.prefix().str() + std::to_string(result) + m.suffix().str();
        }
        // Simple % → divide by 100
        std::regex pct_re(R"((\d+\.?\d*)\s*%)");
        while (std::regex_search(proc, m, pct_re)) {
            double a = std::stod(m[1].str());
            proc = m.prefix().str() + std::to_string(a / 100.0) + m.suffix().str();
        }
    }

    try {
        // ── Shunting-yard algorithm ────────────────────────────────────────
        std::stack<double> values;
        std::stack<char> ops;
        
        for (size_t i = 0; i < proc.length(); i++) {
            char c = proc[i];
            
            if (isspace(c)) continue;
            
            if (isdigit(c) || c == '.') {
                std::string num_str;
                while (i < proc.length() && (isdigit(proc[i]) || proc[i] == '.')) {
                    num_str += proc[i++];
                }
                i--;
                values.push(std::stod(num_str));
            } else if (c == '(') {
                ops.push(c);
            } else if (c == ')') {
                while (!ops.empty() && ops.top() != '(') {
                    double b = values.top(); values.pop();
                    double a = values.top(); values.pop();
                    char op = ops.top(); ops.pop();
                    values.push(apply_op(a, b, op));
                }
                if (!ops.empty()) ops.pop();
            } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^') {
                // Handle unary minus
                if (c == '-' && (i == 0 || proc[i-1] == '(' || proc[i-1] == '+' || proc[i-1] == '-' || proc[i-1] == '*' || proc[i-1] == '/')) {
                    // Unary minus: negate the next number
                    i++;
                    while (i < proc.length() && isspace(proc[i])) i++;
                    std::string num_str;
                    while (i < proc.length() && (isdigit(proc[i]) || proc[i] == '.')) {
                        num_str += proc[i++];
                    }
                    i--;
                    if (!num_str.empty()) values.push(-std::stod(num_str));
                    continue;
                }
                while (!ops.empty() && precedence(ops.top()) >= precedence(c)) {
                    double b = values.top(); values.pop();
                    double a = values.top(); values.pop();
                    char op = ops.top(); ops.pop();
                    values.push(apply_op(a, b, op));
                }
                ops.push(c);
            }
        }
        
        while (!ops.empty()) {
            double b = values.top(); values.pop();
            double a = values.top(); values.pop();
            char op = ops.top(); ops.pop();
            values.push(apply_op(a, b, op));
        }

        if (values.empty()) {
            return {false, "", "Could not evaluate expression."};
        }
        
        double result = values.top();
        
        // Check for special values
        if (std::isnan(result)) return {false, "", "Result is not a number (check division or square root of negative)."};
        if (std::isinf(result)) return {false, "", "Result is infinity (division by zero?)."};
        
        // Format result: remove trailing zeros
        std::string result_str = std::to_string(result);
        size_t dot = result_str.find('.');
        if (dot != std::string::npos) {
            size_t last_non_zero = result_str.find_last_not_of("0");
            if (last_non_zero > dot) {
                result_str = result_str.substr(0, last_non_zero + 1);
            } else {
                result_str = result_str.substr(0, dot);
            }
        }
        
        return {true, result_str, ""};
    } catch (const std::exception& e) {
        return {false, "", "Error evaluating expression: " + std::string(e.what())};
    }
}

} // namespace Jarvis