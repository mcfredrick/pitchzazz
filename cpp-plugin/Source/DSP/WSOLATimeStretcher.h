#pragma once

#include <vector>

namespace pitchzazz
{

/// Waveform Similarity Overlap-Add (WSOLA) time-stretcher — changes
/// duration while leaving pitch untouched, the mechanism this project's
/// Varispeed engine uses to restore original timing after its resample
/// stage has already shifted pitch (see VarispeedShifter's doc for why
/// that ordering, not the reverse, is what keeps this stage's own latency
/// independent of shift amount). Genuinely new algorithm family in this
/// codebase — unlike PSOLA, which is pitch-*synchronous* (it needs to
/// know the current period to place marks at all), WSOLA doesn't need a
/// pitch estimate: it finds good splice points by cross-correlating
/// waveforms directly, which is what makes it usable here on the
/// *original*, not-yet-pitch-detected-for-this-purpose audio.
///
/// Core idea: place fixed-length, Hann-windowed analysis windows onto the
/// input at a hop that's scaled by the stretch ratio, then overlap-add
/// them onto the output at a *fixed* hop — same "read at one spacing,
/// write at another" trick PSOLA uses for pitch, applied here to time
/// instead. The wrinkle WSOLA adds over plain fixed-hop overlap-add: each
/// analysis window's exact position is nudged by up to `searchRadius`
/// samples, choosing whichever nearby offset's waveform best matches the
/// tail of the previously placed window (highest normalized cross-
/// correlation over the overlap region) — this is what avoids an audible
/// phase discontinuity at the splice, and it's WSOLA's entire reason to
/// exist over naive fixed-hop overlap-add.
///
/// Window ≈25ms, Hann, hop = window/2 (50% overlap — the periodic-Hann
/// definition satisfies constant-overlap-add exactly at that ratio,
/// though this class still normalizes by an explicit weight accumulator
/// rather than relying on that being exact, the same defensive habit
/// PSOLAPitchShifter's accumulator/accumulatorWeight pair already has).
/// Deliberately a first pass, not a state-of-the-art implementation,
/// using plain normalized cross-correlation rather than a smarter/
/// cheaper similarity metric.
///
/// The search's correlation *length* is deliberately capped to a fixed
/// constant (maxCorrelationLengthSamples), not the full overlap region —
/// found the hard way, not designed in from the start: an earlier version
/// correlated the entire synthesisHopSamples-length overlap region, which
/// (since both search radius and correlation length scale with
/// windowSizeSamples, itself sampleRate-derived) made total search cost
/// scale as O(sampleRate²), not O(sampleRate). Invisible at 44.1kHz, but
/// measured live at 96kHz pushing the shift stage to ~26ms against a
/// ~21ms budget — a real, audible buffer underrun, not a theoretical
/// concern. Capping correlation length independent of sample rate turns
/// the cost back to O(searchRadiusSamples) — linear, matching how the
/// existing engines' own costs scale — since a short reference snippet is
/// sufficient to find a good splice offset; it doesn't need to cover the
/// entire overlap region to work.
///
/// Search radius is window/16, not the much wider window/4 an earlier
/// version used — also found via live testing, a separate bug from the
/// one above. A pure correlation-quality search has a fundamental,
/// not-implementation-specific failure mode on stationary/periodic
/// material: the *objectively best* correlation available anywhere in the
/// search window is trivially "match the immediately preceding window's
/// tail almost exactly" (near-zero lag against the reference), which
/// corresponds to advancing at the *fixed* synthesisHopSamples rate, not
/// the ratio-scaled target — i.e. the search has every incentive to
/// quietly stop stretching at all. Direct instrumentation on a sustained
/// 220Hz tone confirmed exactly this: the actual read position advanced
/// by precisely synthesisHopSamples every hop (not the intended,
/// smaller, ratio-scaled hop), drifting away from the true target until
/// the ~276-sample search radius was exhausted, forcing an abrupt
/// ~400-sample corrective jump — recurring every 9-13 hops, landing at
/// 6-9Hz, matching a periodic-discontinuity artifact heard by ear before
/// this fix. Reducing the radius alone is only a partial, complementary
/// fix (it bounds how far the degenerate solution can drift, it doesn't
/// stop it from being preferred) — the real fix is in searchBestOffset(),
/// documented there.
///
/// searchBestOffset() rejected two earlier approaches before landing on a
/// hard minimum-improvement threshold — worth recording, since the first
/// one *looked* like a fix (reduced the artifact) without actually being
/// one: a small penalty proportional to distance-from-nominal shrank the
/// drift range and the corrective-jump size, but a proportional penalty
/// is negligible right near offset 0, exactly where the degenerate
/// zero-lag preference actually originates — so it didn't stop the drift,
/// it just made each cycle smaller and, measured after that change,
/// *faster* (9-13 hops down to ~5), which is worse, not better: a fast
/// small-amplitude periodic correction fuses perceptually into continuous
/// buzz/roughness ("sounds like a fan") rather than occasional discrete
/// clicks. The actual fix (see searchBestOffset()) is a hard bar the
/// nominal position's own correlation score sets — verified afterward by
/// instrumenting the chosen offset per window across ±0.5 to ±12
/// semitones on the same sustained-tone signal: offset settles to a
/// stable constant (never oscillates) and zero output discontinuities
/// occur, versus the previous approach's continued periodic resets.
///
/// Known, accepted limitation, not silently swept aside (same disclosure
/// standard PSOLAPitchShifter's own crackle/beat artifact gets,
/// docs/FINDINGS.md #16/#17): a stationary pure tone is close to a worst
/// case for any correlation-based search, and this fix works by refusing
/// to deviate from nominal without substantial cause — on real voiced
/// material (natural jitter, harmonic content, formant movement), that
/// same threshold could still occasionally let a genuinely-better splice
/// point through and be audible as an isolated correction, just not as a
/// *periodic* one. A more complete fix would track phase continuity
/// explicitly rather than gating on a single fixed correlation margin —
/// bigger scope, not attempted here.
///
/// Streaming push/pull interface, same shape as VarispeedResampler and
/// for the same reason: this is one stage of a multi-stage variable-rate
/// pipeline (push rate and pull rate aren't 1:1, since this class changes
/// duration by design), so a fixed block-in-block-out contract doesn't
/// fit here the way it does for PitchShifter/PSOLAPitchShifter's
/// pitch-only shiftPitch().
///
/// Not real-time-safe to construct (allocates its buffers once);
/// push()/pull() themselves don't allocate.
class WSOLATimeStretcher
{
public:
    explicit WSOLATimeStretcher (double sampleRate);

