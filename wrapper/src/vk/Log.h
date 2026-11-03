#pragma once

namespace thanosray::log {

	// Change output file name before any logging occurs.
	void SetLogFileName(const char* fileName);

	// Loader-lock safe mode:
	// - By default, Logf() buffers to memory and mirrors to OutputDebugStringA.
	// - Once it's safe to touch the filesystem (outside the loader lock), call
	//   SetAllowFileIO(true) and FlushBuffered().
	void SetAllowFileIO(bool allow);
	void FlushBuffered();

	// Close the log file (safe to call multiple times).
	void Shutdown();

	// Bounded, crash-friendly log write.
	void Logf(const char* fmt, ...);

} // namespace thanosray::log
