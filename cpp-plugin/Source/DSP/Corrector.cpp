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

    const auto shiftStart = std::chrono::steady_clock::now();
    result.samples.assign (samples.size(), 0.0f);
    shifter.shiftPitch (overSampling, semitoneShift, samples, result.samples);
    result.timings.shiftUs = microsSince (shiftStart);

    result.detectedHz = pitch.frequencyHz;
    result.detectedClarity = pitch.clarity;
    result.semitoneShift = semitoneShift;

    return result;
}

} // namespace pitchzazz
