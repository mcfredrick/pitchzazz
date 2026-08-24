#pragma once

#include "Corrector.h" // CorrectionResult, StageTimings, hzToMidi/midiToHz
#include "PitchDetector.h"
#include "RetuneSmoothing.h"
#include "Scale.h"
#include "VarispeedShifter.h"
#include <vector>

namespace pitchzazz
{

/// Same detect -> quantize -> shift orchestration as Corrector/
/// PSOLACorrector, with VarispeedShifter in the shift stage. A third
/// near-duplicate rather than a templatized shared base, for the same
/// reason PSOLACorrector's header doc gives: each shifter's actual
/// signature differs just enough (VarispeedShifter needs only
/// semitoneShift, no detected-Hz input at all, unlike PSOLA) that forcing
/// a shared interface onto three working, already-tested classes isn't
/// worth the risk for what it would save. hzToMidi/midiToHz/
/// StageTimings/CorrectionResult are shared from Corrector.h rather than
/// duplicated.
///
/// Not real-time-safe to call directly from processBlock — same
/// constraint as Corrector/PSOLACorrector, same reason (a full DSP pass
/// too costly for the hard audio-thread deadline; see Corrector.h's doc
/// for why that's no longer "because it allocates"). Lives on
/// CorrectorWorker's background thread.
class VarispeedCorrector
{
public:
    VarispeedCorrector (int blockSize, double sampleRate, Scale scale);

    void setScale (Scale newScale) noexcept { scale = newScale; }

    /// Same controls, same formulas, same clamping as Corrector's and
    /// PSOLACorrector's equivalent setters — see RetuneSmoothing.h.
    void setCorrectionAmount (float amount) noexcept { correctionAmount = juce::jlimit (correctionAmountMin, correctionAmountMax, amount); }
    void setRetuneSpeedMs (float speedMs) noexcept { retuneSpeedMs = juce::jlimit (retuneSpeedMsMin, retuneSpeedMsMax, speedMs); }

    /// `samples.size()` and `output.size()` must both equal the
    /// `blockSize` passed to the constructor — see Corrector::process's
    /// equivalent doc for why `output` is caller-owned scratch storage
    /// rather than a returned, freshly-allocated buffer.
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output);

    [[nodiscard]] int getLatencySamples() const noexcept { return shifter.getLatencySamples(); }

private:
    PitchDetector detector;
    VarispeedShifter shifter;
    int blockSize;
    Scale scale;

    float correctionAmount = correctionAmountMax;
    float retuneSpeedMs = retuneSpeedMsMin;
    float previousAppliedShift = 0.0f;

    // See Corrector.h's detectedHzClarityAcceptThreshold doc
    // (docs/FINDINGS.md #31) — held across low-confidence blocks.
    float heldDetectedHz = 0.0f;
};

} // namespace pitchzazz
