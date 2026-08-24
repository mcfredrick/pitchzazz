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
    : sampleRate (sampleRateIn), periodSamples (sampleRate / 150.0), // 150Hz: a reasonable default before any real pitch has been detected
      currentPeriod (periodSamples) // same initial default, so the very first call doesn't interpolate from an artificial value
{
    maxPeriodSamples = (int) std::ceil (sampleRate / (double) minHz);

    // Worst-case half-width across both the lowest detectable pitch and
    // the widest the grain-width multiplier can ever be set to — see the
    // header's doc on maxHalfWidthSamples. grainWidthMultiplierMax > 1.0
    // by construction (see PSOLAPitchShifter.h), so this is always >=
    // maxPeriodSamples, never smaller.
    maxHalfWidthSamples = (int) std::ceil ((double) maxPeriodSamples * (double) grainWidthMultiplierMax);

    // Output is read from a fixed tap `latencySamples` behind the write
    // head — see shiftPitch()'s doc for the derivation: one grain
    // half-width of lookahead to fill a symmetric grain around a mark,
    // plus one more half-width before the accumulator slot a mark could
    // still touch has definitely been fully written (generalized from
    // "period" to "half-width" once grain width became a multiplier of
    // the period rather than always equal to it — at the 1.0x default
    // multiplier this reduces to exactly the original 2*maxPeriodSamples
    // formula). Confirmed exact (not just derived) at the 1.0x default by
    // benchmarks/PSOLALatencyProbe.cpp's onset probe across several
    // sample-rate/frequency combinations — matched to the sample at every
    // one once placeGrainAt()'s floor-based read position got an epsilon
    // guard (see that function's comment for the real bug that masked as
    // a latency-derivation problem for one specific combination before
    // that fix).
    latencySamples = 2 * maxHalfWidthSamples;

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

void PSOLAPitchShifter::placeGrainAt (double synthesisMarkPos, double readMarkPos)
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
    //
    // Known, accepted limitation: snapping to a single bucket means
    // source content jumps discretely every time the synthesis position
    // crosses a bucket boundary, audible on real (non-stationary) audio
    // as crackle plus a low-frequency beat — see docs/FINDINGS.md #19/#20
    // for the full diagnosis and listening results, and #32 for a
    // follow-up finding that this was a much larger-magnitude effect than
    // originally characterized (fixed at the call site, see
    // readMarkPosAccumulator's doc in the header) — this bucket-jump
    // limitation itself (content genuinely reused/skipped one whole
    // period at a time) is unchanged and still a known limitation; #32
    // fixed the *numerical instability* in how the bucket boundary was
    // located, not this underlying reuse/skip behavior. A cross-fade
    // between the two nearest buckets was implemented, tested, and
    // shipped, then found by ear (not by any automated test — three were
    // tried and none discriminated the change) to not fix it, and
    // reverted: two real content snapshots exactly one period apart are
    // close but not identical for genuine voice (natural cycle-to-cycle
    // jitter), so blending them isn't a clean cross-fade — it's closer to
    // blending two slightly-phase-shifted copies of a similar waveform,
    // which produces frequency-dependent cancellation (comb filtering)
    // that sweeps as the misalignment changes, plausibly explaining why
    // it didn't sound better and may have sounded worse. A real fix needs
    // correlation-based alignment of the two candidates before blending,
    // not blending alone — bigger scope, not attempted here.
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
    // Captured before updatePeriodEstimate overwrites periodSamples with
    // this call's target -- the interpolation start point for
    // synthesisSpacing's period term below (see currentPeriod's doc).
    updatePeriodEstimate (detectedHz);

    // First call: nothing real to interpolate from yet (see
    // hasReceivedFirstCall's doc) -- start already at target so this call
    // behaves exactly like the pre-interpolation formula did, rather than
    // ramping across a large, meaningless jump from the constructor's
    // placeholder defaults.
    const double periodAtBlockStart = hasReceivedFirstCall ? currentPeriod : periodSamples;

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

    // currentShift ramps linearly from wherever the *previous* call left it
    // to this call's semitoneShift target over the course of this call,
    // rather than snapping to semitoneShift as a single constant for the
    // whole block (docs/FINDINGS.md #32). A hard per-call constant means
    // shiftRatio (and therefore synthesisSpacing) takes a real, audible
    // step exactly at every block boundary -- confirmed by a block-size
    // sweep to imprint a periodic artifact at exactly the block rate
    // (e.g. 23.4Hz at blockSize=2048/48kHz, verified moving to 11.7Hz and
    // 46.9Hz at 4096 and 1024 respectively). Interpolating per-grain
    // instead of holding one value for the whole call removes that
    // discontinuity; `t` only needs mark-placement granularity (grains
    // fire every ~one period, far more often than block boundaries), not
    // per-sample granularity, to be smooth in practice.
    const float shiftAtBlockStart = hasReceivedFirstCall ? currentShift : semitoneShift;

    for (size_t i = 0; i < input.size(); ++i)
    {
        history[(size_t) (totalSamplesIn % (long long) history.size())] = input[i];
        ++totalSamplesIn;

        // A mark is eligible once its full grain (mark ± halfWidth, using
        // *this call's* actual halfWidth — not periodSamples, now that
        // grainWidthMultiplier can make those different) is entirely
        // within already-written history — this is the only place
        // lookahead is actually spent. Still correct with the read-position
        // accumulator below: readMarkPosAccumulator only ever commits whole
        // interpolatedPeriod-sized advances once readBucketProgress crosses
        // 1.0, the same "round down, never up" behavior the old floor()
        // formula had, so read position stays <= this synthesis mark's own
        // position and this remains a safe (if occasionally slightly
        // conservative) bound on the read requirement too.
        while (nextMarkPos + halfWidth <= (double) (totalSamplesIn - 1))
        {
            const float t = (float) i / (float) input.size();
            const float interpolatedShift = shiftAtBlockStart + (semitoneShift - shiftAtBlockStart) * t;
            const double shiftRatio = std::pow (2.0, (double) interpolatedShift / 12.0);
            // Both terms of synthesisSpacing interpolated across the call --
            // periodSamples (detectedHz-derived) steps once per call same as
            // shiftRatio did, and independently contributes to the same
            // block-rate artifact if left un-interpolated (docs/FINDINGS.md
            // #32).
            const double interpolatedPeriod = periodAtBlockStart + (periodSamples - periodAtBlockStart) * (double) t;
            const double synthesisSpacing = interpolatedPeriod / shiftRatio;

            // Stable, incremental read-position tracking (docs/FINDINGS.md
            // #32's PSOLA follow-up; full derivation in readMarkPosAccumulator's
            // header doc) -- replaces floor(synthesisMarkPos / periodSamples),
            // which divided an ever-growing absolute position by a divisor
            // that steps slightly every block. Uses *this* grain's already-
            // current accumulator value first (matching the old formula's
            // grain-0 behavior of reading from position 0), then advances the
            // accumulator afterward so the *next* grain sees the update --
            // advancing before use would put every read one full period
            // ahead of where it should be, silently breaking the read-
            // stays-behind-write invariant the whole latency design depends
            // on (found the hard way: QualityMetricsTests' unison-artifact-
            // energy checks caught it immediately when the order was
            // reversed). readBucketProgress accumulates this grain-step's
            // fraction of one original period (synthesisSpacing/
            // interpolatedPeriod, always small and bounded); each whole
            // period crossed commits exactly one interpolatedPeriod-sized
            // advance, reproducing the same discrete reuse/skip bucket
            // pattern the original formula intended. The tiny epsilon
            // guards the same boundary-precision edge case finding #18
            // already found in the old formula (a progress value that
            // "should" be exactly 1.0 landing a few ULPs under it from
            // repeated addition).
            placeGrainAt (nextMarkPos, readMarkPosAccumulator);

            readBucketProgress += synthesisSpacing / interpolatedPeriod;
            while (readBucketProgress >= 1.0 - 1.0e-6)
            {
                readMarkPosAccumulator += interpolatedPeriod;
                readBucketProgress -= 1.0;
            }

            nextMarkPos += synthesisSpacing;
        }

        // Fixed-delay-line read: position (totalSamplesIn - 1 -
        // latencySamples) is guaranteed safe because latencySamples uses
        // the worst-case (longest possible) half-width, and no mark can
        // ever reach farther back than halfWidth <= maxHalfWidthSamples
        // from wherever nextMarkPos currently is.
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

    currentShift = semitoneShift; // this call's target is the next call's interpolation start point
    currentPeriod = periodSamples; // same, for the period term
    hasReceivedFirstCall = true;
}

} // namespace pitchzazz
