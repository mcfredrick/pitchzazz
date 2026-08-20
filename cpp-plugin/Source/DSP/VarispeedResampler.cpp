#include "VarispeedResampler.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

namespace
{
    // Catmull-Rom cubic Hermite spline through (0,y0)(1,y1)(2,y2)(3,y3),
    // evaluated at t in [0,1] between y1 and y2 — the standard tension-0.5
    // Catmull-Rom coefficients, not an ad hoc polynomial.
    float catmullRom (float y0, float y1, float y2, float y3, float t) noexcept
    {
        const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float a2 = -0.5f * y0 + 0.5f * y2;
        const float a3 = y1;
        return ((a0 * t + a1) * t + a2) * t + a3;
    }
}

VarispeedResampler::VarispeedResampler()
{
    // Generous, fixed headroom (~370ms @44.1kHz) rather than tightly sized
    // to the 4-sample kernel width alone — the caller (VarispeedShifter)
    // may push several blocks' worth before pulling, since this class is
    // one stage of a two-stage variable-rate pipeline with no guaranteed
    // 1:1 push/pull cadence. Cheap in memory (a few tens of KB of floats),
    // the same "headroom over the theoretical minimum" trade-off
    // PSOLAPitchShifter's own history buffer already makes.
    constexpr size_t historySize = 16384;
    history.assign (historySize, 0.0f);
}

void VarispeedResampler::push (const float* input, int count)
{
    for (int i = 0; i < count; ++i)
    {
        history[(size_t) (totalPushed % (long long) history.size())] = input[i];
        ++totalPushed;
    }
}

int VarispeedResampler::pull (float ratio, float* output, int maxCount)
{
    const auto sampleAt = [this] (long long idx) noexcept
    {
        return history[(size_t) (idx % (long long) history.size())];
    };

    int produced = 0;
    for (; produced < maxCount; ++produced)
    {
        const auto idx0 = (long long) std::floor (readPos);

        // Need idx0+2 (the last sample the kernel reads) to already be
        // pushed — if not, this and every later output sample in this
        // call must wait for more input, so stop here rather than
        // producing a partially-correct sample.
        if (idx0 + kernelLookahead >= totalPushed)
            break;

        // Start-of-stream edge case: there's no real sample before index
        // 0 yet, so repeat index 0 rather than reading a negative/wrapped
        // index — a one-off clamp, not a general bounds-check substitute
        // for the sizing this class otherwise relies on.
        const auto idxPrev = std::max<long long> (idx0 - 1, 0);

        const float y0 = sampleAt (idxPrev);
        const float y1 = sampleAt (idx0);
        const float y2 = sampleAt (idx0 + 1);
        const float y3 = sampleAt (idx0 + 2);
        const auto frac = (float) (readPos - (double) idx0);

        output[produced] = catmullRom (y0, y1, y2, y3, frac);
        readPos += (double) ratio;
    }
    return produced;
}

} // namespace pitchzazz
