#pragma once

#include "VkProxyState.h"

namespace thanosray::vk {

// MASM thunks jump through this table by index (0..kProcTableSize-1).
extern "C" __declspec(dllexport) extern void* g_procTable[kProcTableSize];

const char* const* ExportNameTable();

} // namespace thanosray::vk
