#include "PSOLAPitchShifter.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

namespace
{
    constexpr float pi = 3.14159265358979323846f;
}

PSOLAPitchShifter::PSOLAPitchShifter (double sampleRateIn)
    : sampleRate (sampleRateIn), periodSamples (sampleRate / 150.0) // 150Hz: a reasonable default before any real pitch has been detected
{
    maxPeriodSamples = (int) std::ceil (sampleRate / (double) minHz);

    // Output is read from a fixed tap `latencySamples` behind the write
    // head — see shiftPitch()'s doc for the derivation: one period of
    // lookahead to fill a symmetric grain around a mark, plus one more
    // period before the accumulator slot a mark could still touch has
    // definitely been fully written. Confirmed exact (not just derived)
    // by benchmarks/PSOLALatencyProbe.cpp's onset probe across several
    // sample-rate/frequency combinations — matched to the sample at every
    // one once placeGrainAt()'s floor-based read position got an epsilon
    // guard (see that function's comment for the real bug that masked as
    // a latency-derivation problem for one specific combination before
    // that fix).
    latencySamples = 2 * maxPeriodSamples;

    // Generous, fixed headroom on top of the theoretical minimum (rather
    // than tightly sizing to exactly what's needed) — cheap in memory
    // (a few tens of KB of floats) and avoids off-by-one wraparound risk
    // if a caller ever passes an unusually large block size.
    const size_t bufferMargin = 4096;
    history.assign ((size_t) latencySamples + bufferMargin, 0.0f);
    accumulator.assign ((size_t) latencySamples + bufferMargin, 0.0f);
    accumulatorWeight.assign (accumulator.size(), 0.0f);
}

void PSOLAPitchShifter::updatePeriodEstimate (float detectedHz) noexcept
{
    if (detectedHz > 0.0f)
    {
        const float clampedHz = std::clamp (detectedHz, minHz, maxHz);
        periodSamples = sampleRate / (double) clampedHz;
    }
    // detectedHz <= 0 (unvoiced/silence): hold the previous estimate — see
    // the header doc for why holding rather than resetting is the right
    // default here.
}

void PSOLAPitchShifter::placeGrainAt (double synthesisMarkPos)
{
    // Grain half-width is the *original* (unshifted) period, not the
    // shifted target — this is the actual mechanism by which PSOLA
    // preserves formants while changing pitch: spacing between
    // consecutive synthesis marks sets perceived pitch, grain width sets
    // the spectral envelope, and those are independent knobs here on
    // purpose.
    const int halfWidth = std::clamp ((int) std::llround (periodSamples), 1, maxPeriodSamples);

    // The actual pitch-shift mechanism: READ position and WRITE position
    // are deliberately *different*. Content always comes from the most
    // recently completed analysis mark at or before this synthesis mark
    // (a grid of original-period-spaced positions, floor-aligned rather
    // than nearest — see the .cpp-level comment in shiftPitch() for why
    // floor, not round, keeps the lookahead/latency bound tight). That
    // same source content gets *placed* at the synthesis mark's own
    // position. When synthesis marks are closer together than the
    // analysis grid (pitch shift up), several consecutive synthesis marks
    // land in the same analysis bucket and reuse/repeat that grain's
    // content at closer spacing; when they're farther apart (shift down),
    // some analysis buckets never get used at all. Reusing/skipping
    // content at a different spacing than it was recorded at is what
    // changes pitch — placing content back at the position it was read
    // from (which an earlier version of this function did) cannot change
    // pitch at all, only reconstruct the original.
    // The tiny epsilon before flooring matters: nextMarkPos accumulates by
    // repeated += synthesisSpacing (shiftPitch()'s loop), and at a 1:1
    // ratio that spacing equals periodSamples exactly, so
    // synthesisMarkPos "should" land on an exact integer multiple of
    // periodSamples at every mark. Floating-point rounding from repeated
    // addition can leave it a few ULPs *below* that integer instead of
    // exactly on it, and plain std::floor of e.g. 17.999999997 gives 17,
    // not 18 — reading an entire extra period of stale (wrong-grain)
    // content. Found by direct instrumentation after a latency probe
    // showed a real but unexplained ~1-period content gap specific to
    // certain sample-rate/frequency combinations (not all — only ones
    // where the accumulated rounding happened to land on the wrong side
    // of an integer boundary) — see PSOLALatencyProbe.cpp's comment.
    const double readMarkPos = std::floor (synthesisMarkPos / periodSamples + 1.0e-6) * periodSamples;
    const long long readCenter = (long long) std::llround (readMarkPos);
    const long long writeCenter = (long long) std::llround (synthesisMarkPos);

    for (int k = -halfWidth; k <= halfWidth; ++k)
    {
        const long long srcAbs = readCenter + (long long) k;
        if (srcAbs < 0)
            continue; // no real history yet this far back — only happens during startup

        const float windowValue = grainWindow[(size_t) (k + halfWidth)];
        const size_t historySlot = (size_t) (srcAbs % (long long) history.size());
        const float sample = history[historySlot] * windowValue;

        const long long dstAbs = writeCenter + (long long) k;
        if (dstAbs < 0)
            continue;
        const size_t accSlot = (size_t) (dstAbs % (long long) accumulator.size());
        accumulator[accSlot] += sample;
        accumulatorWeight[accSlot] += windowValue;
    }
}

