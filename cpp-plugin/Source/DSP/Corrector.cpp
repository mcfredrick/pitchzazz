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

CorrectionResult Corrector::process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output)
{
    jassert ((int) samples.size() == blockSize);
    jassert ((int) output.size() == blockSize);

    CorrectionResult result;

    const auto detectStart = std::chrono::steady_clock::now();
    const DetectedPitch pitch = detector.detect (samples, sampleRate);
    result.timings.detectUs = microsSince (detectStart);

    const auto quantizeStart = std::chrono::steady_clock::now();

    // Gate acceptance on clarity before trusting this block's detection at
    // all -- see detectedHzClarityAcceptThreshold's doc (docs/FINDINGS.md
    // #31): a low-clarity reading is more likely the shared McLeod/NSDF
    // detector locking onto a harmonic than a real pitch, so it's ignored
    // in favor of the last trustworthy value rather than fed straight
    // into the correction pipeline.
    //
    // detectedHzMaxPlausibleJumpSemitones' second gate is deliberately NOT
    // applied here (docs/FINDINGS.md #32's revert note): confirmed harmful
    // for PSOLA (a stateful, path-dependent grain accumulator that never
    // resyncs after a rejection changes its trajectory) via a direct
    // sample-level diff and the user's own A/B listening. This engine's
    // own shifter is similarly stateful (a streaming STFT phase
    // accumulator), so the same risk applies even without separately
    // reproducing the measurement -- reverted here too rather than assumed
    // safe.
    if (pitch.frequencyHz > 0.0f && pitch.clarity >= detectedHzClarityAcceptThreshold)
        heldDetectedHz = pitch.frequencyHz;

    float semitoneShift = 0.0f;
    if (heldDetectedHz > 0.0f)
    {
        const float currentNote = hzToMidi (heldDetectedHz);
        const int targetNote = nearestInScaleMidi ((int) std::round (currentNote), scale);
        semitoneShift = (float) targetNote - currentNote;
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
    shifter.shiftPitch (overSampling, appliedShift, samples, output);
    result.timings.shiftUs = microsSince (shiftStart);

    result.detectedHz = heldDetectedHz; // the accepted value, not the raw (possibly-gated-out) reading -- see above
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
