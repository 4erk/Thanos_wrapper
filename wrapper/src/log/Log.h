#pragma once

namespace thanosray::log {

// Change output file name before any logging occurs.
void SetLogFileName(const char* fileName);

// Close the log file (safe to call multiple times).
void Shutdown();

// Bounded, crash-friendly log write.
void Logf(const char* fmt, ...);
bool LogOncef(const char* uniqueKey, const char* fmt, ...);

} // namespace thanosray::log
