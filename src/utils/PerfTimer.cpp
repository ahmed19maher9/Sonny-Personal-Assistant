#include "PerfTimer.h"

// Static member definitions
std::atomic<int> PerfTimer::operation_counter_{0};
bool PerfTimer::debug_mode_ = false;