// sonny_pch.h — Precompiled header for personal_assistant
//
// Included in every translation unit via target_precompile_headers().
// Keep to universally-needed, expensive headers only.  Headers that are only
// used by a handful of files (e.g. json.hpp, llama.h) are intentionally
// excluded to avoid bloating the PCH for TUs that do not need them.
//
// Windows SDK — must come before any other Windows headers
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

// ---- C++ Standard Library --------------------------------------------------
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <array>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <atomic>

// Threading & synchronization
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>

// Streams & IO
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>

// Utilities
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <cassert>
#include <cstdint>
#include <cstring>

// ---- Project utilities (included by almost every TU) ----------------------
#include "Logger.h"
