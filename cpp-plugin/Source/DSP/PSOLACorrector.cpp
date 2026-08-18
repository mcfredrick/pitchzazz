#include "PSOLACorrector.h"
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

PSOLACorrector::PSOLACorrector (int blockSizeIn, double sampleRate, Scale scaleIn)
    : detector (blockSizeIn), shifter (sampleRate), blockSize (blockSizeIn), scale (scaleIn)
{
}

CorrectionResult PSOLACorrector::process (const std::vector<float>& samples, double sampleRate)
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

    // See Corrector::process()'s equivalent block for the full reasoning
    // — identical formula, identical ordering, so the user-facing control
    // feels like the same knob on both engines.
    const float targetShift = applyCorrectionAmount (semitoneShift, correctionAmount);
    const float blockPeriodMs = (float) samples.size() / (float) sampleRate * 1000.0f;
    const float appliedShift = glideTowards (previousAppliedShift, targetShift, retuneSpeedMs, blockPeriodMs);
    previousAppliedShift = appliedShift;

    const auto shiftStart = std::chrono::steady_clock::now();
    result.samples.assign (samples.size(), 0.0f);
    shifter.shiftPitch (pitch.frequencyHz, appliedShift, samples, result.samples);
    result.timings.shiftUs = microsSince (shiftStart);

    result.detectedHz = pitch.frequencyHz;
    result.detectedClarity = pitch.clarity;
    result.semitoneShift = appliedShift; // the applied shift, not the raw target -- see Corrector::process()'s comment

    return result;
}

} // namespace pitchzazz
