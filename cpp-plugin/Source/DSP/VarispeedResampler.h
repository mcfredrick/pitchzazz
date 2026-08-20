#pragma once

#include <vector>

namespace pitchzazz
{

/// Variable-rate resampler — the stage that actually produces this
/// engine's "tape/vinyl" character. Playing a signal back at a different
/// rate shifts its *entire* spectrum uniformly, formants included (this is
/// exactly what changes when you nudge a turntable's pitch fader, or why a
/// tape sped up sounds smaller/higher and slowed down sounds bigger/lower)
/// — as opposed to the phase vocoder or PSOLA, which both deliberately
/// decouple pitch from the spectral envelope. That formant shift is the
/// deliberate creative point of this engine (see VarispeedShifter's doc),
/// not an artifact to hide.
///
/// Cubic Hermite (Catmull-Rom) interpolation, not linear or windowed-sinc:
/// linear is a poor low-pass filter (audibly dulls highs when reading
/// slower, produces obvious stairstep/zipper imaging when reading faster —
/// reads as *broken*, not *vintage*); a full windowed-sinc kernel is the
/// "correct" band-limited choice but adds real per-sample cost and its own
/// lookahead for a transparency this engine isn't going for in the first
/// place. Cubic Hermite is the standard middle ground real resamplers use
/// (e.g. SoX's cubic mode): a fixed 4-tap kernel, cheap, allocation-free,
/// and its own mild high-frequency roll-off and self-aliasing at extreme
/// ratios sit comfortably alongside — rather than fighting — the tape
/// character this engine exists to produce. No separate anti-aliasing
/// pre-filter is applied for the same reason: physical tape/vinyl varispeed
/// has no digital anti-aliasing either, and the kernel's own roll-off
/// substitutes loosely for one. Known, accepted limitation: an extreme
/// up-shift could alias harshly enough to stop sounding "vintage" and start
/// sounding broken — not tuned against by ear yet, flagged here rather than
/// assumed fine.
///
/// Streaming push/pull shape, not a block-in-block-out `shiftPitch()` like
/// PitchShifter/PSOLAPitchShifter — this class is one stage of a two-stage
/// pipeline (see VarispeedShifter), and the two stages naturally produce
/// samples at different rates, so a push/pull interface is what lets the
/// caller drain each stage independently rather than forcing a fixed
/// block-size contract on an intermediate signal whose length isn't fixed.
///
/// Not real-time-safe to construct (allocates its history buffer once);
/// push()/pull() themselves don't allocate. Deliberately takes no
/// sampleRate — the cubic kernel works entirely in sample-index space
/// (`ratio` is already unitless), unlike its sibling shifter classes which
/// need sampleRate to reason about periods/windows in Hz or milliseconds.
class VarispeedResampler
{
public:
    VarispeedResampler();

    /// Appends newly-available input samples to the internal history.
    /// Never allocates — `history` is a fixed-size circular buffer sized
    /// once at construction with generous headroom (see the .cpp), the
    /// same "cheap in memory, avoids wraparound risk" trade-off
    /// PSOLAPitchShifter's own history buffer makes, rather than tightly
    /// sizing to the theoretical minimum.
    void push (const float* input, int count);

    /// Produces up to `maxCount` resampled samples into `output`,
    /// advancing the internal fractional read position by `ratio` per
    /// output sample (`ratio` > 1 reads the input faster — pitch/speed up;
    /// < 1 reads it slower — pitch/speed down). Returns the actual number
    /// of samples produced, which is less than `maxCount` whenever the
    /// next output sample would need an input sample not yet pushed —
    /// the caller (VarispeedShifter) is expected to call push() again and
    /// retry rather than this class blocking or padding with silence
    /// itself, matching this codebase's standing rule that only the
    /// real-time audio callback's own underrun handling gets to decide
    /// what "not enough data yet" means.
    int pull (float ratio, float* output, int maxCount);

    /// Fixed at 2 samples — cubic Hermite needs the input sample just
    /// past the current integer read position, plus one more to fit the
    /// 4-point kernel (positions readPos-1, readPos, readPos+1, readPos+2
    /// around the interpolated fractional position). This is a genuinely
    /// ratio-independent constant, unlike a windowed-sinc kernel whose
    /// practical width often gets scaled with ratio for anti-aliasing —
    /// exactly why this class was chosen over that for a bounded-latency
    /// pipeline (see VarispeedShifter's latency doc).
    [[nodiscard]] int getLatencySamples() const noexcept { return kernelLookahead; }

private:
    static constexpr int kernelLookahead = 2;

    // Circular history of pushed input. Sized to a fixed, generous margin
    // (see the .cpp) rather than tightly to the 4-sample kernel width —
    // the caller may push more than one block's worth before pulling (the
    // two-stage FIFO in VarispeedShifter doesn't guarantee push/pull
    // alternate 1:1), so this needs real headroom, not just kernel width.
    std::vector<float> history;

    // Absolute write position into the conceptual infinite input stream —
    // history[totalPushed % history.size()] is always the next slot to
    // write. readPos is likewise absolute, not wrapped, so the "is enough
    // input available" check in pull() is a single unwrapped comparison
    // against totalPushed, same pattern PSOLAPitchShifter's
    // totalSamplesIn/nextMarkPos pair uses for the same reason.
    long long totalPushed = 0;
    double readPos = 0.0;
};

} // namespace pitchzazz
