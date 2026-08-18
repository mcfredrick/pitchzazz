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
    // head. Three periods, not two: one period of lookahead to fill a
    // symmetric grain around a mark, one more before the accumulator
    // slot a mark could still touch has definitely been fully written
    // (this pair is the same derivation a single-bucket version of
    // placeGrainAt() needed), plus a third because placeGrainAt() now
    // cross-fades between *two* neighboring analysis buckets instead of
    // snapping to one (see that function's doc for why — it fixes a real
    // crackle/beating artifact) and the farther of the two needs a full
    // extra period of lookahead in the worst case. This is a genuine
    // latency cost of the quality fix, not padding — see
    // docs/PERFORMANCE_LOG.md's dated entry for the actual before/after
    // numbers and why it's still judged worth paying.
    latencySamples = 3 * maxPeriodSamples;

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
    // are deliberately *different*. Content comes from the original-
    // period-spaced analysis grid; that content gets *placed* at the
    // synthesis mark's own position instead. When synthesis marks are
    // closer together than the analysis grid (pitch shift up), several
    // consecutive synthesis marks draw from the same neighborhood of
    // analysis content and repeat it at closer spacing; when they're
    // farther apart (shift down), some analysis content never gets used.
    // Reusing/skipping content at a different spacing than it was
    // recorded at is what changes pitch — placing content back at the
    // exact position it was read from (an earlier version of this
    // function did that) cannot change pitch at all, only reconstruct
    // the original.
    //
    // Cross-fades between the two *nearest* analysis buckets rather than
    // snapping to one (an earlier version did that too, and produced an
    // audible crackle/low-frequency-beat artifact — see docs/FINDINGS.md
    // for the full diagnosis). Snapping to a single bucket means the
    // source content jumps discretely every time the synthesis position
    // crosses a bucket boundary; blending the two neighbors by how close
    // the synthesis mark is to each removes that discontinuity. This
    // costs one more period of lookahead (both neighboring buckets must
    // be available, not just one) — see shiftPitch()'s trigger condition
    // and getLatencySamples()'s doc for the resulting latency change.
    //
    // The tiny epsilon in bucketPos matters for the same reason a floor-
    // only version needed one: nextMarkPos accumulates by repeated +=
    // synthesisSpacing (shiftPitch()'s loop), and at a 1:1 ratio that
    // spacing equals periodSamples exactly, so synthesisMarkPos "should"
    // land on an exact integer multiple of periodSamples at every mark.
    // Floating-point rounding from repeated addition can leave it a few
    // ULPs *below* that integer instead of exactly on it, and an
    // unguarded floor of e.g. 17.999999997 gives 17, not 18 — an entire
    // extra period of misplaced blend weight. Found by direct
    // instrumentation the first time this project hit it (see
    // docs/FINDINGS.md #18); guarded here from the start this time.
    const double bucketPos = synthesisMarkPos / periodSamples + 1.0e-6;
    const double bucketFloor = std::floor (bucketPos);
    const float blendHigh = (float) (bucketPos - bucketFloor);
    const float blendLow = 1.0f - blendHigh;

    const long long readCenterLow = (long long) std::llround (bucketFloor * periodSamples);
    const long long readCenterHigh = (long long) std::llround ((bucketFloor + 1.0) * periodSamples);
    const long long writeCenter = (long long) std::llround (synthesisMarkPos);

    for (int k = -halfWidth; k <= halfWidth; ++k)
    {
        const long long lowAbs = readCenterLow + (long long) k;
        const long long highAbs = readCenterHigh + (long long) k;
        if (lowAbs < 0 || highAbs < 0)
            continue; // no real history yet this far back — only happens during startup

        const float windowValue = grainWindow[(size_t) (k + halfWidth)];
        const float lowSample = history[(size_t) (lowAbs % (long long) history.size())];
        const float highSample = history[(size_t) (highAbs % (long long) history.size())];
        const float sample = (blendLow * lowSample + blendHigh * highSample) * windowValue;

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

        // A mark is eligible once *both* analysis buckets placeGrainAt()
        // cross-fades between are entirely within already-written
        // history. The lower bucket alone would need mark+periodSamples
        // of lookahead (as when this read from one bucket only); the
        // upper bucket sits one more full period beyond that in the
        // worst case (mark landing exactly on a bucket boundary), hence
        // 2*periodSamples here, not periodSamples. This is where the
        // cross-fade's extra period of lookahead actually gets spent —
        // see getLatencySamples()'s doc for the resulting fixed-tap
        // change (2 periods -> 3).
        while (nextMarkPos + 2.0 * periodSamples <= (double) (totalSamplesIn - 1))
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