    /// Appends newly-available original-domain input samples. Never
    /// allocates — history is a fixed-size circular buffer with generous
    /// headroom (see the .cpp), same "cheap in memory, avoids wraparound
    /// risk" trade-off this codebase's other circular buffers already make.
    void push (const float* input, int count);

    /// Produces up to `maxCount` time-stretched samples into `output` —
    /// pitch is unaffected; only duration changes. `ratio` > 1 stretches
    /// (output runs longer than the input that produced it — used here
    /// to *undo* a pitch-up resample's time compression); < 1 compresses.
    /// Linearly interpolated from `ratioStart` to `ratioEnd` across the
    /// call rather than held at one constant value, for the same reason
    /// VarispeedResampler::pull() interpolates now (docs/FINDINGS.md): a
    /// constant analysisHopSamples for a whole call meant window spacing
    /// took a genuine step at every call boundary. Interpolated per window
    /// placed (this class's own natural granularity), using how much of
    /// `maxCount` has been produced so far as the progress fraction.
    /// Returns the actual number of samples produced, which is less than
    /// `maxCount` whenever producing more would need an analysis window
    /// that reaches past what's been pushed so far — same "caller pushes
    /// more and retries" contract as VarispeedResampler::pull().
    int pull (float ratioStart, float ratioEnd, float* output, int maxCount);

