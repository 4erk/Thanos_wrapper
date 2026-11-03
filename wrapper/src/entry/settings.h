#pragma once
#include <cstdint>

namespace thanosray {

    struct Settings
    {
        // ---- General / debug ----
        bool     DebugOverlay = false;
        bool     VerboseLog = false;

        // ---- Lighting visibility knobs (quick wins) ----
        // These do nothing by themselves yet — you’ll use them in your hooks/passes later.
        float    ExposureBoost = 0.0f;     // e.g. 0.15 makes everything brighter
        float    ShadowLift = 0.0f;     // e.g. 0.10 lifts dark areas a bit

        // ---- Future “fallback RT” controls ----
        bool     FallbackGI = true;
        int32_t  GI_Quality = 2;        // 0..3
        int32_t  GI_Steps = 16;       // ray-march steps in screen space
        int32_t  GI_RaysPerPix = 2;        // “sample count” per pixel
        bool     GI_HalfRes = true;

        // ---- CPU/worker knobs (for later CPU-side work / scheduling) ----
        int32_t  WorkerThreads = 0;        // 0 = auto (hardware_concurrency)
        int32_t  WorkGridX = 50;       // your idea: user adjustable
        int32_t  WorkGridY = 50;
        int32_t  WorkGridZ = 50;
    };

    // Initializes settings system, creates INI if missing, loads it.
    void Settings_Init();

    // Get a copy of current settings (thread-safe).
    Settings Settings_Get();

    // Force reload from disk (thread-safe). Returns true if reloaded.
    bool Settings_ReloadNow();

    // Reload only if file changed (cheap). Returns true if reloaded.
    bool Settings_ReloadIfChanged();

    // Path to the ini file (wide string). Mainly for logging/debug.
    const wchar_t* Settings_GetIniPath();

} // namespace thanosray

