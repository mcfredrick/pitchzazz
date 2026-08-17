#include "PitchShifter.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

namespace
{
    int nearestPowerOfTwo (int n) noexcept
    {
        int lower = 1;
        while (lower * 2 <= n)
            lower *= 2;
        const int upper = lower * 2;
        return (n - lower <= upper - n) ? lower : upper;
    }

    int orderOf (int powerOfTwo) noexcept
    {
        int order = 0;
        while ((1 << order) < powerOfTwo)
            ++order;
        return order;
    }

    // frame_size = sample_rate * window_ms / 1000, rounded to the
    // *nearest* power of two. Rust's version rounds up to even instead
    // (realfft accepts any even length); JUCE's FFT needs a power of two.
    // "Nearest" was chosen over "next power of two" to avoid silently
    // doubling the window/latency — e.g. 44100Hz * 50ms would round up to
    // 4096 (~93ms, ~2x the requested window) but rounds to the nearer
    // 2048 (~46.4ms) instead. This is the one place the C++ and Rust
    // engines' actual STFT window size provably diverges — worth calling
    // out explicitly in the Phase 3 Rust-vs-C++ comparison
    // (docs/ROADMAP.md), not hidden. Typical results: 44100/48000Hz ->
    // 2048 samples (~46.4ms / ~42.7ms); 96000Hz -> 4096 (~42.7ms).
    int computeFrameSize (int windowDurationMs, double sampleRate) noexcept
    {
        const int target = (int) (sampleRate * (double) windowDurationMs / 1000.0);
        return nearestPowerOfTwo (juce::jmax (2, target));
    }
}

PitchShifter::PitchShifter (int windowDurationMs, double sampleRateIn)
    : sampleRate (sampleRateIn),
      frameSize (computeFrameSize (windowDurationMs, sampleRateIn)),
      fftOrder (orderOf (frameSize)),
      fft (fftOrder)
{
    windowing.resize ((size_t) frameSize);
    for (int k = 0; k < frameSize; ++k)
        windowing[(size_t) k] = -0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) k / (float) frameSize) + 0.5f;

    inFifo.assign ((size_t) frameSize, 0.0f);
    outFifo.assign ((size_t) frameSize, 0.0f);

    const int halfFrameSize = frameSize / 2 + 1;
    lastPhase.assign ((size_t) halfFrameSize, 0.0f);
    phaseSum.assign ((size_t) halfFrameSize, 0.0f);
    synthesizedFrequency.assign ((size_t) frameSize, 0.0f);
    synthesizedMagnitude.assign ((size_t) frameSize, 0.0f);
    outputAccumulator.assign ((size_t) frameSize * 2, 0.0f);

    fftTime.assign ((size_t) frameSize, {});
    fftFreq.assign ((size_t) frameSize, {});
}

void PitchShifter::shiftPitch (int overSampling, float semitoneShift,
                                const std::vector<float>& input, std::vector<float>& output)
{
    const float shift = std::pow (2.0f, semitoneShift / 12.0f);
    const float fsReal = (float) frameSize;
    const int halfFrameSize = frameSize / 2 + 1;

    const int step = frameSize / overSampling;
    const float binFrequencies = (float) sampleRate / fsReal;
    const float expected = juce::MathConstants<float>::twoPi / (float) overSampling;
    const int fifoLatency = frameSize - step;

    if (overlap == 0)
        overlap = fifoLatency;

    const float pitchWeight = shift * binFrequencies;
    const float oversampWeight = ((float) overSampling / juce::MathConstants<float>::twoPi) * pitchWeight;
    const float meanExpected = expected / binFrequencies;

    for (size_t i = 0; i < output.size(); ++i)
    {
        inFifo[(size_t) overlap] = input[i];
        output[i] = outFifo[(size_t) (overlap - fifoLatency)];
        ++overlap;

        if (overlap >= frameSize)
        {
            overlap = fifoLatency;

            for (int k = 0; k < frameSize; ++k)
                fftTime[(size_t) k] = { inFifo[(size_t) k] * windowing[(size_t) k], 0.0f };

            fft.perform (fftTime.data(), fftFreq.data(), false); // forward: unnormalized, matches realfft

            std::fill (synthesizedMagnitude.begin(), synthesizedMagnitude.end(), 0.0f);
            std::fill (synthesizedFrequency.begin(), synthesizedFrequency.end(), 0.0f);

            for (int k = 0; k < halfFrameSize; ++k)
            {
                const float kReal = (float) k;
                const int index = (int) std::round (kReal * shift);
                if (index < halfFrameSize)
                {
                    const float magnitude = std::abs (fftFreq[(size_t) k]);
                    const float phase = std::arg (fftFreq[(size_t) k]);
                    float deltaPhase = (phase - lastPhase[(size_t) k]) - kReal * expected;

                    auto qpd = (int64_t) (deltaPhase / juce::MathConstants<float>::pi);
                    if (qpd >= 0)
                        qpd += qpd & 1;
                    else
                        qpd -= qpd & 1;

                    deltaPhase -= juce::MathConstants<float>::pi * (float) qpd;
                    lastPhase[(size_t) k] = phase;
                    synthesizedMagnitude[(size_t) index] += magnitude;
                    synthesizedFrequency[(size_t) index] = kReal * pitchWeight + oversampWeight * deltaPhase;
                }
            }

            std::fill (fftFreq.begin(), fftFreq.end(), std::complex<float> {});

            for (int k = 0; k < halfFrameSize; ++k)
            {
                phaseSum[(size_t) k] += meanExpected * synthesizedFrequency[(size_t) k];
                fftFreq[(size_t) k] = std::polar (synthesizedMagnitude[(size_t) k], phaseSum[(size_t) k]);
            }

            // Real input -> conjugate-symmetric spectrum. Only bins
            // [0, halfFrameSize) were synthesized above (matching the
            // Rust version's real-FFT half-spectrum exactly); mirror them
            // into the upper half so a full complex inverse FFT still
            // reconstructs a real time-domain frame. Bins 0 and Nyquist
            // (index frameSize/2) have no mirror partner and are already
            // set by the loop above.
            for (int k = 1; k < frameSize / 2; ++k)
                fftFreq[(size_t) (frameSize - k)] = std::conj (fftFreq[(size_t) k]);

            fft.perform (fftFreq.data(), fftTime.data(), true); // inverse: JUCE normalizes by 1/frameSize; rustfft/realfft do not — compensated below

            // Rust: acc_oversamp = 2.0 / (half_frame_size * over_sampling)
            // assumes an unnormalized inverse FFT (realfft's convention).
            // JUCE's inverse already divides by frameSize, so multiply
            // that factor back in here to match — same normalization
            // mismatch as PitchDetector's autocorrelation, see that
            // file's comment for the general JUCE-vs-rustfft pattern.
            const float accOversamp = 2.0f * fsReal / ((float) halfFrameSize * (float) overSampling);

            for (int k = 0; k < frameSize; ++k)
            {
                const float product = windowing[(size_t) k] * fftTime[(size_t) k].real() * accOversamp;
                outputAccumulator[(size_t) k] += product / 2.0f;
            }

            std::copy (outputAccumulator.begin(), outputAccumulator.begin() + step, outFifo.begin());
            std::copy (outputAccumulator.begin() + step, outputAccumulator.begin() + step + frameSize, outputAccumulator.begin());
            std::copy (inFifo.begin() + step, inFifo.begin() + step + fifoLatency, inFifo.begin());
        }
    }
}

} // namespace pitchzazz
