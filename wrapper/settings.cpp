//#include "pch.h"
#include "settings.h"
#include "src/log/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <thread>

namespace thanosray {

    static std::mutex   g_settingsMutex;
    static Settings     g_settings;
    static std::wstring g_iniPath;
    static FILETIME     g_lastWrite = { 0, 0 };
    static bool         g_inited = false;

    static inline std::string Trim(std::string s)
    {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && isSpace((unsigned char)s.back()))  s.pop_back();
        return s;
    }

    static inline std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
            });
        return s;
    }

    static bool ParseBool(const std::string& v, bool def)
    {
        std::string s = ToLower(Trim(v));
        if (s == "1" || s == "true" || s == "yes" || s == "on")  return true;
        if (s == "0" || s == "false" || s == "no" || s == "off") return false;
        return def;
    }

    static int32_t ParseI32(const std::string& v, int32_t def)
    {
        try {
            size_t idx = 0;
            int64_t x = std::stoll(Trim(v), &idx, 10);
            if (idx == 0) return def;
            if (x < INT32_MIN) x = INT32_MIN;
            if (x > INT32_MAX) x = INT32_MAX;
            return (int32_t)x;
        }
        catch (...) {
            return def;
        }
    }

    static float ParseF32(const std::string& v, float def)
    {
        try {
            size_t idx = 0;
            float x = std::stof(Trim(v), &idx);
            if (idx == 0) return def;
            return x;
        }
        catch (...) {
            return def;
        }
    }

    static std::wstring GetModuleDir()
    {
        // Get path of the module that contains THIS function (your wrapper DLL).
        HMODULE hm = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&GetModuleDir,
            &hm);

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(hm, path, MAX_PATH);

        std::wstring p = path;
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p.resize(slash);
        return p;
    }

    static FILETIME GetLastWriteTime(const std::wstring& path)
    {
        FILETIME ft = { 0,0 };
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            ft = fad.ftLastWriteTime;
        }
        return ft;
    }

    static bool FileTimeEqual(const FILETIME& a, const FILETIME& b)
    {
        return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
    }

    static void WriteDefaultIni(const std::wstring& path, const Settings& def)
    {
        std::ofstream f(std::filesystem::path(path), std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;

        f <<
            "; Thanos-ray settings\n"
            "; Lines starting with ';' or '#' are comments.\n"
            "; Format: key=value\n"
            "\n"
            "[debug]\n"
            "DebugOverlay=" << (def.DebugOverlay ? "1" : "0") << "\n"
            "VerboseLog=" << (def.VerboseLog ? "1" : "0") << "\n"
            "\n"
            "[visibility]\n"
            "; Quick knobs for too-dark scenes\n"
            "ExposureBoost=" << def.ExposureBoost << "\n"
            "ShadowLift=" << def.ShadowLift << "\n"
            "\n"
            "[fallback_gi]\n"
            "FallbackGI=" << (def.FallbackGI ? "1" : "0") << "\n"
            "GI_Quality=" << def.GI_Quality << "\n"
            "GI_Steps=" << def.GI_Steps << "\n"
            "GI_RaysPerPix=" << def.GI_RaysPerPix << "\n"
            "GI_HalfRes=" << (def.GI_HalfRes ? "1" : "0") << "\n"
            "\n"
            "[cpu]\n"
            "; 0 = auto\n"
            "WorkerThreads=" << def.WorkerThreads << "\n"
            "WorkGridX=" << def.WorkGridX << "\n"
            "WorkGridY=" << def.WorkGridY << "\n"
            "WorkGridZ=" << def.WorkGridZ << "\n";

        f.close();
    }

    static Settings LoadIniInto(Settings base, const std::wstring& path)
    {
        std::ifstream f{std::filesystem::path(path)};
        if (!f.is_open()) return base;

        std::string line;
        std::string section;

        while (std::getline(f, line)) {
            line = Trim(line);
            if (line.empty()) continue;
            if (line[0] == ';' || line[0] == '#') continue;

            // [section]
            if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
                section = ToLower(Trim(line.substr(1, line.size() - 2)));
                continue;
            }

            // key=value
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = ToLower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            // Allow section.key too, but keep it optional.
            std::string fullKey = section.empty() ? key : (section + "." + key);

            if (fullKey == "debug.debugoverlay" || fullKey == "debugoverlay") {
                base.DebugOverlay = ParseBool(val, base.DebugOverlay);
            }
            else if (fullKey == "debug.verboselog" || fullKey == "verboselog") {
                base.VerboseLog = ParseBool(val, base.VerboseLog);
            }
            else if (fullKey == "visibility.exposureboost" || fullKey == "exposureboost") {
                base.ExposureBoost = ParseF32(val, base.ExposureBoost);
            }
            else if (fullKey == "visibility.shadowlift" || fullKey == "shadowlift") {
                base.ShadowLift = ParseF32(val, base.ShadowLift);
            }
            else if (fullKey == "fallback_gi.fallbackgi" || fullKey == "fallbackgi") {
                base.FallbackGI = ParseBool(val, base.FallbackGI);
            }
            else if (fullKey == "fallback_gi.gi_quality" || fullKey == "gi_quality") {
                base.GI_Quality = ParseI32(val, base.GI_Quality);
            }
            else if (fullKey == "fallback_gi.gi_steps" || fullKey == "gi_steps") {
                base.GI_Steps = ParseI32(val, base.GI_Steps);
            }
            else if (fullKey == "fallback_gi.gi_raysperpix" || fullKey == "gi_raysperpix") {
                base.GI_RaysPerPix = ParseI32(val, base.GI_RaysPerPix);
            }
            else if (fullKey == "fallback_gi.gi_halfres" || fullKey == "gi_halfres") {
                base.GI_HalfRes = ParseBool(val, base.GI_HalfRes);
            }
            else if (fullKey == "cpu.workerthreads" || fullKey == "workerthreads") {
                base.WorkerThreads = ParseI32(val, base.WorkerThreads);
            }
            else if (fullKey == "cpu.workgridx" || fullKey == "workgridx") {
                base.WorkGridX = ParseI32(val, base.WorkGridX);
            }
            else if (fullKey == "cpu.workgridy" || fullKey == "workgridy") {
                base.WorkGridY = ParseI32(val, base.WorkGridY);
            }
            else if (fullKey == "cpu.workgridz" || fullKey == "workgridz") {
                base.WorkGridZ = ParseI32(val, base.WorkGridZ);
            }
        }

        // clamp a few sane ranges
        if (base.GI_Quality < 0) base.GI_Quality = 0;
        if (base.GI_Quality > 3) base.GI_Quality = 3;
        if (base.GI_Steps < 1) base.GI_Steps = 1;
        if (base.GI_Steps > 256) base.GI_Steps = 256;
        if (base.GI_RaysPerPix < 1) base.GI_RaysPerPix = 1;
        if (base.GI_RaysPerPix > 64) base.GI_RaysPerPix = 64;
        if (base.WorkerThreads < 0) base.WorkerThreads = 0;
        if (base.WorkerThreads > 256) base.WorkerThreads = 256;
        if (base.WorkGridX < 1) base.WorkGridX = 1;
        if (base.WorkGridY < 1) base.WorkGridY = 1;
        if (base.WorkGridZ < 1) base.WorkGridZ = 1;

        return base;
    }

    void Settings_Init()
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (g_inited) return;

        std::wstring dir = GetModuleDir();
        g_iniPath = dir + L"\\Thanos-ray.ini";

        // If missing, write defaults
        DWORD attrs = GetFileAttributesW(g_iniPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            Settings def;
            WriteDefaultIni(g_iniPath, def);
            thanosray::log::Logf("[SET] Created default INI: %ls", g_iniPath.c_str());
        }

        // Load
        Settings def;
        g_settings = LoadIniInto(def, g_iniPath);
        g_lastWrite = GetLastWriteTime(g_iniPath);
        g_inited = true;

        thanosray::log::Logf("[SET] Loaded INI: %ls", g_iniPath.c_str());
        thanosray::log::Logf("[SET] ExposureBoost=%.3f ShadowLift=%.3f FallbackGI=%d Quality=%d Steps=%d Rays=%d HalfRes=%d Threads=%d Grid=%dx%dx%d",
            g_settings.ExposureBoost, g_settings.ShadowLift,
            g_settings.FallbackGI ? 1 : 0,
            (int)g_settings.GI_Quality,
            (int)g_settings.GI_Steps,
            (int)g_settings.GI_RaysPerPix,
            g_settings.GI_HalfRes ? 1 : 0,
            (int)g_settings.WorkerThreads,
            (int)g_settings.WorkGridX, (int)g_settings.WorkGridY, (int)g_settings.WorkGridZ
        );
    }

    Settings Settings_Get()
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        return g_settings;
    }

    bool Settings_ReloadNow()
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (!g_inited) return false;

        Settings def;
        Settings loaded = LoadIniInto(def, g_iniPath);
        g_settings = loaded;
        g_lastWrite = GetLastWriteTime(g_iniPath);

        thanosray::log::Logf("[SET] Reloaded INI");
        return true;
    }

    bool Settings_ReloadIfChanged()
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (!g_inited) return false;

        FILETIME ft = GetLastWriteTime(g_iniPath);
        if (FileTimeEqual(ft, g_lastWrite)) return false;

        Settings def;
        Settings loaded = LoadIniInto(def, g_iniPath);
        g_settings = loaded;
        g_lastWrite = ft;

        thanosray::log::Logf("[SET] Reloaded INI (file changed)");
        return true;
    }

    const wchar_t* Settings_GetIniPath()
    {
        return g_iniPath.c_str();
    }

} // namespace thanosray
