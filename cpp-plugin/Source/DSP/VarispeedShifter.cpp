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
    const float ratio = std::pow (2.0f, clampedShift / 12.0f);

    stretcher.push (input.data(), (int) input.size());

    // Drain everything WSOLA currently has ready into the resampler — a
    // fixed-size stack scratch buffer, not a heap allocation, same
    // "fixed-size stack array in a hot loop" pattern pitch-cli's own
    // real-time callback uses for its downmix path (crates/pitch-cli/
    // src/main.rs). How much WSOLA has ready varies per call (it's a
    // variable-rate stage), which is exactly why this drains in a loop
    // until empty rather than assuming a fixed count.
    constexpr int scratchSize = 512;
    float scratch[scratchSize];
    int stretched;
    do
    {
        stretched = stretcher.pull (ratio, scratch, scratchSize);
        if (stretched > 0)
            resampler.push (scratch, stretched);
    } while (stretched > 0);

    // Resample stage both restores duration and applies the actual pitch
    // shift (see this class's header doc) — pull exactly output.size()
    // samples if available. Early on, before the combined pipeline has
    // buffered enough to satisfy its own latency, fewer are available.
    const int produced = resampler.pull (ratio, output.data(), (int) output.size());

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
    constexpr int fadeOutSamples = 128; // ~2.9ms @44.1kHz — a few ms, same order as outputGainRampMs
    const int fadeOutStart = std::max (0, produced - fadeOutSamples);
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
}

} // namespace pitchzazz
