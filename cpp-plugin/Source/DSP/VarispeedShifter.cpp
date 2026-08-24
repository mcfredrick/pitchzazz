#include "VarispeedShifter.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

VarispeedShifter::VarispeedShifter (double sampleRateIn)
    : sampleRate (sampleRateIn), stretcher (sampleRateIn)
{
}

void VarispeedShifter::shiftPitch (float semitoneShift, const std::vector<float>& input, std::vector<float>& output)
{
    const float clampedShift = std::clamp (semitoneShift, -varispeedMaxAbsSemitoneShift, varispeedMaxAbsSemitoneShift);

    // shiftAtBlockStart ramps from wherever the *previous* call left off to
    // this call's clampedShift target, the same interpolation pattern
    // PSOLAPitchShifter uses and for the same reason (docs/FINDINGS.md): a
    // single ratio value held constant for a whole call meant both owned
    // stages' internal read/analysis rate took a genuine step at every
    // call boundary, confirmed as this engine's dominant real-world
    // artifact by a causal block-size sweep. The first-ever call has
    // nothing real to interpolate from (currentShift is still the
    // constructor's placeholder 0), so it skips interpolation and starts
    // already at target -- same special case PSOLAPitchShifter's
    // hasReceivedFirstCall exists for.
    const float shiftAtBlockStart = hasReceivedFirstCall ? currentShift : clampedShift;
    const float ratioAtBlockStart = std::pow (2.0f, shiftAtBlockStart / 12.0f);
    const float ratioTarget = std::pow (2.0f, clampedShift / 12.0f);

    stretcher.push (input.data(), (int) input.size());

    // Drain everything WSOLA currently has ready into the resampler — a
    // fixed-size stack scratch buffer, not a heap allocation, same
    // "fixed-size stack array in a hot loop" pattern pitch-cli's own
    // real-time callback uses for its downmix path (crates/pitch-cli/
    // src/main.rs). How much WSOLA has ready varies per call (it's a
    // variable-rate stage), which is exactly why this drains in a loop
    // until empty rather than assuming a fixed count. Each sub-call's own
    // ratio range is the properly-scaled *segment* of the overall
    // ratioAtBlockStart -> ratioTarget ramp its own totalStretched
    // position falls in -- not (progress-so-far, ratioTarget) each time,
    // which would make the ramp complete anew within every ~512-sample
    // sub-call instead of once smoothly across the whole shiftPitch()
    // call (caught by the block-size sweep re-measuring no improvement
    // after the first version of this fix; docs/FINDINGS.md). Using
    // output.size() as the ramp's total span is an approximate progress
    // proxy (this stage's own output count isn't 1:1 with the final
    // output length), good enough since consecutive segments still chain
    // continuously end-to-start.
    constexpr int scratchSize = 512;
    float scratch[scratchSize];
    int stretched;
    int totalStretched = 0;
    do
    {
        const auto interpolatedRatio = [&] (int sampleOffset) noexcept
        {
            const float t = (float) sampleOffset / (float) std::max (1, (int) output.size());
            return ratioAtBlockStart + (ratioTarget - ratioAtBlockStart) * std::min (1.0f, t);
        };
        const float ratioSegmentStart = interpolatedRatio (totalStretched);
        const float ratioSegmentEnd = interpolatedRatio (totalStretched + scratchSize);
        stretched = stretcher.pull (ratioSegmentStart, ratioSegmentEnd, scratch, scratchSize);
        if (stretched > 0)
        {
            resampler.push (scratch, stretched);
            totalStretched += stretched;
        }
    } while (stretched > 0);

    // Resample stage both restores duration and applies the actual pitch
    // shift (see this class's header doc) — pull exactly output.size()
    // samples if available. Early on, before the combined pipeline has
    // buffered enough to satisfy its own latency, fewer are available.
    const int produced = resampler.pull (ratioAtBlockStart, ratioTarget, output.data(), (int) output.size());

    // A hard cut at exactly `produced` would jump directly from whatever
    // the waveform's instantaneous value was to silence — an audible-
    // scale click, confirmed by an earlier version of this method against
    // VarispeedShifterTests.cpp's startup-discontinuity test. A *reactive*
    // gain ramp alone doesn't fix it: by the time enough consecutive real
    // samples have arrived for the ramp to settle near 1.0, it's already
    // sitting at full gain with no warning before the boundary, so it
    // still can't respond in time. This needs to be anticipatory instead
    // — `produced` is already known before this loop runs, so the fade
    // toward silence is computed backward from that known boundary rather
    // than reacted to after crossing it. PitchShifter/PSOLAPitchShifter
    // don't need this at all: their own windowed synthesis already tapers
    // to near-zero at a grain's edges, so their silence transitions are
    // smooth by construction — this pipeline's raw resampled output has
    // no such taper, so one is applied explicitly.
    //
    // The reverse direction (fading back in once a later call produces a
    // full block again) doesn't need anticipation — nothing forces an
    // early jump the way the deficit boundary does, so the persistent,
    // purely reactive ramp already produces a smooth fade-in on its own,
    // picking up from wherever the anticipated fade-out above left it.
    // Only an actual, genuine shortfall this call (produced < requested)
    // needs the anticipatory fade at all -- a real bug found the same way
    // as the PSOLA/block-rate artifact above (an envelope-domain spectral
    // comparison showing a strong, unexplained signal exactly at the
    // block rate; docs/FINDINGS.md). The fadeOutStart computation below
    // was unconditional: in ordinary healthy operation `produced` reliably
    // equals `output.size()` every call once the pipeline is past its
    // initial latency fill, so `fadeOutStart = produced - fadeOutSamples`
    // still fell inside this call's own range and silently faded the last
    // ~128 samples of *every single block* toward silence, then reactively
    // ramped back up at the start of the next one -- an unconditional,
    // periodic amplitude dip at exactly the block rate, regardless of
    // whether there was ever a real deficit to anticipate. Gating on an
    // actual shortfall removes it: a full block now ramps toward 1.0 (or
    // sits there) for its entire length, no manufactured dip.
    constexpr int fadeOutSamples = 128; // ~2.9ms @44.1kHz — a few ms, same order as outputGainRampMs
    const bool hasShortfall = produced < (int) output.size();
    const int fadeOutStart = hasShortfall ? std::max (0, produced - fadeOutSamples) : (int) output.size();
    const float samplePeriodMs = 1000.0f / (float) sampleRate;

    for (int i = 0; i < (int) output.size(); ++i)
    {
        if (i < fadeOutStart)
        {
            outputGain = glideTowards (outputGain, 1.0f, outputGainRampMs, samplePeriodMs);
            output[(size_t) i] *= outputGain;
        }
        else if (i < produced)
        {
            const float t = (float) (i - fadeOutStart) / (float) std::max (1, produced - fadeOutStart);
            outputGain = 1.0f - t;
            output[(size_t) i] *= outputGain;
        }
        else
        {
            outputGain = 0.0f;
            output[(size_t) i] = 0.0f;
        }
    }

    currentShift = clampedShift; // this call's target is the next call's interpolation start point
    hasReceivedFirstCall = true;
}

} // namespace pitchzazz