void PSOLAPitchShifter::shiftPitch (float detectedHz, float semitoneShift,
                                     const std::vector<float>& input, std::vector<float>& output)
{
    updatePeriodEstimate (detectedHz);

    // Precomputed once per call (not per grain): periodSamples is fixed
    // for the whole call already (locally-stationary-pitch simplification
    // documented in the class doc), so every grain fired during this call
    // uses the identical window.
    const int halfWidth = std::clamp ((int) std::llround (periodSamples), 1, maxPeriodSamples);
    grainWindow.assign ((size_t) (2 * halfWidth + 1), 0.0f);
    for (int k = 0; k < (int) grainWindow.size(); ++k)
        grainWindow[(size_t) k] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) k / (float) (grainWindow.size() - 1));

    const double shiftRatio = std::pow (2.0, (double) semitoneShift / 12.0);
    const double synthesisSpacing = periodSamples / shiftRatio;

    for (size_t i = 0; i < input.size(); ++i)
    {
        history[(size_t) (totalSamplesIn % (long long) history.size())] = input[i];
        ++totalSamplesIn;

        // A mark is eligible once its full grain (mark ± periodSamples)
        // is entirely within already-written history — this is the only
        // place lookahead is actually spent. Still correct with
        // placeGrainAt's floor-based read position: since the actual read
        // position is always <= this synthesis mark's own position (floor
        // rounds down, never up), this check is a safe — if occasionally
        // very slightly conservative — bound on the read requirement too.
        while (nextMarkPos + periodSamples <= (double) (totalSamplesIn - 1))
        {
            placeGrainAt (nextMarkPos);
            nextMarkPos += synthesisSpacing;
        }

        // Fixed-delay-line read: position (totalSamplesIn - 1 -
        // latencySamples) is guaranteed safe because latencySamples uses
        // the worst-case (longest possible) period, and no mark can ever
        // reach farther back than periodSamples <= maxPeriodSamples from
        // wherever nextMarkPos currently is.
        const long long readPos = totalSamplesIn - 1 - (long long) latencySamples;
        if (readPos >= 0)
        {
            const size_t slot = (size_t) (readPos % (long long) accumulator.size());
            const float weight = accumulatorWeight[slot];
            output[i] = weight > 1.0e-6f ? accumulator[slot] / weight : accumulator[slot];
            accumulator[slot] = 0.0f;
            accumulatorWeight[slot] = 0.0f;
        }
        else
        {
            output[i] = 0.0f; // still within the initial latencySamples fill-up
        }
    }
}

} // namespace pitchzazz
