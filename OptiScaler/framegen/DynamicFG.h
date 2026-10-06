#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

// What the dynamic FG is doing right now, shown on the FPS overlay
struct DynamicFGStats
{
    static constexpr const char* CountSwitching = "DLSSG count switching";
    static constexpr const char* PresentDropping = "present dropping";

    inline static std::atomic<const char*> mode = nullptr; // nullptr when inactive
    inline static std::atomic<float> targetFps = 0.0f;
    inline static std::atomic<float> baseFps = 0.0f;   // real frames
    inline static std::atomic<float> outputFps = 0.0f; // presents that reach the screen
    inline static std::atomic<uint32_t> decision = 0;  // generated frames for the last real frame
    inline static std::atomic<uint32_t> maxFrames = 0;
};

// Decides, once per real frame, how many frames to interpolate so that the
// average output framerate approaches a target ("decimal" frame generation).
//
// Uses error diffusion: every real frame owes (realFrameTime / targetFrameTime - 1)
// generated frames, the fractional part is carried over to the next frame.
// Example: base 80 fps, target 120 fps -> owes 0.5 per frame -> 0,1,0,1,...
class DynamicFGController
{
  public:
    // nowMs:      monotonic timestamp of this real frame in milliseconds
    // targetFps:  desired output framerate, <= 0 disables the controller
    // maxFrames:  frames the backend would generate for this frame (0 = none possible)
    // minFrames:  lowest allowed decision, DLSSG can't be switched off per frame
    // Returns the number of frames to generate, in [minFrames, maxFrames]
    uint32_t Decide(double nowMs, double targetFps, uint32_t maxFrames, uint32_t minFrames = 0)
    {
        const bool firstFrame = _lastMs < 0.0;
        const double deltaMs = nowMs - _lastMs;
        _lastMs = nowMs;

        if (targetFps <= 0.0)
        {
            Reset(nowMs);
            return maxFrames;
        }

        // Pause, alt-tab, loading screen etc. Start over instead of averaging it in
        if (firstFrame || deltaMs <= 0.0 || deltaMs > ResetThresholdMs)
        {
            Reset(nowMs);
            return maxFrames;
        }

        _avgFrameTimeMs = _avgFrameTimeMs <= 0.0 ? deltaMs : _avgFrameTimeMs + Smoothing * (deltaMs - _avgFrameTimeMs);

        if (maxFrames == 0)
            return 0;

        minFrames = (std::min) (minFrames, maxFrames);

        const double targetFrameTimeMs = 1000.0 / targetFps;
        const double owed = _avgFrameTimeMs / targetFrameTimeMs - 1.0;

        _debt += std::clamp(owed, static_cast<double>(minFrames), static_cast<double>(maxFrames));

        const auto frames = static_cast<uint32_t>(std::min(_debt, static_cast<double>(maxFrames)));
        _debt -= frames;

        _lastDecision = frames;
        return frames;
    }

    void Reset(double nowMs = -1.0)
    {
        _lastMs = nowMs;
        _avgFrameTimeMs = 0.0;
        _debt = 0.0;
    }

    double AverageFrameTimeMs() const { return _avgFrameTimeMs; }
    uint32_t LastDecision() const { return _lastDecision; }

    static constexpr double ResetThresholdMs = 250.0;
    static constexpr double Smoothing = 0.25;

  private:
    double _lastMs = -1.0;
    double _avgFrameTimeMs = 0.0;
    double _debt = 0.0;
    uint32_t _lastDecision = 0;
};
