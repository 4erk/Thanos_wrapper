//#include "pch.h"
#include "VkProxyExports.h"

namespace thanosray::vk {

extern "C" __declspec(dllexport) void* g_procTable[kProcTableSize] = {};

// Build the export name list from exports_cpp.inc so it always stays in sync
// with thunks.asm / exports.inc.
static const char* const kExportNames[kProcTableSize] = {
#define X(name, idx) #name,
#include "..\..\exports_cpp.inc"
#undef X
};

const char* const* ExportNameTable()
{
    return kExportNames;
}

} // namespace thanosray::vk
