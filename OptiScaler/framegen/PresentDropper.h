#pragma once

#include <Util.h>
#include <framegen/DynamicFG.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

// EXPERIMENT: decimal frame generation by dropping presents.
// Frame generation runs at a fixed multiplier, this decides per present whether it reaches the screen
// so the displayed framerate approaches a target. Used by the dynamic FG for targets below 2x.
// Environment variables for testing:
//   OPTI_PRESENT_LOG=1           log present rate and caller once per second
//   OPTI_PRESENT_LOG=2           also write every decision to PresentDropper.csv (qpc,kept)
//   OPTI_PRESENT_DROP_TARGET=N   force dropping to N fps, overrides the dynamic FG
//   OPTI_PRESENT_DROP_METHOD=skip  skip the Present instead of repeating the previous frame
class PresentDropper
{
  public:
    // Set by the dynamic FG, 0 is off
    static void SetTarget(double fps) { _runtimeTarget = fps; }

    // Expected spacing of frame generation output (real frame time / multiplier), lets presents that arrive
    // in a burst (vsync off, flip metering paces them on the GPU) get their intended display times
    static void SetSlotMs(double ms) { _slotMs = ms; }

    static bool SkipMethod()
    {
        static const bool skip = EnvString("OPTI_PRESENT_DROP_METHOD") == "skip";
        return skip;
    }

    // Returns true when this present should not reach the screen
    static bool ShouldDrop(void* caller)
    {
        static const double logLevel = EnvFloat("OPTI_PRESENT_LOG");
        static const double forcedTarget = EnvFloat("OPTI_PRESENT_DROP_TARGET");

        const double target = forcedTarget > 0.0 ? forcedTarget : _runtimeTarget.load();
        const double now = Util::MillisecondsNow();
        bool drop = false;

        if (_lastMs > 0.0)
        {
            const double delta = now - _lastMs;

            // Pause, alt-tab, loading screen, start over
            if (delta > 250.0)
            {
                _avgIntervalMs = 0.0;
                _nextShowMs = 0.0;
                _activeSinceMs = 0.0;
            }
            else
            {
                _avgIntervalMs = _avgIntervalMs <= 0.0 ? delta : _avgIntervalMs + 0.1 * (delta - _avgIntervalMs);
            }
        }

        _lastMs = now;

        // Intended display time: presents closer together than half a slot are part of a burst,
        // they will be shown one slot apart
        const double slot = _slotMs.load() > 0.0 ? _slotMs.load() : _avgIntervalMs;
        if (_virtualMs > 0.0 && slot > 0.0 && now - _lastCallMs < slot * 0.5)
            _virtualMs += slot;
        else
            _virtualMs = now;

        _lastCallMs = now;

        // Dropping during frame generation warm-up stops DLSSG generating for good, wait a bit first
        if (target <= 0.0)
            _activeSinceMs = 0.0;
        else if (_activeSinceMs <= 0.0)
            _activeSinceMs = now;

        if (target > 0.0 && slot > 0.0 && now - _activeSinceMs > WarmupMs)
        {
            // Output clock at the target rate, show a present when its display time reaches the next tick.
            // Half a slot of slack so a present close to the tick isn't pushed to the next one
            const double period = 1000.0 / target;
            const double slack = slot * 0.5;

            if (_nextShowMs <= 0.0 || _virtualMs - _nextShowMs > period * 2.0 || _nextShowMs - _virtualMs > period * 2.0)
                _nextShowMs = _virtualMs;

            if (_virtualMs + slack >= _nextShowMs)
                _nextShowMs += period;
            else
                drop = true;
        }

        _presents++;
        if (drop)
            _dropped++;

        if (logLevel >= 2.0)
            LogDecision(!drop);

        if (target > 0.0)
        {
            DynamicFGStats::mode = DynamicFGStats::PresentDropping;
            DynamicFGStats::targetFps = (float) target;
        }
        else if (DynamicFGStats::mode == DynamicFGStats::PresentDropping)
        {
            DynamicFGStats::mode = nullptr;
        }

        if (now - _windowStartMs >= 1000.0)
        {
            // Displayed framerate over the last second, for the overlay
            DynamicFGStats::outputFps = (float) ((_presents - _dropped) * 1000.0 / (now - _windowStartMs));

            if (logLevel > 0.0)
            {
                char path[MAX_PATH] {};
                if (auto module = Util::GetCallerModule(caller); module != nullptr)
                    GetModuleFileNameA(module, path, MAX_PATH);

                LOG_INFO("PresentDropper: presents {}, dropped {}, avg interval {:.2f}ms, target {}, caller {}",
                         _presents, _dropped, _avgIntervalMs, target, path);
            }

            _windowStartMs = now;
            _presents = 0;
            _dropped = 0;
        }

        return drop;
    }

  private:
    static void LogDecision(bool kept)
    {
        static FILE* file = nullptr;
        if (file == nullptr && fopen_s(&file, "PresentDropper.csv", "w") == 0 && file != nullptr)
            fprintf(file, "qpc,kept\n");

        if (file != nullptr)
        {
            LARGE_INTEGER qpc {};
            QueryPerformanceCounter(&qpc);
            fprintf(file, "%lld,%d\n", qpc.QuadPart, kept ? 1 : 0);
            fflush(file);
        }
    }

    static std::string EnvString(const char* name)
    {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
            return {};

        std::string result = value;
        free(value);
        return result;
    }

    static double EnvFloat(const char* name)
    {
        auto value = EnvString(name);
        return value.empty() ? 0.0 : std::atof(value.c_str());
    }

    inline static std::atomic<double> _runtimeTarget = 0.0;
    inline static std::atomic<double> _slotMs = 0.0;
    inline static double _activeSinceMs = 0.0;
    static constexpr double WarmupMs = 2000.0;
    inline static double _virtualMs = 0.0;
    inline static double _lastCallMs = 0.0;
    inline static double _lastMs = 0.0;
    inline static double _avgIntervalMs = 0.0;
    inline static double _nextShowMs = 0.0;
    inline static double _windowStartMs = 0.0;
    inline static uint32_t _presents = 0;
    inline static uint32_t _dropped = 0;
};
