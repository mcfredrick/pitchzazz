#pragma once

#include <algorithm>
#include <vector>

namespace pitchzazz
{

// Bounds for the "grain width" creative control (docs/ROADMAP.md Phase 5's
// "Per-algorithm creative parameter exposure" item 2) — a multiplier on the
// grain half-width placeGrainAt() uses, which is normally locked to exactly
// one period. That fixed width is the actual mechanism by which PSOLA
// preserves formants while changing pitch (see placeGrainAt()'s doc: grain
// *spacing* sets perceived pitch, grain *width* sets the spectral
// envelope, and decoupling those is the whole point of the algorithm) — so
// widening or narrowing this moves deliberately away from that
// formant-preservation property in exchange for a different, more
// "granular"/textured character, the same kind of trade-off a granular
// synth's grain-size knob makes.
//
// The upper bound is NOT a free musical choice, unlike most of this
// codebase's other creative-control ranges — it directly sets
// getLatencySamples()'s fixed worst-case value (see that method's doc:
// latencySamples = 2 * ceil(maxPeriodSamples * grainWidthMultiplierMax),
// sized once at construction to safely cover every value this control
// could ever be set to, not just whatever it happens to be set to right
// now). A first pass at 3.0x was caught by
// tests/DSP/PSOLAPitchShifterTests.cpp's latency-formula test: it pushes
// worst-case latency to ~75ms at 44.1/48kHz, and 1.5x was chosen instead
// to keep worst-case latency at ~37.5ms — at the time, a real, meaningful
// reduction versus the phase vocoder's then-~42.7-46.4ms window
// (docs/ROADMAP.md's original framing for building TD-PSOLA at all: a
// latency win over the phase vocoder).
//
// CHANGED (2026-08-19): was 1.5. That framing no longer holds after
// PluginProcessor.h's windowSizeMs was independently tightened
// (50ms->30ms), cutting the phase vocoder to ~21-23ms and leaving PSOLA
// at 1.5x (~37.5ms) *higher*-latency than the engine it was originally
// built to beat — see docs/ALGORITHMS.md and docs/PERFORMANCE_LOG.md's
// 2026-08-19 entry for the full story. minHz (below) is a correctness
// floor tied to real bass-vocal fundamentals, not just a latency knob,
// so it isn't touched here. 1.25x brings worst-case latency to ~31ms —
// not lower than the phase vocoder's new number, but a real ~17% cut
// from 37.5ms — while still leaving the creative control room to widen
// the grain above its 1.0x default, unlike 1.0x (which would leave no
// "wider" range at all). Confirmed by ear, same caveat the 1.5x choice
// originally had: 1.25x sounds fine, and — not just a cautious middle
// value — going any lower audibly degrades, so this sits close to this
// control's actual perceptual floor, not merely a headroom-preserving
// compromise.
constexpr float grainWidthMultiplierMin = 0.5f;
constexpr float grainWidthMultiplierMax = 1.25f;

// Namespace-scope (not a PSOLAPitchShifter private member) for the same
// reason grainWidthMultiplierMax is: getLatencySamples()'s worst-case
// formula depends on it, and tests assert that formula explicitly rather
// than just "some small number" (PSOLAPitchShifterTests.cpp) — they need
// the authoritative constant, not a hardcoded duplicate of it. See
// PSOLAPitchShifter.h's own doc on searchAlignmentOffset() for what this
// bounds.
constexpr int psolaAlignSearchRadius = 32;

/// A theory-derived (not guessed) choice of grain-width multiplier for a
/// given shift, found from measuring artifact energy at real shift
/// amounts with docs/TESTING.md's QualityMetrics tooling and tracing the
/// mechanism it revealed: at a fixed multiplier, overlap between
/// consecutive grains equals `2 * multiplier * shiftRatio`
/// (grainWidth ~= 2*periodSamples*multiplier, synthesisSpacing =
/// periodSamples/shiftRatio) — at the default multiplier=1.0 and
/// shiftRatio=1 (unison, measured clean at 0.56% artifact energy), that's
/// the value 2. Solving `2 = 2*multiplier*shiftRatio` for multiplier
/// holds overlap at that same known-clean baseline for any upward shift:
/// `multiplier = 1/shiftRatio`.
///
/// Only applied above unison (shiftRatio > 1): overlap only grows past
/// the baseline in the upward direction (spacing shrinks while grain
/// width stays fixed) — downward shifts *reduce* overlap on their own
/// (spacing grows), and have no measured artifact-energy problem this
/// formula is meant to address, so this function leaves the multiplier
/// at its default there rather than acting on a mechanism the data
/// doesn't support changing.
///
/// Clamped to `grainWidthMultiplierMin` — the floor of the existing
/// shipped creative control, already covered by
/// PSOLAPitchShifterTests.cpp's own energy-bounds test at that exact
/// value, so this never asks the shifter to operate somewhere untested.
///
/// Deliberately a free function, not built into shiftPitch()/
/// PSOLACorrector::process() itself: `grainWidthMultiplier` is also a
/// user-facing GUI slider (docs/ROADMAP.md Phase 5's "Width" control),
/// and silently overriding that manual setting on every block would be a
/// real behavior change to an already-shipped control, not a pure
/// quality fix — wiring an automatic/manual interaction model is a
/// separate, deliberate decision for later, not assumed here. This
/// function exists so the choice can be tested and measured in
/// isolation (see QualityMetricsTests.cpp's before/after comparison)
/// without pre-deciding that question.
[[nodiscard]] float chooseGrainWidthMultiplierForShift (float semitoneShift) noexcept;

/// Time-Domain Pitch-Synchronous Overlap-Add (TD-PSOLA) pitch shifter —
/// the algorithm family real low-latency vocal-effects hardware/plugins
/// use (see docs/PERFORMANCE_LOG.md's SOTA-comparison entry), as opposed
/// to PitchShifter's phase vocoder. The two are deliberately NOT
/// interchangeable drop-ins: the phase vocoder needs only a shift ratio
/// (it manipulates phase in the frequency domain, agnostic to the actual
/// pitch); PSOLA is pitch-*synchronous* by construction — it has to know
/// the current period to place its analysis/synthesis marks at all, which
/// is why shiftPitch() takes `detectedHz` as well as `semitoneShift`
/// (PSOLACorrector passes through what its PitchDetector already found,
/// see PSOLACorrector.cpp).
///
/// Core idea, concretely: pull a short (~2 pitch periods, Hann-windowed)
/// grain out of the input at each of a series of "pitch marks," and
/// overlap-add those grains back together at a *different* spacing —
/// closer together for a pitch shift up, farther apart for a shift down.
/// Grain *spacing* controls perceived pitch; grain *width* (fixed at the
/// original, unshifted period) preserves the spectral envelope/formants
/// that spacing alone would otherwise distort — that decoupling is the
/// whole reason this algorithm exists instead of naive resampling.
///
/// Real time-domain PSOLA implementations place marks at genuine glottal-
/// closure instants (epoch detection). This one places marks at fixed
/// intervals of the current period estimate instead — a deliberate
/// simplification: within one ~46ms analysis block the pitch is already
/// assumed locally stationary (PitchDetector only reports one f0 per
/// block), so sub-block epoch tracking would add real complexity for
/// correctness this project can't otherwise verify (no epoch-accuracy
/// ground truth to test against). The trade-off costs a little
/// reconstruction quality on non-stationary pitch; it does not undermine
/// the actual point of building this engine, which is the *latency*
/// comparison against the phase vocoder — see getLatencySamples()'s doc.
///
/// Known, accepted limitation, not silently swept aside: reading source
/// content from a single nearest analysis position (rather than the true
/// continuous signal) caused an audible crackle/low-frequency-beat artifact
/// on real (non-stationary) audio — see placeGrainAt()'s doc for the
/// mechanism, and docs/FINDINGS.md #19/#20 for the full story, including a
/// first cross-fade attempt that blended the two nearest buckets *without*
/// aligning them first, shipped, then found by ear to not actually fix it.
/// `feature/psola-crackle-latency` (docs/PERFORMANCE_LOG.md's 2026-08-23
/// entry) revisits this with correlation-based alignment before blending —
/// the fix #20 itself identifies as the real one — at a real, disclosed
/// latency cost (see getLatencySamples()'s doc); not yet re-confirmed by
/// ear, only by DSP reasoning and the existing automated test suite, since
/// no automated metric tried so far (four attempts now) discriminates this
/// artifact.
///
/// Not real-time-safe to construct (allocates its buffers once); shiftPitch()
/// itself doesn't allocate — same shape as PitchShifter.
class PSOLAPitchShifter
{
public:
    explicit PSOLAPitchShifter (double sampleRate);

