#pragma once

#include <vector>

namespace pitchzazz
{

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
/// continuous signal) causes an audible crackle/low-frequency-beat
/// artifact on real (non-stationary) audio — see placeGrainAt()'s doc for
/// the mechanism, and docs/FINDINGS.md #19/#20 for the full story,
/// including a cross-fade attempt that was tried, tested, shipped, then
/// found by ear to not actually fix it (and cost real latency doing it) —
/// reverted rather than kept for a benefit that didn't materialize. Fixing
/// this properly needs correlation-based grain alignment before blending,
/// not just blending — a bigger change than this project's timeline
/// currently has room for; documented here so the gap is explicit rather
/// than discovered by surprise.
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

    /// A **fixed** delay-line tap (2 pitch periods at `minHz`, see the
    /// .cpp), not one that adapts to the currently detected pitch — sized
    /// from the worst case so it's always safely long enough, then used
    /// unconditionally regardless of what's actually playing. This means
    /// a higher detected pitch does *not* get a shorter real latency than
    /// a low one: the underlying per-grain lookahead math is pitch-
    /// dependent, but the reported/actual output delay isn't, since a
    /// single fixed number is what JUCE's setLatencySamples() needs and a
    /// host can't be told "it varies." Confirmed exact — not just an
    /// upper bound — by benchmarks/PSOLALatencyProbe.cpp's onset probe at
    /// several sample-rate/frequency combinations. Still meaningfully —
    /// just not dramatically — lower than the phase vocoder's fixed
    /// ~46-50ms window (docs/PERFORMANCE_LOG.md).
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

    std::vector<float> grainWindow; // recomputed once per shiftPitch() call, see updatePeriodEstimate()

    int latencySamples = 0;

    void updatePeriodEstimate (float detectedHz) noexcept;
    void placeGrainAt (double markPos);
};

} // namespace pitchzazz
