#include "PSOLAPitchShifter.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

namespace
{
    constexpr float pi = 3.14159265358979323846f;
}

float chooseGrainWidthMultiplierForShift (float semitoneShift) noexcept
{
    const float shiftRatio = std::pow (2.0f, semitoneShift / 12.0f);
    if (shiftRatio <= 1.0f)
        return 1.0f;
    return std::clamp (1.0f / shiftRatio, grainWidthMultiplierMin, 1.0f);
}

PSOLAPitchShifter::PSOLAPitchShifter (double sampleRateIn)
    : sampleRate (sampleRateIn), periodSamples (sampleRate / 150.0) // 150Hz: a reasonable default before any real pitch has been detected
{
    maxPeriodSamples = (int) std::ceil (sampleRate / (double) minHz);

    // Worst-case half-width across both the lowest detectable pitch and
    // the widest the grain-width multiplier can ever be set to — see the
    // header's doc on maxHalfWidthSamples. grainWidthMultiplierMax > 1.0
    // by construction (see PSOLAPitchShifter.h), so this is always >=
    // maxPeriodSamples, never smaller.
    maxHalfWidthSamples = (int) std::ceil ((double) maxPeriodSamples * (double) grainWidthMultiplierMax);

    // See getLatencySamples()'s doc for the full derivation: with the
    // correlation-aligned crackle fix, a mark's required read content can
    // now reach one whole period, plus bucket B's own half-width, plus the
    // alignment search radius, past its own write position.
    maxForwardReachSamples = maxPeriodSamples + 2 * maxHalfWidthSamples + psolaAlignSearchRadius;

    // Output is read from a fixed tap `latencySamples` behind the write
    // head. NOT `2 * maxForwardReachSamples` -- that would double-count
    // the forward-reach term instead of adding the actual second
    // component the margin needs (a write-side half-width, not another
    // full forward-reach). Proof sketch (also in getLatencySamples()'s
    // doc): reading accumulator slot P is safe once every mark that could
    // still touch it (any M with M <= P + maxHalfWidthSamples, the
    // write-side reach) is guaranteed to have already fired, which holds
    // once M <= totalSamplesIn - 1 - maxForwardReachSamples for all such
    // M -- solving P + maxHalfWidthSamples <= totalSamplesIn - 1 -
    // maxForwardReachSamples for latencySamples = totalSamplesIn - 1 - P
    // gives exactly latencySamples >= maxForwardReachSamples +
    // maxHalfWidthSamples. This is the *tight* bound, not a cautious
    // extra margin on top of it: notice it degenerates to exactly the
    // original single-bucket formula (2 * maxHalfWidthSamples) when
    // maxForwardReachSamples == maxHalfWidthSamples, which is the case
    // before this change (a single-bucket mark's forward reach *is* just
    // its own half-width) -- confirming the original "2x" was already
    // this same tight bound, not extra headroom being removed here.
    latencySamples = maxForwardReachSamples + maxHalfWidthSamples;

    // Generous, fixed headroom on top of the theoretical minimum (rather
    // than tightly sizing to exactly what's needed) — cheap in memory
    // (a few tens of KB of floats) and avoids off-by-one wraparound risk
    // if a caller ever passes an unusually large block size.
    const size_t bufferMargin = 4096;
    history.assign ((size_t) latencySamples + bufferMargin, 0.0f);
    accumulator.assign ((size_t) latencySamples + bufferMargin, 0.0f);
    accumulatorWeight.assign (accumulator.size(), 0.0f);

    // See grainWindow's doc: reserved once, to the worst case, so a
    // grain-width multiplier change at runtime can never force a
    // reallocation here.
    grainWindow.reserve ((size_t) (2 * maxHalfWidthSamples + 1));
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

int PSOLAPitchShifter::searchAlignmentOffset (long long refCenter, long long candCenter, int halfWidth) const noexcept
{
    // Same NCC + hard-minimum-improvement pattern as
    // WSOLATimeStretcher::searchBestOffset, for the identical reason: a
    // pure best-correlation search has a degenerate trivial solution
    // (offset 0 already correlates reasonably against near-periodic
    // content), so nominal has to be beaten substantially, not just edged
    // out, before being allowed to move — otherwise finite-window/
    // floating-point noise alone could pick a different offset every call
    // on genuinely stationary material, which is worse than not aligning
    // at all.
    const int correlationLength = std::min (halfWidth, maxAlignCorrelationLength);

    const auto sampleAt = [this] (long long abs) noexcept -> float
    {
        if (abs < 0)
            return 0.0f;
        return history[(size_t) (abs % (long long) history.size())];
    };

    float refEnergy = 0.0f;
    for (int k = 0; k < correlationLength; ++k)
    {
        const float a = sampleAt (refCenter + k);
        refEnergy += a * a;
    }

    const auto correlationAt = [&] (long long candStart) noexcept -> float
    {
        float dot = 0.0f;
        float candEnergy = 0.0f;
        for (int k = 0; k < correlationLength; ++k)
        {
            const float a = sampleAt (refCenter + k);
            const float b = sampleAt (candStart + k);
            dot += a * b;
            candEnergy += b * b;
        }
        return dot / (std::sqrt (refEnergy * candEnergy) + 1.0e-9f);
    };

    constexpr float minImprovementToDeviate = 0.02f;
    const float nominalScore = correlationAt (candCenter);
    float bestScore = nominalScore;
    int bestOffset = 0;

    for (int offset = -psolaAlignSearchRadius; offset <= psolaAlignSearchRadius; ++offset)
    {
        if (offset == 0)
            continue;
        const float score = correlationAt (candCenter + offset);
        if (score > nominalScore + minImprovementToDeviate && score > bestScore)
        {
            bestScore = score;
            bestOffset = offset;
        }
    }
    return bestOffset;
}

void PSOLAPitchShifter::placeGrainAt (double synthesisMarkPos)
{
    // Grain half-width is the *original* (unshifted) period — scaled by
    // grainWidthMultiplier, the creative control (docs/ROADMAP.md Phase
    // 5) that deliberately trades away some of the formant-preservation
    // property this decoupling exists for, see the header's doc — not the
    // shifted target: spacing between consecutive synthesis marks sets
    // perceived pitch, grain width sets the spectral envelope, and those
    // are independent knobs here on purpose. Clamped to maxHalfWidthSamples
    // (not maxPeriodSamples), the buffer/latency sizing's actual worst
    // case once the multiplier can move halfWidth past one full period.
    const int halfWidth = std::clamp ((int) std::llround (periodSamples * (double) grainWidthMultiplier), 1, maxHalfWidthSamples);

    // The actual pitch-shift mechanism: READ position and WRITE position
    // are deliberately *different*. Content always comes from the most
    // recently completed analysis mark at or before this synthesis mark
    // (a grid of original-period-spaced positions, floor-aligned rather
    // than nearest — see the .cpp-level comment in shiftPitch() for why
    // floor, not round, keeps the lookahead/latency bound tight). When
    // synthesis marks are closer together than the analysis grid (pitch
    // shift up), several consecutive synthesis marks land in or near the
    // same analysis bucket and reuse that grain's content at closer
    // spacing; when they're farther apart (shift down), some analysis
    // buckets never get used at all. Reusing/skipping content at a
    // different spacing than it was recorded at is what changes pitch.
    //
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
    const double readMarkPosA = std::floor (synthesisMarkPos / periodSamples + 1.0e-6) * periodSamples;
    const long long readCenterA = (long long) std::llround (readMarkPosA);
    const long long writeCenter = (long long) std::llround (synthesisMarkPos);

    // Known, previously-accepted limitation, now being revisited: snapping
    // to a single bucket means source content jumps discretely every time
    // the synthesis position crosses a bucket boundary, audible on real
    // (non-stationary) audio as crackle plus a low-frequency beat — see
    // docs/FINDINGS.md #19/#20. #19's first cross-fade attempt blended the
    // two nearest buckets at their *nominal* alignment and didn't fix it:
    // two content snapshots exactly one period apart are close but not
    // identical for genuine voice (natural cycle-to-cycle jitter), so a
    // plain blend is closer to summing two slightly-phase-shifted copies
    // of a similar waveform, which produces frequency-dependent
    // cancellation (comb filtering) that sweeps as the misalignment
    // changes. This version searches for the best small-offset alignment
    // of bucket B against bucket A first (searchAlignmentOffset(), same
    // NCC + hard-margin pattern as WSOLATimeStretcher's splice search)
    // before blending — the fix #20 itself identifies as the real one.
    const double weightB = std::clamp ((synthesisMarkPos - readMarkPosA) / periodSamples, 0.0, 1.0);
    long long readCenterB = readCenterA + (long long) std::llround (periodSamples);

    if (weightB > 1.0e-6)
        readCenterB += searchAlignmentOffset (readCenterA, readCenterB, halfWidth);

    for (int k = -halfWidth; k <= halfWidth; ++k)
    {
        const long long dstAbs = writeCenter + (long long) k;
        if (dstAbs < 0)
            continue;
        const size_t accSlot = (size_t) (dstAbs % (long long) accumulator.size());
        const float windowValue = grainWindow[(size_t) (k + halfWidth)];

        const long long srcAbsA = readCenterA + (long long) k;
        const float weightA = (float) (1.0 - weightB);
        if (srcAbsA >= 0 && weightA > 1.0e-6f)
        {
            const size_t historySlotA = (size_t) (srcAbsA % (long long) history.size());
            accumulator[accSlot] += history[historySlotA] * windowValue * weightA;
            accumulatorWeight[accSlot] += windowValue * weightA;
        }

        if (weightB > 1.0e-6)
        {
            const long long srcAbsB = readCenterB + (long long) k;
            if (srcAbsB >= 0)
            {
                const size_t historySlotB = (size_t) (srcAbsB % (long long) history.size());
                accumulator[accSlot] += history[historySlotB] * windowValue * (float) weightB;
                accumulatorWeight[accSlot] += windowValue * (float) weightB;
            }
        }
    }
}

void PSOLAPitchShifter::shiftPitch (float detectedHz, float semitoneShift,
                                     const std::vector<float>& input, std::vector<float>& output)
{
    updatePeriodEstimate (detectedHz);

    // Precomputed once per call (not per grain): periodSamples and
    // grainWidthMultiplier are both fixed for the whole call already
    // (locally-stationary-pitch simplification documented in the class
    // doc — the multiplier only ever changes between shiftPitch() calls,
    // via setGrainWidthMultiplier()), so every grain fired during this
    // call uses the identical window. Must match placeGrainAt()'s own
    // halfWidth computation exactly (same formula, same members) — they
    // stay in sync because both derive from the same periodSamples/
    // grainWidthMultiplier state, not because either calls the other.
    const int halfWidth = std::clamp ((int) std::llround (periodSamples * (double) grainWidthMultiplier), 1, maxHalfWidthSamples);
    grainWindow.assign ((size_t) (2 * halfWidth + 1), 0.0f);
    for (int k = 0; k < (int) grainWindow.size(); ++k)
        grainWindow[(size_t) k] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) k / (float) (grainWindow.size() - 1));

    const double shiftRatio = std::pow (2.0, (double) semitoneShift / 12.0);
    const double synthesisSpacing = periodSamples / shiftRatio;

    for (size_t i = 0; i < input.size(); ++i)
    {
        history[(size_t) (totalSamplesIn % (long long) history.size())] = input[i];
        ++totalSamplesIn;

        // A mark is eligible once its full required read reach is
        // entirely within already-written history — this is the only
        // place lookahead is actually spent. That reach is larger than
        // just "mark ± halfWidth" now that placeGrainAt() also blends
        // bucket B (see getLatencySamples()'s doc for the full
        // derivation): one more period out to bucket B's own center,
        // bucket B's own half-width, and the alignment search margin.
        // Uses *this call's* actual periodSamples/halfWidth (not the
        // worst-case maxForwardReachSamples), the same tightness
        // convention halfWidth itself already used here before this
        // change — still a safe, if occasionally slightly conservative,
        // bound, for the same reason the original single-term version
        // was: placeGrainAt's floor-based bucket A read position is
        // always <= the mark's own position, and bucket B's search can
        // only move its start by at most psolaAlignSearchRadius.
        const double forwardReach = periodSamples + 2.0 * (double) halfWidth + (double) psolaAlignSearchRadius;
        while (nextMarkPos + forwardReach <= (double) (totalSamplesIn - 1))
        {
            placeGrainAt (nextMarkPos);
            nextMarkPos += synthesisSpacing;
        }

        // Fixed-delay-line read: position (totalSamplesIn - 1 -
        // latencySamples) is guaranteed safe because latencySamples uses
        // the worst-case (longest possible) forward reach
        // (maxForwardReachSamples), and no mark can ever reach farther
        // than this call's actual forwardReach from wherever nextMarkPos
        // currently is.
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
