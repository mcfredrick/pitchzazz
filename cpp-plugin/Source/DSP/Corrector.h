#pragma once

#include "PitchDetector.h"
#include "PitchShifter.h"
#include "RetuneSmoothing.h"
#include "Scale.h"
#include <vector>

namespace pitchzazz
{

/// Standard equal-temperament Hz -> fractional MIDI note number, A4 = 69 = 440Hz.
[[nodiscard]] float hzToMidi (float freqHz) noexcept;

/// Inverse of hzToMidi.
[[nodiscard]] float midiToHz (float midi) noexcept;

/// Minimum NSDF clarity `Corrector`/`PSOLACorrector`/`VarispeedCorrector`
/// each require before accepting a block's `PitchDetector::detect()`
/// result into the correction pipeline at all — see docs/FINDINGS.md #31.
/// The shared McLeod/NSDF detector occasionally locks onto the
/// 2nd-harmonic NSDF peak instead of the true fundamental (a well-known
/// autocorrelation-family failure mode); on real vocal content this
/// showed up as an octave-doubled `detectedHz` for one to a few
/// consecutive blocks, which (since all three correctors derive
/// `semitoneShift` from `detectedHz` the same way) biased the applied
/// shift by roughly an octave and produced an audible artifact on
/// TD-PSOLA and Varispeed (the phase vocoder's heavily-overlapped
/// frequency-domain shift smooths the same bad input into inaudibility —
/// see finding #31's comparison — so this fix is about correctness, not
/// just those two engines' audible symptom).
///
/// 0.5, not the detector's own internal `clarityThreshold` (0.1,
/// `PitchDetector.h`) — that threshold gates "is there a pitch here at
/// all" (silence/noise rejection), a different, lower bar than "is this
/// specific peak trustworthy." Confirmed by measurement, not guessed: a
/// clarity histogram across a full real vocal clip's 410 voiced blocks
/// (`benchmarks/PitchDetectorOctaveProbe.cpp`) is sharply bimodal — 386
/// blocks (94%) at 0.8-1.0, a legitimate softer tail of 11 blocks at
/// 0.6-0.8, then a near-empty gap at 0.5-0.6 (1 block) before a sparse
/// 0.1-0.5 scatter of 12 blocks that's almost entirely harmonic-lock
/// glitches (including every block of the diagnosed octave-doubling
/// event). 0.5 sits in that gap.
constexpr float detectedHzClarityAcceptThreshold = 0.5f;

/// A second, independent gate on accepting a block's `detectedHz`,
/// implemented and shipped on top of `detectedHzClarityAcceptThreshold`,
/// then reverted the same session (docs/FINDINGS.md #32's revert note) —
/// left defined, not deleted, since the characterization itself is still
/// real and useful if a differently-shaped fix for the same residual
/// glitch is attempted later (same "keep the measured property, drop only
/// the premature wiring" precedent as finding #27's grain-width formula).
/// The motivating problem was real: a spurious ~7.5kHz reading (real
/// content ~34x lower) slipped through the clarity gate at 0.51-0.64,
/// just above threshold, producing an audible residual crackle even
/// after #31 shipped. This gate rejected it correctly in isolation (a
/// real, continuously-voiced human fundamental cannot jump 12+ semitones
/// within a single ~46ms detection block, no matter how "confident" the
/// detector claims to be), but a sample-level diff plus the user's own
/// A/B listening showed that rejecting a reading doesn't just fix that
/// one block: PSOLA's grain-read position is a stateful, path-dependent
/// accumulator that never resyncs, so the rejection sent it down a
/// different phase trajectory for the *rest* of the render, changing 17.5
/// of 22 seconds of output and making the whole clip sound worse, not
/// just those two spots better. Likely applies to Varispeed's and the
/// phase vocoder's own stateful shifters too, not verified separately.
/// A real fix for this residual needs to correct the reading *without*
/// perturbing the accumulator's trajectory, which this gate's simple
/// reject-and-hold approach doesn't do.
constexpr float detectedHzMaxPlausibleJumpSemitones = 12.0f;

/// Signed cents offset of `freqHz` from the nearest chromatic
/// (equal-tempered) MIDI note — always in [-50, +50] by construction of
/// nearest-integer rounding, never a display clamp. Deliberately measured
/// against the nearest *chromatic* note, not `Corrector::process()`'s
/// scale-corrected target: that target can be several hundred cents away
/// whenever the nearest chromatic note isn't in the active Scale
/// (semitoneShift), which would make a "how in tune is the input" reading
/// pin at an extreme for most out-of-scale content instead of showing
/// genuine intonation. Lives alongside hzToMidi/midiToHz rather than in
/// the GUI layer so it's covered by this crate's own unit tests instead
/// of being untested GUI-only arithmetic — used by the editor's tuner-
/// style cents meter (docs/ROADMAP.md Phase 5).
float centsOffsetFromNearestNote (float freqHz) noexcept;

/// Wall-clock cost of each stage of one Corrector::process() call, in
/// microseconds. Mirrors pitch-core's StageTimings exactly — same field
/// names, same shape — so the Phase 3 Rust-vs-C++ benchmark comparison
/// (docs/ROADMAP.md) reports cost the same way on both sides.
struct StageTimings
{
    double detectUs = 0.0;
    double quantizeUs = 0.0;
    double shiftUs = 0.0;