    /// `detectedHz` is the *current, pre-shift* detected pitch (0 if
    /// unvoiced/silent — the period estimate is held at its last known
    /// value in that case, same "keep the pipeline warm through silence"
    /// choice CorrectorWorker's hot-swap crossfade already relies on
    /// elsewhere). `semitoneShift` is signed, same convention as
    /// PitchShifter. `input`/`output` must be the same length.
    void shiftPitch (float detectedHz, float semitoneShift,
                      const std::vector<float>& input, std::vector<float>& output);

    /// Grain half-width multiplier (docs/ROADMAP.md Phase 5) — clamped
    /// here (not just at the UI layer) so this class's own invariants hold
    /// regardless of caller discipline, same "validate at the boundary you
    /// own" convention Corrector::setCorrectionAmount uses. Takes effect on
    /// the *next* shiftPitch() call, same as the period estimate itself —
    /// there's no separate "pending" mechanism needed inside this class,
    /// since nothing here runs concurrently with it (CorrectorWorker calls
    /// setters and shiftPitch() from the same worker-thread call sequence).
    void setGrainWidthMultiplier (float multiplier) noexcept
    {
        grainWidthMultiplier = std::clamp (multiplier, grainWidthMultiplierMin, grainWidthMultiplierMax);
    }

    /// A **fixed** delay-line tap — sized from the worst case across every
    /// axis that affects it, not one that adapts to the currently detected
    /// pitch, the currently set grain-width multiplier, or how much a
    /// given mark's cross-fade weight toward bucket B happens to be — a
    /// host needs one constant number, not one that varies block to block.
    ///
    /// CHANGED (2026-08-23, `feature/psola-crackle-latency`): placeGrainAt()
    /// now blends *two* analysis buckets (the correlation-aligned crackle
    /// fix, see that method's doc), which needs the farther bucket's
    /// content available too — one more period of reach, plus the
    /// alignment search radius, on top of the original single-bucket
    /// derivation. `maxForwardReachSamples` is the worst-case distance
    /// past a mark's own write position that its required read content can
    /// now extend: `maxPeriodSamples + 2*maxHalfWidthSamples +
    /// psolaAlignSearchRadius` (bucket A's own half-width reach, bucket B's
    /// half-width reach, the extra period to reach bucket B's *start*, and
    /// the alignment search margin).
    ///
    /// `latencySamples = maxForwardReachSamples + maxHalfWidthSamples` —
    /// **not** `2 * maxForwardReachSamples` (an earlier version of this
    /// derivation used that, and it was a real over-margin, not just extra
    /// caution: see the constructor's comment for the exact proof). The
    /// original single-bucket formula (`2 * maxHalfWidthSamples`) is the
    /// same tight bound in disguise — it's what this formula degenerates
    /// to when `maxForwardReachSamples == maxHalfWidthSamples`, true
    /// before this change since a single-bucket mark's forward reach *is*
    /// just its own half-width. This is still a real, substantial latency
    /// increase versus the single-bucket baseline even at the tight bound
    /// (see docs/PERFORMANCE_LOG.md's 2026-08-23 entry for the measured
    /// before/after numbers) — a disclosed trade-off for the crackle fix,
    /// not a free win, and the reason this lives in its own branch pending
    /// a decision on whether the quality gain (itself unverified by any
    /// automated metric so far — same entry) is worth it.
    [[nodiscard]] int getLatencySamples() const noexcept { return latencySamples; }

private:
    double sampleRate;

