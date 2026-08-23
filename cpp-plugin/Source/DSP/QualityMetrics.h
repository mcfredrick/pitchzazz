#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

namespace pitchzazz
{

/// Objective, numeric alternative to "does it sound right" for comparing
/// this project's pitch-shift engines — docs/TESTING.md's existing
/// 3-step framing (synthetic known signal -> real production
/// architecture -> numeric threshold decided in advance), applied here to
/// spectral cleanliness rather than timing or energy. Operates on a
/// rendered output buffer from a synthetic test tone with a *known*
/// target fundamental — this is a measurement tool for signals with a
/// ground-truth answer, not a blind analyzer of arbitrary audio.
///
/// Two numbers, both derived from one windowed FFT of a steady-state
/// (post-warm-up) segment of the output:
///
///   - thdPlusNPercent: the standard audio-engineering THD+N definition
///     (AES17-style) — sqrt((totalEnergy - fundamentalEnergy) /
///     fundamentalEnergy) * 100. Everything that isn't the fundamental,
///     relative to the fundamental.
///   - artifactEnergyPercent: energy that isn't explained by *any*
///     harmonic of the target fundamental (not just the fundamental
///     itself) — sqrt((totalEnergy - harmonicEnergy) / totalEnergy) *
///     100. This is the metric that actually answers the anti-aliasing
///     question this feature grew out of: a clean harmonic pitch shift
///     has real energy at its harmonics, so THD+N alone doesn't
///     distinguish "shifted cleanly" from "shifted with artifacts" — what
///     this catches is energy *outside* the harmonic comb, which is
///     exactly what resampling aliasing or grain-boundary discontinuities
///     produce and clean bin-relocation/grain-repositioning don't.
///
/// FFT normalization note: JUCE's *inverse* FFT auto-normalizes by 1/N
/// (see PitchShifter.cpp/PitchDetector.cpp's comments for the general
/// convention this project already tracks elsewhere) — but both metrics
/// here are pure energy *ratios* computed from one *forward*-only FFT, so
/// any constant scale factor cancels in the ratio. Normalization simply
/// doesn't matter for this class, unlike for the inverse-FFT-using
/// shifters this project already has.
///
/// Not real-time-safe to construct (allocates FFT/scratch buffers once);
/// analyze() itself doesn't allocate. Same "pure DSP, no threading/IO"
/// shape as Corrector/PSOLAPitchShifter — this is an analysis/test tool,
/// not part of the real-time signal path, but follows the same
/// discipline anyway since nothing about it is specific to production
/// code.
class QualityMetrics
{
public:
    struct Result
    {
        float thdPlusNPercent = 0.0f;
        float artifactEnergyPercent = 0.0f;

        // False when the target fundamental itself carries too little of
        // the signal's total energy for a fundamental-relative ratio to
        // mean anything (see analyze()'s doc) — thdPlusNPercent is 0.0f
        // in that case, not a huge or NaN number; check this flag rather
        // than assuming the percent field alone tells the whole story.
        bool thdPlusNValid = true;
    };

    /// `fftSize` must be a power of two. This project's tests/probe use
    /// 16384 — ~2.7Hz bins at 44.1kHz, fine enough to separate a
    /// fundamental's harmonics from each other and from nearby artifact
    /// energy without an unreasonably long analysis window.
    explicit QualityMetrics (int fftSize);

    /// `signal` must be at least `fftSize` samples — pass a steady-state
    /// (post algorithmic-warm-up) tail of a longer render, not a
    /// signal's leading edge, or the transient itself reads as "artifact
    /// energy". Only the *last* fftSize samples of `signal` are
    /// analyzed.
    ///
    /// A real, measured edge case (not a hypothetical): TD-PSOLA shifted
    /// a full octave down redistributes almost all of a tone's energy
    /// onto its upper harmonics, leaving the true fundamental bin
    /// carrying next to nothing — thdPlusNPercent's ratio is relative to
    /// that near-zero fundamental, so it explodes to a meaningless
    /// multi-million-percent number in that case rather than reporting
    /// something a caller could mistake for a small, well-behaved value.
    /// Result::thdPlusNValid is false whenever the fundamental carries
    /// less than a small fraction of total energy, and thdPlusNPercent
    /// is 0.0f (not a huge or NaN number) in that case — check the flag,
    /// don't just trust the percent alone. artifactEnergyPercent doesn't
    /// have this failure mode: its denominator is total energy, not the
    /// fundamental alone, so it stays well-conditioned regardless.
    [[nodiscard]] Result analyze (const std::vector<float>& signal, double sampleRate, float targetFundamentalHz);

private:
    int fftSize;
    int fftOrder;
    juce::dsp::FFT fft;
    std::vector<float> windowing;
    std::vector<std::complex<float>> timeDomain, freqDomain;
};

} // namespace pitchzazz