    /// Fixed at windowSizeSamples + searchRadiusSamples — both are
    /// sampleRate-derived constants, not ratio-dependent, which is the
    /// entire point of stretching the *original* signal before resampling
    /// rather than after: this stage's lookahead can't grow with shift
    /// amount the way it would if it operated on an already-resampled
    /// (time-compressed-or-expanded) signal instead. See
    /// VarispeedShifter's latency doc for the full accounting.
    [[nodiscard]] int getLatencySamples() const noexcept { return windowSizeSamples + searchRadiusSamples; }

private:
    // ~25ms, Hann, rounded to even so synthesisHopSamples is exact and the
    // window is symmetric. Long enough to span multiple pitch periods
    // down to a typical vocal's fundamental without depending on knowing
    // the actual pitch (unlike PSOLA, which sizes its grain from a
    // detected period) — a fixed, content-agnostic window is what lets
    // this class skip pitch detection entirely.
    int windowSizeSamples = 0;
    int synthesisHopSamples = 0; // windowSizeSamples / 2, the fixed output-domain hop
    int searchRadiusSamples = 0; // windowSizeSamples / 4

    // Fixed, NOT sampleRate-derived — see the class doc for why this is
    // the specific fix for the O(sampleRate²) search-cost bug found via
    // live 96kHz testing. 256 samples is comfortably enough to distinguish
    // waveform similarity for splice alignment (multiple cycles even at a
    // low ~170Hz fundamental at 44.1kHz) without scaling up at higher
    // sample rates the way the full overlap region did.
    static constexpr int maxCorrelationLengthSamples = 256;
    int correlationLengthSamples = 0; // min(synthesisHopSamples, maxCorrelationLengthSamples)

    std::vector<float> hannWindow; // periodic definition, length windowSizeSamples

    // Circular history of pushed input, generously sized (see the .cpp) —
    // needs to hold at least one full search-and-window's worth of
    // lookahead beyond the nominal analysis position, plus real headroom
    // for bursty push cadence, same reasoning VarispeedResampler's history
    // buffer documents.
    std::vector<float> history;
    long long totalPushed = 0;

    // Circular overlap-add accumulator (signal) and parallel weight
    // accumulator, same normalize-by-actual-overlap-density purpose
    // PSOLAPitchShifter's accumulator/accumulatorWeight pair serves —
    // needed here because the search offset can make windows overlap by
    // slightly more or less than the nominal 50%.
    std::vector<float> accumulator;
    std::vector<float> accumulatorWeight;

    // Absolute (unwrapped) analysis- and synthesis-domain positions —
    // same "single absolute counter, no wraparound arithmetic in the
    // position logic itself" pattern PSOLAPitchShifter's
    // totalSamplesIn/nextMarkPos pair uses, for the same reason: only the
    // final circular-buffer index computation needs a modulo.
    long long nextAnalysisPos = 0;  // nominal (pre-search-correction) start of the next window to place
    long long nextSynthesisPos = 0; // fixed-hop start of the next window's output position
    long long lastPlacedAnalysisStart = -1; // -1 until the first window is placed — no correlation reference yet
    long long drainedSynthesisPos = 0; // next accumulator position not yet handed back via pull()
    long long placedFrontier = 0;      // accumulator positions before this are finalized and safe to drain

    /// Places one more analysis window if (and only if) enough input has
    /// been pushed for the widest possible search candidate; returns
    /// false (places nothing) otherwise. `analysisHopSamples` is
    /// recomputed by the caller each pull() from the current ratio, since
    /// WSOLA's whole stretch mechanism is this hop scaling relative to
    /// the fixed synthesisHopSamples.
    bool placeNextWindowIfPossible (int analysisHopSamples);

    /// Cross-correlates the tail of the previously placed window against
    /// every candidate offset in [-searchRadiusSamples, searchRadiusSamples]
    /// around `nominalStart`, returning whichever offset maximizes
    /// normalized correlation over the overlap region. Only ever called
    /// once lastPlacedAnalysisStart >= 0 (there's nothing to correlate
    /// against for the very first window, which is placed at its nominal
    /// position unconditionally).
    [[nodiscard]] int searchBestOffset (long long nominalStart) const noexcept;

    [[nodiscard]] float historyAt (long long index) const noexcept
    {
        return history[(size_t) (index % (long long) history.size())];
    }
};

} // namespace pitchzazz
