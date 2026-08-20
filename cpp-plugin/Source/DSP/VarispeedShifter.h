#pragma once

#include "RetuneSmoothing.h"
#include "VarispeedResampler.h"
#include "WSOLATimeStretcher.h"
#include <vector>

namespace pitchzazz
{

/// Not a free musical choice — this bounds VarispeedResampler's ratio
/// range, and unlike PSOLA's grainWidthMultiplierMax (docs/ROADMAP.md
/// Phase 5), it does *not* affect getLatencySamples() (see this class's
/// doc on why WSOLA-then-resample keeps latency ratio-independent). It's
/// purely a buffer-margin safety bound: VarispeedResampler's/
/// WSOLATimeStretcher's fixed-capacity scratch buffers need *some* upper
/// bound on how far a single shiftPitch() call can push them, since
/// neither engine has PSOLA's natural pitch-range clamp (minHz/maxHz) to
/// borrow one from. ±24 semitones (two octaves) comfortably covers this
/// project's actual use case (vocal pitch correction) with real margin.
constexpr float varispeedMaxAbsSemitoneShift = 24.0f;

/// The shift stage of the Varispeed engine: stretches the original signal
/// with WSOLA (pitch-preserving, duration-changing), then resamples the
/// stretched result (duration-restoring, pitch-changing) — in that order,
/// not the reverse. Both stages advance by the *same* ratio
/// (2^(semitoneShift/12)): WSOLA stretches original-domain audio longer
/// by that ratio, then the resampler reads it back faster by the same
/// ratio, which shrinks it back to (approximately) the original duration
/// while raising pitch by exactly that ratio — net effect: pitch shifted,
/// timing preserved, formants shifted along with pitch (the deliberate
/// tape/vinyl character, not a defect — see VarispeedResampler's doc).
///
/// This ordering — stretch first, resample last — is the one real
/// architectural decision this engine makes differently from a naive
/// "resample then stretch" pipeline: resampling first would mean WSOLA
/// operates on an already time-compressed-or-expanded signal, so its own
/// fixed-sample lookahead would translate to a *ratio-scaled* amount of
/// real-world latency (worse the further you shift). Stretching the
/// original signal first means WSOLA's lookahead stays exactly what
/// WSOLATimeStretcher::getLatencySamples() already documents it as:
/// fixed, sample-rate-derived, ratio-independent. The tape/vinyl
/// character survives unchanged either way, since resampling is still the
/// very last operation applied in both orderings — only the latency
/// story differs.
///
/// Same "always take and return exactly blockSize samples" contract as
/// PitchShifter/PSOLAPitchShifter, achieved the same way they achieve it
/// — internal buffering (here, the two owned stages' own internal FIFOs)
/// absorbs the fact that both stages naturally produce a variable number
/// of samples per call; any shortfall is filled with silence, the same
/// "not warmed up yet" behavior PSOLAPitchShifter's readPos < 0 case uses
/// during its own initial latency fill-up.
///
/// Not real-time-safe to construct (allocates both owned stages' buffers
/// once); shiftPitch() itself doesn't allocate — see both stages' own
/// docs for confirmation neither push()/pull() does either.
class VarispeedShifter
{
public:
    explicit VarispeedShifter (double sampleRate);

    /// `input`/`output` must be the same length. `semitoneShift` is
    /// clamped to ±varispeedMaxAbsSemitoneShift before use — see that
    /// constant's doc for why this class needs its own bound rather than
    /// borrowing one the way PSOLA borrows from its pitch-range clamp.
    void shiftPitch (float semitoneShift, const std::vector<float>& input, std::vector<float>& output);

    /// Sum of both stages' own fixed, ratio-independent latencies — see
    /// this class's doc for why that independence is the whole point of
    /// the stretch-then-resample ordering.
    [[nodiscard]] int getLatencySamples() const noexcept
    {
        return stretcher.getLatencySamples() + resampler.getLatencySamples();
    }

private:
    double sampleRate;
    WSOLATimeStretcher stretcher;
    VarispeedResampler resampler;

    // Fast per-sample gain ramp toward 1.0 when real (resampled) output
    // is available and toward 0.0 when it runs dry — see the .cpp for
    // why this is needed at all: this pipeline's own priming/warm-up
    // period would otherwise transition from real audio straight to a
    // hard-zeroed tail *mid-block*, an audible-scale cliff, not the
    // graceful taper PitchShifter/PSOLAPitchShifter get for free from
    // their windowed synthesis. A few milliseconds, not a musical
    // control — this is click suppression, not a creative parameter, so
    // unlike RetuneSmoothing.h's retuneSpeedMs it isn't user-facing or
    // clamped to a musically-motivated range.
    static constexpr float outputGainRampMs = 3.0f;
    float outputGain = 0.0f;
};

} // namespace pitchzazz
