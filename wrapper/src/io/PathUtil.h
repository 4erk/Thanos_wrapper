#pragma once

#include <cstddef>

namespace thanosray::io {

// Builds an absolute path to a sibling file in the same directory as this DLL.
// Returns true on success.
bool BuildSiblingPath(wchar_t* outPath, size_t outPathCount, const wchar_t* siblingFileName);

} // namespace thanosray::io
