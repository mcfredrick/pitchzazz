#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

namespace pitchzazz
{

struct DetectedPitch
{
    float frequencyHz = 0.0f;
    float clarity = 0.0f;
};

/// McLeod pitch detector (MPM — "A Smarter Way to Find Pitch", McLeod &
/// Wyvill), ported from the Rust `pitch-detection` crate's
/// `McLeodDetector`, audited at
/// ~/.cargo/registry/src/.../pitch-detection-0.3.0/src/detector/{mcleod,internals}.rs
/// and utils/peak.rs, rather than re-implemented from the paper directly
/// — the C++ and Rust sides need to run the same peak-picking/
/// interpolation logic, or the Phase 3 Rust-vs-C++ comparison
/// (docs/ROADMAP.md) doesn't mean anything.
///
/// One deliberate numeric deviation, documented in detail in the .cpp:
/// this class zero-pads to the next power of two (JUCE's FFT requires
/// it; rustfft doesn't), where the Rust version pads to exactly
/// `blockSize + blockSize/2`. Both padding amounts are large enough that
/// the algorithm's actual behavior is unaffected — see the .cpp's
/// `normalizedSquareDifference` comment for why.
///
/// Buffers are sized once at construction and reused across calls to
/// `detect()` — no per-block allocation, matching the same principle
/// pitch-core's `PitchCorrector::new` documents for the Rust side.
class PitchDetector
{
public:
    /// `blockSize` is the number of input samples per `detect()` call,
    /// fixed for the object's lifetime (like the Rust version's
    /// `block_size`).
    explicit PitchDetector (int blockSize);

    /// `signal.size()` must equal `blockSize`. Returns {0, 0} if the
    /// block's energy is below the power threshold — matches the Rust
    /// version's `unwrap_or(Pitch { frequency: 0.0, clarity: 0.0 })`
    /// fallback in corrector.rs.
    [[nodiscard]] DetectedPitch detect (const std::vector<float>& signal, double sampleRate);

private:
    int blockSize;
    int fftOrder;
    int fftSize; // 2^fftOrder

    juce::dsp::FFT fft;

    std::vector<std::complex<float>> complexA, complexB;
    std::vector<float> acf;     // autocorrelation, length fftSize
    std::vector<float> mScratch; // NSDF denominator, length fftSize
    std::vector<float> nsdf;    // normalized square difference, length fftSize

    // Reused across detect() calls via .clear() (retains capacity) rather
    // than a fresh local vector per call — the class doc's "no per-block
    // allocation" claim used to be false in practice: detectPeaks() built
    // a brand-new vector on every call regardless of this comment. See
    // docs/FINDINGS.md for the finding.
    std::vector<std::pair<size_t, float>> peakScratch;

    static constexpr float powerThreshold = 0.15f;
    static constexpr float clarityThreshold = 0.1f;

    void normalizedSquareDifference (const std::vector<float>& signal);
};

} // namespace pitchzazz
