#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

namespace pitchzazz
{

/// Phase-vocoder pitch shifter, ported from the Rust `pitch_shift` crate
/// (audited at ~/.cargo/registry/src/.../pitch_shift-1.0.0/src/lib.rs),
/// itself an implementation of Stephan Bernsee's classic smbPitchShift
/// algorithm. Faithful port of the STFT / overlap-add / phase-
/// accumulation math; see the .cpp for where JUCE's FFT normalization
/// convention differs from rustfft/realfft's and how that's compensated.
///
/// Not real-time-safe to construct (allocates its internal FIFOs/window
/// once), but `shiftPitch()` itself doesn't allocate — same shape as the
/// Rust PitchShifter's design and pitch-core's PitchCorrector doc.
class PitchShifter
{
public:
    /// `windowDurationMs` should be ~50ms (matches pitch-cli's
    /// `PitchCorrector::new(..., 50, ...)` call). The actual internal
    /// frame size is rounded to the *nearest* power of two — JUCE's FFT
    /// requires a power-of-two size; rustfft/realfft don't. See the .cpp
    /// constructor for the resulting frame sizes per sample rate and why
    /// "nearest" was chosen over "next".
    PitchShifter (int windowDurationMs, double sampleRate);

    /// `overSampling` trades quality for CPU cost — see pitch-core's
    /// OVER_SAMPLING constant and docs/PERFORMANCE_LOG.md for why this
    /// project settled on 8 for the Rust engine (needs re-verifying for
    /// this engine's own cost profile before assuming it carries over —
    /// see docs/ROADMAP.md Phase 2/3). `semitoneShift` is signed.
    /// `input`/`output` must be the same length (the plugin's block
    /// size) — independent of the internal frame size; the phase
    /// vocoder's own FIFO absorbs the size mismatch, exactly like the
    /// Rust version.
    void shiftPitch (int overSampling, float semitoneShift,
                      const std::vector<float>& input, std::vector<float>& output);

    /// The algorithmic latency this shifter contributes, in samples —
    /// empirically confirmed (not just assumed) to equal the analysis
    /// frame size exactly, independent of block size or over-sampling;
    /// see docs/PERFORMANCE_LOG.md's "Measured pipeline latency" entry.
    int getLatencySamples() const noexcept { return frameSize; }

private:
    double sampleRate;
    int frameSize = 0;
    int fftOrder = 0;
    juce::dsp::FFT fft;

    std::vector<float> windowing;
    std::vector<float> inFifo, outFifo;
    std::vector<float> lastPhase, phaseSum;
    std::vector<float> outputAccumulator;
    std::vector<float> synthesizedFrequency, synthesizedMagnitude;
    std::vector<std::complex<float>> fftTime; // frameSize
    std::vector<std::complex<float>> fftFreq; // frameSize, full complex spectrum

    int overlap = 0;
};

} // namespace pitchzazz
