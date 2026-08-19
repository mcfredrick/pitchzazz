#include "PitchDetector.h"
#include <cmath>
#include <limits>
#include <utility>

namespace pitchzazz
{

namespace
{
    int nextPowerOfTwoOrder (int n) noexcept
    {
        int order = 0;
        int size = 1;
        while (size < n)
        {
            size <<= 1;
            ++order;
        }
        return order;
    }

    // Port of pitch-detection's internals::m_of_tau. `result` must be at
    // least `signal.size() + 1` long. `signalSquareSum` is deliberately
    // the *unnormalized* autocorrelation-at-zero-lag value (see the
    // caller), not a plain sum of squares — matching the Rust call site
    // exactly, not a "cleaned up" version of it. See
    // normalizedSquareDifference()'s comment for why this is safe.
    void mOfTau (const std::vector<float>& signal, float signalSquareSum, std::vector<float>& result)
    {
        const float start = 2.0f * signalSquareSum;
        result[0] = start;
        float acc = start;
        for (size_t i = 0; i < signal.size(); ++i)
        {
            acc -= signal[i] * signal[i];
            result[i + 1] = acc;
        }
        const float last = acc;
        for (size_t i = signal.size() + 1; i < result.size(); ++i)
            result[i] = last;
    }

    // Port of pitch-detection's utils::peak. Finds every "positive lobe"
    // maximum in `data`, skipping the initial positive run at the very
    // start (self-correlation at zero lag is always the global max and
    // never a meaningful peak) and discarding a trailing positive run
    // that reaches the end of the array without dipping negative first
    // (the original crate's comment: "the data was probably truncated").
    // `peaks` is caller-owned scratch storage, cleared (not reallocated)
    // on entry — capacity retained across calls so steady-state detect()
    // calls never touch the heap here.
    void detectPeaks (const std::vector<float>& data, std::vector<std::pair<size_t, float>>& peaks)
    {
        peaks.clear();
        const size_t n = data.size();
        size_t idx = 0;

        while (idx < n && data[idx] >= 0.0f)
            ++idx;

        while (idx < n)
        {
            while (idx < n && data[idx] < 0.0f)
                ++idx;
            if (idx >= n)
                break;

            size_t maxIndex = idx;
            float maxValue = -std::numeric_limits<float>::infinity();
            while (idx < n && data[idx] >= 0.0f)
            {
                if (data[idx] > maxValue)
                {
                    maxValue = data[idx];
                    maxIndex = idx;
                }
                ++idx;
            }

            if (idx == n)
                break; // trailing run never dipped negative — discard, matches PeaksIter

            peaks.emplace_back (maxIndex, maxValue);
        }
    }

    // Quadratic interpolation through (-1, y0), (0, y1), (1, y2). Returns
    // (x-offset, peak value). Direct port of find_quadratic_peak.
    std::pair<float, float> findQuadraticPeak (float y0, float y1, float y2) noexcept
    {
        const float a = (y0 + y2) / 2.0f - y1;
        const float b = (y2 - y0) / 2.0f;
        const float c = y1;

        if (a > 0.0f) // concave up: the "peak" is one of the endpoints
            return (y0 > y2) ? std::pair<float, float> { -1.0f, y0 } : std::pair<float, float> { 1.0f, y2 };

        return { -b / (2.0f * a), -b * b / (4.0f * a) + c };
    }

    std::pair<float, float> correctPeak (std::pair<size_t, float> peak, const std::vector<float>& data) noexcept
    {
        const size_t idx = peak.first;
        const auto [xOffset, y] = findQuadraticPeak (data[idx - 1], data[idx], data[idx + 1]);
        return { xOffset + (float) idx, y };
    }

    DetectedPitch pitchFromPeaks (const std::vector<float>& nsdf, double sampleRate, float clarityThreshold,
                                   std::vector<std::pair<size_t, float>>& peakScratch)
    {
        detectPeaks (nsdf, peakScratch);
        for (const auto& peak : peakScratch)
        {
            if (peak.second > clarityThreshold)
            {
                const auto [tau, height] = correctPeak (peak, nsdf);
                return { (float) (sampleRate / (double) tau), height / nsdf[0] };
            }
        }
        return {};
    }
}

PitchDetector::PitchDetector (int blockSizeIn)
    : blockSize (blockSizeIn),
      fftOrder (nextPowerOfTwoOrder (blockSizeIn + blockSizeIn / 2)),
      fftSize (1 << fftOrder),
      fft (fftOrder),
      complexA ((size_t) fftSize),
      complexB ((size_t) fftSize),
      acf ((size_t) fftSize),
      mScratch ((size_t) fftSize),
      nsdf ((size_t) fftSize)
{
}

void PitchDetector::normalizedSquareDifference (const std::vector<float>& signal)
{
    // Autocorrelation via FFT (Wiener-Khinchin): zero-pad the signal into
    // a complex buffer, forward FFT, take |X(f)|^2, inverse FFT, keep the
    // real part. Matches pitch-detection's internals::autocorrelation.
    std::fill (complexA.begin(), complexA.end(), std::complex<float> {});
    for (size_t i = 0; i < signal.size(); ++i)
        complexA[i] = { signal[i], 0.0f };

    fft.perform (complexA.data(), complexB.data(), false); // forward: unnormalized, matches rustfft

    for (auto& c : complexB)
        c = { c.real() * c.real() + c.imag() * c.imag(), 0.0f };

    fft.perform (complexB.data(), complexA.data(), true); // inverse

    // JUCE's inverse FFT divides by fftSize (a "true" inverse); rustfft's
    // does not (see rustfft's own doc: "callers must manually normalize").
    // The Rust `autocorrelation()` never adds that missing 1/N back in,
    // so its ACF values — and everything downstream that's built from
    // them — are implicitly inflated by a factor of the FFT size. That
    // inflation isn't a bug worth "fixing" here: m_of_tau's `start` term
    // is `2 * acf[0]`, and for real audio-sized blocks that inflated
    // start value dwarfs the raw per-sample subtraction terms that follow
    // it (start is on the order of fftSize * signal-energy, the total
    // possible decrease is only on the order of signal-energy), so the
    // NSDF ratio 2*acf[i]/m(i) ends up close to a true normalized
    // autocorrelation coefficient regardless of the exact inflation
    // factor. What matters for matching Rust's behavior against the same
    // POWER_THRESHOLD/CLARITY_THRESHOLD constants is reproducing this
    // exact shape (inflated ACF feeding into m_of_tau, not a "clean"
    // independently-computed sum of squares) — so we undo JUCE's 1/N
    // here to restore the same unnormalized-round-trip convention rustfft
    // has, rather than leaving JUCE's normalized value in place.
    for (int i = 0; i < fftSize; ++i)
        acf[(size_t) i] = complexA[(size_t) i].real() * (float) fftSize;

    mOfTau (signal, acf[0], mScratch);

    for (int i = 0; i < fftSize; ++i)
        nsdf[(size_t) i] = 2.0f * acf[(size_t) i] / mScratch[(size_t) i];
}

DetectedPitch PitchDetector::detect (const std::vector<float>& signal, double sampleRate)
{
    jassert ((int) signal.size() == blockSize);

    float energy = 0.0f;
    for (float s : signal)
        energy += s * s;
    if (energy < powerThreshold)
        return {};

    normalizedSquareDifference (signal);

    return pitchFromPeaks (nsdf, sampleRate, clarityThreshold, peakScratch);
}

} // namespace pitchzazz