    // Bounds the period estimate and sizes every buffer below — including
    // getLatencySamples()'s worst-case bound (2 periods at minHz), so this
    // is a direct latency/safety-margin tradeoff, not just a detection
    // range. 80Hz sits right at the bottom of a typical bass vocal's
    // fundamental (~80-150Hz) rather than comfortably below it (60Hz,
    // this project's original choice, had more margin but cost ~8.3ms of
    // worst-case latency for headroom this tool's actual use case — vocal
    // pitch correction, not general-purpose/instrument-agnostic shifting
    // — mostly doesn't need). A genuinely low bass note below 80Hz still
    // works correctly; it just falls back to the clamped worst-case
    // period like any out-of-range input already did before this change.
    // 1000Hz covers well past soprano/most instruments this project's
    // scale-quantization targets. Values outside this range get clamped,
    // not rejected — see updatePeriodEstimate().
    static constexpr float minHz = 80.0f;
    static constexpr float maxHz = 1000.0f;

    int maxPeriodSamples = 0; // sampleRate / minHz, rounded up

    // Worst-case grain half-width across both axes that affect it: the
    // lowest detectable pitch (maxPeriodSamples) *and* the widest the
    // grain-width multiplier can ever push it (grainWidthMultiplierMax).
    // Everything below that has to size for "the biggest grain this class
    // could ever place" (buffer sizes, latencySamples, the clamp in
    // shiftPitch()'s per-call halfWidth) uses this instead of
    // maxPeriodSamples directly, so raising the multiplier can never
    // exceed what was actually allocated for.
    int maxHalfWidthSamples = 0;

