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

CorrectionResult PSOLACorrector::process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output)
{
    jassert ((int) samples.size() == blockSize);
    jassert ((int) output.size() == blockSize);

    CorrectionResult result;

    const auto detectStart = std::chrono::steady_clock::now();
    const DetectedPitch pitch = detector.detect (samples, sampleRate);
    result.timings.detectUs = microsSince (detectStart);

    const auto quantizeStart = std::chrono::steady_clock::now();

    // See Corrector::process()'s equivalent block for the full reasoning
    // (docs/FINDINGS.md #31) -- particularly important for this engine
    // specifically, since a rejected reading here also protects
    // shifter.shiftPitch()'s own detectedHz-derived grain period below,
    // not just the quantizer.
    //
    // detectedHzMaxPlausibleJumpSemitones' gate is deliberately NOT applied
    // here (docs/FINDINGS.md #32's revert note): PSOLA's grain-read
    // accumulator is stateful and path-dependent (see
    // readMarkPosAccumulator's doc), so rejecting a reading doesn't just
    // fix the rejected block in isolation -- it sends the accumulator down
    // a different phase trajectory that never resyncs, changing grain
    // placement for the rest of the render even after both paths agree on
    // the correct pitch again. Measured directly (sample-level diff, not
    // assumed): applying the gate here changed 17.5 of 22 seconds of
    // output despite the underlying detected-pitch stream matching
    // everywhere except the two rejected blocks. The user's own A/B
    // listening confirmed the gate made the whole clip sound worse, not
    // just those two spots better -- reverted on that evidence, clarity
    // alone stays PSOLA's only gate.
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

    // See Corrector::process()'s equivalent block for the full reasoning
    // — identical formula, identical ordering, so the user-facing control
    // feels like the same knob on both engines.
    const float targetShift = applyCorrectionAmount (semitoneShift, correctionAmount);
    const float blockPeriodMs = (float) samples.size() / (float) sampleRate * 1000.0f;
    const float appliedShift = glideTowards (previousAppliedShift, targetShift, retuneSpeedMs, blockPeriodMs);
    previousAppliedShift = appliedShift;

    const auto shiftStart = std::chrono::steady_clock::now();
    shifter.shiftPitch (heldDetectedHz, appliedShift, samples, output);
    result.timings.shiftUs = microsSince (shiftStart);

    result.detectedHz = heldDetectedHz; // the accepted value, not the raw (possibly-gated-out) reading -- see above
    result.detectedClarity = pitch.clarity;
    result.semitoneShift = appliedShift; // the applied shift, not the raw target -- see Corrector::process()'s comment

    return result;
}

} // namespace pitchzazz
