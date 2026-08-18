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
    float semitoneShift = 0.0f;
    if (pitch.frequencyHz > 0.0f)
    {
        const float currentNote = hzToMidi (pitch.frequencyHz);
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
    result.samples.assign (samples.size(), 0.0f);
    shifter.shiftPitch (overSampling, appliedShift, samples, result.samples);
    result.timings.shiftUs = microsSince (shiftStart);

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