    double totalUs() const noexcept { return detectUs + quantizeUs + shiftUs; }
};

struct CorrectionResult
{
    float detectedHz = 0.0f;
    float detectedClarity = 0.0f;
    float semitoneShift = 0.0f;
    StageTimings timings;
};

/// Detects pitch, snaps it to the nearest note in a Scale, and pitch-
/// shifts the block accordingly. Direct port of pitch-core's
/// PitchCorrector (crates/pitch-core/src/corrector.rs) — same stages,
/// same constants, same over-sampling factor. See `overSampling` below
/// for the current value and docs/PERFORMANCE_LOG.md for the measured
/// cost/latency data behind it — re-measured for this engine
/// specifically, not assumed to carry over from the Rust side's numbers.
///
/// Not real-time-safe to call directly from processBlock: this is a full
/// FFT-based DSP pass, too costly for the hard audio-thread deadline —
/// not because it allocates (it doesn't; `output` is caller-owned and
/// reused across calls, same as every other steady-state buffer in this
/// class, see docs/FINDINGS.md). Belongs on a worker thread, fed and
/// drained via lock-free queues — see docs/ARCHITECTURE.md and the
/// Day 3 task in docs/ROADMAP.md Phase 2.
class Corrector
{
public:
    Corrector (int blockSize, double sampleRate, int windowSizeMs, Scale scale);

    void setScale (Scale newScale) noexcept { scale = newScale; }

    /// The classic Auto-Tune "correction amount" / "retune speed" controls
    /// (docs/ROADMAP.md Phase 5) — see RetuneSmoothing.h for the formulas
    /// and the reasoning behind each. Clamped here (not just at the UI
    /// layer) so this class's own invariants hold regardless of caller
    /// discipline, matching this project's "validate at the boundary you
    /// own" convention.
    void setCorrectionAmount (float amount) noexcept { correctionAmount = juce::jlimit (correctionAmountMin, correctionAmountMax, amount); }
    void setRetuneSpeedMs (float speedMs) noexcept { retuneSpeedMs = juce::jlimit (retuneSpeedMsMin, retuneSpeedMsMax, speedMs); }

    /// `samples.size()` and `output.size()` must both equal the
    /// `blockSize` passed to the constructor — `output` is caller-owned
    /// scratch storage, written in place rather than returned, so a
    /// caller that keeps it alive across calls (like CorrectorWorker's
    /// persistent buffers) gets zero per-block heap allocation here.
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output);

    /// Algorithmic pipeline latency, in samples — delegates to the
    /// shifter, since that's the sole contributor (see
    /// PitchShifter::getLatencySamples's doc). Unaffected by
    /// correctionAmount/retuneSpeedMs above: both only change *how much*
    /// of the shift is applied and *how quickly* it's approached, not the
    /// shifter's own analysis window, which is what determines latency.
    [[nodiscard]] int getLatencySamples() const noexcept { return shifter.getLatencySamples(); }

private:
    PitchDetector detector;
    PitchShifter shifter;
    int blockSize;
    Scale scale;

    float correctionAmount = correctionAmountMax; // 1.0 = this project's original full-snap default
    float retuneSpeedMs = retuneSpeedMsMin;        // 0 = this project's original instant-snap default
    // Glide state: the shift actually applied last block, chased towards
    // this block's (amount-blended) target by glideTowards(). Starts at 0
    // rather than uninitialized/NaN so the very first block glides from
    // "no correction" rather than an undefined value.
    float previousAppliedShift = 0.0f;

    // Last-accepted (clarity >= detectedHzClarityAcceptThreshold)
    // detected pitch, held across low-confidence blocks — see that
    // constant's doc (docs/FINDINGS.md #31). Starts at 0, same "no
    // correction yet" convention previousAppliedShift's own initial value
    // already uses.
    float heldDetectedHz = 0.0f;

    // Controls the phase vocoder's STFT hop size (step = frameSize /
    // overSampling), not the analysis window itself — affects
    // reconstruction quality and CPU cost, not latency (confirmed
    // experimentally: latency was identical across 4/8/16/32/64 in
    // docs/PERFORMANCE_LOG.md's "OVER_SAMPLING re-evaluated" entry, which
    // has the full cost-vs-value data for both engines). Raised from 8 to
    // 16 there: cost scales ~linearly and stays cheap even at 64 (under
    // 6% of budget for this engine in Release), so affordability isn't
    // the constraint — diminishing quality returns past some point are,
    // which needs a listening test this project doesn't have tooling
    // for. 16 is a conservative, defensible interim value, not a claim
    // that it's optimal.
    static constexpr int overSampling = 16;
};

} // namespace pitchzazz
