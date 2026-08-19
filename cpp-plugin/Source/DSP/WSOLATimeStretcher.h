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
/// Search radius ≈ window/4. Deliberately a first pass, not a
/// state-of-the-art implementation: the search correlates only over the
/// overlap region (not the full window, to keep cost down) using plain
/// normalized cross-correlation, not a smarter/cheaper similarity metric
/// — see docs/PERFORMANCE_LOG.md for the measured cost once benchmarked.
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
    /// Returns the actual number of samples produced, which is less than
    /// `maxCount` whenever producing more would need an analysis window
    /// that reaches past what's been pushed so far — same "caller pushes
    /// more and retries" contract as VarispeedResampler::pull().
    int pull (float ratio, float* output, int maxCount);

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
