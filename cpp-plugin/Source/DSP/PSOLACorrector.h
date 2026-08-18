#pragma once

#include "Corrector.h" // CorrectionResult, StageTimings, hzToMidi/midiToHz
#include "PSOLAPitchShifter.h"
#include "PitchDetector.h"
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
/// constraint as Corrector, same reason (process() allocates its output
/// vector). Lives on CorrectorWorker's background thread.
class PSOLACorrector
{
public:
    PSOLACorrector (int blockSize, double sampleRate, Scale scale);

    void setScale (Scale newScale) noexcept { scale = newScale; }

    /// `samples.size()` must equal the `blockSize` passed to the constructor.
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate);

    [[nodiscard]] int getLatencySamples() const noexcept { return shifter.getLatencySamples(); }

private:
    PitchDetector detector;
    PSOLAPitchShifter shifter;
    int blockSize;
    Scale scale;
};

} // namespace pitchzazz
