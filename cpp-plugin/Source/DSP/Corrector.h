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

/// What a MIDI-targeted engine does when no MIDI note is currently held
/// (docs/ROADMAP.md Phase 5's "monophonic MIDI vocoder mode"). Antares
/// Auto-Tune's own documented "Target Notes Via MIDI" mode bypasses
/// entirely with no note held — a deliberate choice for its pre-timed,
/// offline-track use case, not necessarily the right default for this
/// project's live-mic framing, so all four options are exposed as a user
/// choice rather than picking one silently. Lives here (not PitchEngine.h,
/// where the setter that consumes it is declared) because PitchEngine.h
/// already includes this file — the reverse include would be circular.
enum class MidiFallbackMode
{
    scaleQuantize, // default: behaves exactly as if MIDI targeting didn't exist
    holdLastNote,  // keep correcting to the last-held note after release
    bypass,        // pass audio through with no correction
    silence        // mute output (via a short anti-click gain ramp, not a hard cut)
};

struct CorrectionResult
{
    std::vector<float> samples;
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
/// Not real-time-safe to call directly from processBlock: `process()`
/// allocates its output vector. Belongs on a worker thread, fed and
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

    /// Monophonic MIDI vocoder mode (docs/ROADMAP.md Phase 5) — see
    /// MidiFallbackMode's doc for what `midiFallbackMode` controls.
    /// `noteNumber` is clamped to a valid MIDI note or -1 ("none held"),
    /// same "validate at the boundary you own" convention as the setters
    /// above.
    void setMidiTargetNote (int noteNumber) noexcept { midiTargetNote = (noteNumber >= 0 && noteNumber <= 127) ? noteNumber : -1; }
    void setMidiFallbackMode (MidiFallbackMode mode) noexcept { midiFallbackMode = mode; }

    /// `samples.size()` must equal the `blockSize` passed to the constructor.
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate);

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

    int midiTargetNote = -1;                                    // -1 = no MIDI note currently held
    MidiFallbackMode midiFallbackMode = MidiFallbackMode::scaleQuantize;
    int lastKnownMidiNote = -1;                                 // for holdLastNote; -1 until a note has ever been held
    // Anti-click gain for the "silence" fallback mode — ramped per sample
    // toward 0 (muted) or 1 (audible) rather than cut, see
    // RetuneSmoothing.h's silenceRampMs doc for why. Starts at 1 (audible)
    // since the plugin shouldn't start muted before any MIDI has arrived.
    float silenceGain = 1.0f;

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
