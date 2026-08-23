#include "QualityMetrics.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

namespace
{
    int orderOfPowerOfTwo (int powerOfTwo) noexcept
    {
        int order = 0;
        while ((1 << order) < powerOfTwo)
            ++order;
        return order;
    }

    // A Hann window's mainlobe first null sits 2 bins from center; 3
    // bins of tolerance absorbs that plus a little slack for the target
    // frequency not landing exactly on a bin. Any wider risks folding a
    // real, distinct nearby harmonic into the wrong one's tally at low
    // fundamentals; any narrower risks counting a clean tone's own
    // mainlobe leakage as "artifact" energy, which would make even a
    // perfectly clean shifter fail this metric.
    constexpr int harmonicBinTolerance = 3;

    // Below this fraction of total energy, "relative to the fundamental"
    // stops being a meaningful ratio at all (see analyze()'s doc for the
    // real TD-PSOLA-at-an-octave-down case that motivated this guard) —
    // not a tuned/measured threshold, just small enough that any signal
    // with a remotely genuine tone at the target frequency clears it by
    // orders of magnitude.
    constexpr double minFundamentalEnergyFraction = 1.0e-4;
}

QualityMetrics::QualityMetrics (int fftSizeIn)
    : fftSize (fftSizeIn),
      fftOrder (orderOfPowerOfTwo (fftSizeIn)),
      fft (fftOrder)
{
    jassert ((1 << fftOrder) == fftSize); // must be a genuine power of two, not just >= one

    // Hann window on the analysis segment — separate from, and in
    // addition to, whatever internal windowing a shifter under test
    // already does on its own grains/frames. Without this, the FFT's
    // implicit rectangular truncation of a finite segment leaks energy
    // into every bin (not just near the true tone), which would show up
    // as spurious "artifact energy" even for a perfectly clean sine —
    // contaminating exactly the engines this metric exists to prove
    // clean.
    windowing.resize ((size_t) fftSize);
    for (int k = 0; k < fftSize; ++k)
        windowing[(size_t) k] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) k / (float) (fftSize - 1));

    timeDomain.assign ((size_t) fftSize, {});
    freqDomain.assign ((size_t) fftSize, {});
}

QualityMetrics::Result QualityMetrics::analyze (const std::vector<float>& signal, double sampleRate, float targetFundamentalHz)
{
    jassert ((int) signal.size() >= fftSize);
    const size_t offset = signal.size() - (size_t) fftSize;

    for (int k = 0; k < fftSize; ++k)
        timeDomain[(size_t) k] = { signal[offset + (size_t) k] * windowing[(size_t) k], 0.0f };

    fft.perform (timeDomain.data(), freqDomain.data(), false); // forward: normalization irrelevant, see class doc

    const int halfSize = fftSize / 2; // Nyquist bin, exclusive upper bound for the real content considered below
    const float binWidth = (float) sampleRate / (float) fftSize;

    std::vector<float> binEnergy ((size_t) halfSize, 0.0f);
    double totalEnergy = 0.0;
    // Skip DC (k=0): a shifted/quantized tone should have none, and a DC
    // offset isn't the kind of spectral-purity artifact this metric is
    // targeting.
    for (int k = 1; k < halfSize; ++k)
    {
        const float magnitude = std::abs (freqDomain[(size_t) k]);
        const float energy = magnitude * magnitude;
        binEnergy[(size_t) k] = energy;
        totalEnergy += energy;
    }

    const auto energyNear = [&] (float freq) -> double
    {
        const int centerBin = (int) std::round (freq / binWidth);
        double sum = 0.0;
        for (int k = std::max (1, centerBin - harmonicBinTolerance); k < std::min (halfSize, centerBin + harmonicBinTolerance + 1); ++k)
            sum += binEnergy[(size_t) k];
        return sum;
    };

    const double fundamentalEnergy = energyNear (targetFundamentalHz);

    // harmonicEnergy's n=1 term *is* fundamentalEnergy — this
    // deliberately includes it rather than summing "harmonics above the
    // fundamental" separately, since artifactEnergyPercent's definition
    // is "energy outside the whole harmonic comb," fundamental included.
    double harmonicEnergy = 0.0;
    const float nyquist = (float) sampleRate / 2.0f;
    for (int n = 1; (float) n * targetFundamentalHz < nyquist; ++n)
        harmonicEnergy += energyNear ((float) n * targetFundamentalHz);

    Result result;
    // thdPlusNPercent's ratio is only meaningful when the fundamental
    // actually carries a real share of the signal's energy — see this
    // guard's own doc in the header (a real, measured TD-PSOLA case: a
    // full octave down, almost no energy survives at the true
    // fundamental itself, only at its upper harmonics). Below that
    // floor, report 0.0f/invalid rather than a ratio that's technically
    // defined but not meaningful — a huge or wildly noisy number here
    // would poison any comparison table it's read into, silently, which
    // is worse than an explicit "not applicable."
    if (totalEnergy > 1.0e-12 && fundamentalEnergy > totalEnergy * minFundamentalEnergyFraction)
    {
        result.thdPlusNPercent = 100.0f * (float) std::sqrt (std::max (0.0, totalEnergy - fundamentalEnergy) / fundamentalEnergy);
    }
    else
    {
        result.thdPlusNPercent = 0.0f;
        result.thdPlusNValid = false;
    }

    if (totalEnergy > 1.0e-12)
        result.artifactEnergyPercent = 100.0f * (float) std::sqrt (std::max (0.0, totalEnergy - harmonicEnergy) / totalEnergy);

    return result;
}

} // namespace pitchzazz
