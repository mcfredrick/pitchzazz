#pragma once

#include "Corrector.h" // CorrectionResult, StageTimings, hzToMidi/midiToHz
#include "PSOLAPitchShifter.h"
#include "PitchDetector.h"
#include "RetuneSmoothing.h"
#include "Scale.h"
#include <vector>

namespace pitchzazz
{

/// Same detect -> quantize -> shift orchestration as Corrector, with
/// PSOLAPitchShifter in the shift stage instead of the phase-vocoder
/// PitchShifter. Deliberately a separate, near-duplicate class rather
/// than templatizing Corrector over the shifter type: PSOLA's shift stage
/// needs the detected Hz as an extra input the phase vocoder never did
/// (see PSOLAPitchShifter.h), which would force an interface change onto
/// the existing, already-tested Corrector/PitchShifter pair for a benefit
/// that only pays off once there's a third shifter — not worth the risk
/// to working code for two call sites. hzToMidi/midiToHz/StageTimings/
/// CorrectionResult are shared from Corrector.h rather than duplicated.
///
/// Not real-time-safe to call directly from processBlock — same
/// constraint as Corrector, same reason (a full DSP pass too costly for
/// the hard audio-thread deadline; see Corrector.h's doc for why that's
/// no longer "because it allocates"). Lives on CorrectorWorker's
/// background thread.
class PSOLACorrector
{
public:
    PSOLACorrector (int blockSize, double sampleRate, Scale scale);

    void setScale (Scale newScale) noexcept { scale = newScale; }

    /// Same controls, same formulas, same clamping as Corrector's
    /// equivalent setters — see RetuneSmoothing.h. Duplicated per this
    /// class's own doc comment above (a near-duplicate of Corrector by
    /// design, not templatized), but the *formulas themselves* live in one
    /// shared header specifically so amount/speed can't drift out of sync
    /// between the two engines even though the orchestration around them
    /// does.
    void setCorrectionAmount (float amount) noexcept { correctionAmount = juce::jlimit (correctionAmountMin, correctionAmountMax, amount); }
    void setRetuneSpeedMs (float speedMs) noexcept { retuneSpeedMs = juce::jlimit (retuneSpeedMsMin, retuneSpeedMsMax, speedMs); }

    /// PSOLA-only "grain width" creative control (docs/ROADMAP.md Phase 5,
    /// item 2) — a thin forward to the shifter, which owns the actual
    /// clamping/buffer-sizing implications (PSOLAPitchShifter.h). No
    /// phase-vocoder equivalent exists (Corrector has no matching setter)
    /// since a phase vocoder has no concept of "grain."
    void setGrainWidthMultiplier (float multiplier) noexcept { shifter.setGrainWidthMultiplier (multiplier); }

    /// `samples.size()` and `output.size()` must both equal the
    /// `blockSize` passed to the constructor — see Corrector::process's
    /// equivalent doc for why `output` is caller-owned scratch storage
    /// rather than a returned, freshly-allocated buffer.
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output);

    [[nodiscard]] int getLatencySamples() const noexcept { return shifter.getLatencySamples(); }

private:
    PitchDetector detector;
    PSOLAPitchShifter shifter;
    int blockSize;
    Scale scale;

    float correctionAmount = correctionAmountMax;
    float retuneSpeedMs = retuneSpeedMsMin;
    float previousAppliedShift = 0.0f;
};

} // namespace pitchzazz
