#pragma once
#include "ShaderDiagnostics.h"
#include <chrono>
#include <thread>
#include <unordered_map>
#include <algorithm>

namespace thanosray::vk::pacing {
using Clock=std::chrono::steady_clock;
inline Clock::time_point Reserve(Clock::time_point now, Clock::time_point& previous, int fps)
{
    const auto period=std::chrono::nanoseconds(1000000000ll/fps);
    previous=std::max(now,previous+period);
    return previous;
}
inline void BeforePresent(VkQueue queue)
{
    static const int fps=[] {
        const int value=GetPrivateProfileIntW(L"experiments",L"FrameLimit",0,diagnostics::ConfigPath().c_str());
        const int limit=value>=10 && value<=240 ? value : 0;
        if(limit) log::Logf("[FRAME-LIMIT] enabled fps=%d",limit);
        return limit;
    }();
    if(!fps) return;
    static std::mutex mutex;
    static std::unordered_map<VkQueue,Clock::time_point> queues;
    Clock::time_point target;
    {
        std::lock_guard lock(mutex);
        // A bounded cache also covers applications that repeatedly recreate devices.
        if(queues.size()>=64 && !queues.count(queue)) queues.clear();
        target=Reserve(Clock::now(),queues[queue],fps);
    }
    std::this_thread::sleep_until(target);
}
}
