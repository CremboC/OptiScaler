#pragma once

#include <Util.h>
#include <framegen/DynamicFG.h>

#include <atomic>
#include <cstdlib>
#include <string>

// EXPERIMENT: decimal frame generation by dropping presents.
// Frame generation runs at a fixed multiplier, this decides per present whether it reaches the screen
// so the displayed framerate approaches a target.
// Controlled by environment variables so it stays out of the config while testing:
//   OPTI_PRESENT_LOG=1           log present rate and caller once per second
//   OPTI_PRESENT_DROP_TARGET=N   drop presents to approach N fps (0 or unset is off)
class PresentDropper
{
  public:
    // Returns true when this present should be skipped
    static bool ShouldDrop(void* caller)
    {
        static const bool logEnabled = EnvFloat("OPTI_PRESENT_LOG") > 0.0;
        static const double target = EnvFloat("OPTI_PRESENT_DROP_TARGET");

        const double now = Util::MillisecondsNow();
        bool drop = false;

        if (_lastMs > 0.0)
        {
            const double delta = now - _lastMs;

            // Pause, alt-tab, loading screen, start over
            if (delta > 250.0)
            {
                _avgIntervalMs = 0.0;
                _acc = 0.0;
            }
            else
            {
                _avgIntervalMs = _avgIntervalMs <= 0.0 ? delta : _avgIntervalMs + 0.1 * (delta - _avgIntervalMs);
            }
        }

        _lastMs = now;

        if (target > 0.0 && _avgIntervalMs > 0.0)
        {
            // Fraction of incoming presents that should be shown, spread evenly (error diffusion)
            const double keep = (std::min) (1.0, _avgIntervalMs * target / 1000.0);
            _acc += keep;

            if (_acc >= 1.0)
                _acc -= 1.0;
            else
                drop = true;
        }

        _presents++;
        if (drop)
            _dropped++;

        if (target > 0.0)
        {
            DynamicFGStats::mode = DynamicFGStats::PresentDropping;
            DynamicFGStats::targetFps = (float) target;
        }

        if (now - _windowStartMs >= 1000.0)
        {
            // Displayed framerate over the last second, for the overlay
            DynamicFGStats::outputFps = (float) ((_presents - _dropped) * 1000.0 / (now - _windowStartMs));

            if (logEnabled)
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
    static double EnvFloat(const char* name)
    {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
            return 0.0;

        double result = std::atof(value);
        free(value);
        return result;
    }

    inline static double _lastMs = 0.0;
    inline static double _avgIntervalMs = 0.0;
    inline static double _acc = 0.0;
    inline static double _windowStartMs = 0.0;
    inline static uint32_t _presents = 0;
    inline static uint32_t _dropped = 0;
};