    // See getLatencySamples()'s doc: the worst-case distance past a mark's
    // own write position its required read content can now extend, once
    // placeGrainAt() reaches into a second (bucket B) analysis bucket.
    int maxForwardReachSamples = 0;

    // Grain half-width multiplier — see setGrainWidthMultiplier()'s doc.
    // 1.0 reproduces this class's original fixed-at-one-period behaviour
    // exactly, so it's the default rather than an edge of the range.
    float grainWidthMultiplier = 1.0f;

    // Circular history of recent input — needs to hold at least
    // 2*maxPeriodSamples (the widest possible grain) plus headroom for
    // whatever block size shiftPitch() is called with, so a mark that
    // becomes eligible mid-block never reads a slot this same call is
    // about to overwrite.
    std::vector<float> history;

    // Circular overlap-add accumulator (signal) and a parallel weight
    // accumulator (sum of window values contributing to each slot) so
    // output can be normalized by actual overlap density at drain time —
    // needed because pitch-up shifts increase how many grains overlap at
    // any given output position, which would otherwise get audibly
    // louder than a pitch-down shift for no reason but overlap count.
    std::vector<float> accumulator;
    std::vector<float> accumulatorWeight;

    // totalSamplesIn is the only running position counter needed — output
    // position is always totalSamplesIn - latencySamples (a fixed tap on a
    // delay line), not a second independently-advancing counter; see the
    // .cpp for why that's both simpler and provably safe.
    long long totalSamplesIn = 0;
    double nextMarkPos = 0.0; // absolute sample position of the next synthesis mark
    double periodSamples;     // current smoothed period estimate, in samples

    // Recomputed once per shiftPitch() call (see updatePeriodEstimate()),
    // sized to 2*halfWidth+1 for that call's actual halfWidth. Capacity is
    // reserved once in the constructor to the worst case (2*maxHalfWidth-
    // Samples+1) specifically so a grainWidthMultiplier change at runtime
    // — which can make halfWidth bigger than pitch alone ever demanded,
    // even after this vector has already "warmed up" at the 1.0x default —
    // can never trigger a reallocation on this class's non-real-time-but-
    // still-shouldn't-be-surprising worker thread.
    std::vector<float> grainWindow;

    int latencySamples = 0;

    // Fixed, not period-scaling -- same "fixed constant, not proportional"
    // idiom WSOLATimeStretcher's maxCorrelationLengthSamples already
    // established in this codebase (for the same reason: bounds the
    // per-grain search cost regardless of how wide a low-pitch grain
    // gets). psolaAlignSearchRadius (namespace scope, see its own doc)
    // covers realistic cycle-to-cycle jitter (a few percent of even a low
    // 80Hz/551-sample period) with margin.
    static constexpr int maxAlignCorrelationLength = 64;

    // Small-window NCC search for the offset that best aligns bucket B's
    // content to bucket A's, before they're cross-faded together in
    // placeGrainAt() -- see that method's doc for why alignment has to
    // happen first (docs/FINDINGS.md #20). Same hard-minimum-improvement
    // pattern as WSOLATimeStretcher::searchBestOffset, for the identical
    // reason: a pure best-correlation search has a degenerate trivial
    // solution (offset 0 already correlates well against itself-ish
    // content on any near-periodic material), so nominal has to be beaten
    // substantially, not just edged out, before being allowed to move.
    [[nodiscard]] int searchAlignmentOffset (long long refCenter, long long candCenter, int halfWidth) const noexcept;

    void updatePeriodEstimate (float detectedHz) noexcept;
    void placeGrainAt (double markPos);
};

} // namespace pitchzazz
