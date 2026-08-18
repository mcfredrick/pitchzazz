#include "Corrector.h"
#include <chrono>
#include <cmath>

namespace pitchzazz
{

namespace
{
    double microsSince (std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count();
    }
}

float hzToMidi (float freqHz) noexcept
{
    return 69.0f + 12.0f * std::log2 (freqHz / 440.0f);
}

float midiToHz (float midi) noexcept
{
    return 440.0f * std::pow (2.0f, (midi - 69.0f) / 12.0f);
}

float centsOffsetFromNearestNote (float freqHz) noexcept
{
    const float midi = hzToMidi (freqHz);
    return (midi - std::round (midi)) * 100.0f;
}

Corrector::Corrector (int blockSizeIn, double sampleRate, int windowSizeMs, Scale scaleIn)
    : detector (blockSizeIn), shifter (windowSizeMs, sampleRate), blockSize (blockSizeIn), scale (scaleIn)
{
}

CorrectionResult Corrector::process (const std::vector<float>& samples, double sampleRate)
{
    jassert ((int) samples.size() == blockSize);

    CorrectionResult result;

    const auto detectStart = std::chrono::steady_clock::now();
    const DetectedPitch pitch = detector.detect (samples, sampleRate);
    result.timings.detectUs = microsSince (detectStart);

    const auto quantizeStart = std::chrono::steady_clock::now();
    // MIDI vocoder mode (docs/ROADMAP.md Phase 5): a held MIDI note
    // overrides scale-quantization as the target; MidiFallbackMode governs
    // what happens when none is held. remembered *before* branching so
    // holdLastNote can see "was a note held as of this call", not a stale
    // value from before this block updated midiTargetNote.
    const bool midiNoteHeld = midiTargetNote >= 0;
    if (midiNoteHeld)
        lastKnownMidiNote = midiTargetNote;

    float semitoneShift = 0.0f;
    if (pitch.frequencyHz > 0.0f)
    {
        const float currentNote = hzToMidi (pitch.frequencyHz);
        if (midiNoteHeld)
        {
            semitoneShift = (float) midiTargetNote - currentNote;
        }
        else if (midiFallbackMode == MidiFallbackMode::holdLastNote && lastKnownMidiNote >= 0)
        {
            semitoneShift = (float) lastKnownMidiNote - currentNote;
        }
        else if (midiFallbackMode == MidiFallbackMode::bypass || midiFallbackMode == MidiFallbackMode::silence)
        {
            // No correction while unheld — silence's actual muting happens
            // post-shift below (a gain ramp, not a shift-amount decision),
            // so this branch is identical to bypass here on purpose.
            semitoneShift = 0.0f;
        }
        else // scaleQuantize (the default), or holdLastNote before any note has ever been held
        {
            const int targetNote = nearestInScaleMidi ((int) std::round (currentNote), scale);
            semitoneShift = (float) targetNote - currentNote;
        }
        if (! std::isfinite (semitoneShift))
            semitoneShift = 0.0f;
    }
    result.timings.quantizeUs = microsSince (quantizeStart);

    // Correction amount scales *how far* to correct; retune speed then
    // glides the currently-applied shift towards that (amount-scaled)
    // target rather than jumping to it instantly — see RetuneSmoothing.h.
    // blockPeriodMs is derived from the actual `samples` passed in rather
    // than a cached constructor value: samples.size() is already asserted
    // to equal blockSize above, so this is just spelling that assumption
    // out at the point it's used, not a new assumption.
    const float targetShift = applyCorrectionAmount (semitoneShift, correctionAmount);
    const float blockPeriodMs = (float) samples.size() / (float) sampleRate * 1000.0f;
    const float appliedShift = glideTowards (previousAppliedShift, targetShift, retuneSpeedMs, blockPeriodMs);
    previousAppliedShift = appliedShift;

    const auto shiftStart = std::chrono::steady_clock::now();
    result.samples.assign (samples.size(), 0.0f);
    shifter.shiftPitch (overSampling, appliedShift, samples, result.samples);
    result.timings.shiftUs = microsSince (shiftStart);

    // Anti-click gain ramp for the "silence" fallback mode (see
    // RetuneSmoothing.h's silenceRampMs doc) — applied per sample, not
    // once per block, specifically so muting/unmuting never introduces a
    // block-boundary step in the waveform, the same class of artifact
    // docs/FINDINGS.md #14 already found once in this codebase's hot-swap
    // crossfade. The shift stage above always ran normally first, so this
    // only scales already-correct output — internal shifter state stays
    // warm through a mute exactly like it does through a crossfade.
    // Outside "silence" mode the target is always 1.0 (fully audible), so
    // this loop is a no-op cost-wise everywhere else, not a special case.
    const bool shouldBeSilent = (midiFallbackMode == MidiFallbackMode::silence) && ! midiNoteHeld;
    const float silenceTargetGain = shouldBeSilent ? 0.0f : 1.0f;
    const float samplePeriodMs = 1000.0f / (float) sampleRate;
    for (float& sample : result.samples)
    {
        silenceGain = glideTowards (silenceGain, silenceTargetGain, silenceRampMs, samplePeriodMs);
        sample *= silenceGain;
    }

    result.detectedHz = pitch.frequencyHz;
    result.detectedClarity = pitch.clarity;
    // The *applied* shift, not the raw full-snap target — this is what
    // actually reached the shifter, so it's what the GUI's "corrected
    // pitch" readout (PluginEditor.cpp) should reflect. With amount < 1 or
    // an in-progress glide, that's meaningfully different from "where the
    // scale wants to send it," and reporting the raw target here would
    // make the display lie about what the audio is actually doing.
    result.semitoneShift = appliedShift;

    return result;
}

} // namespace pitchzazz
