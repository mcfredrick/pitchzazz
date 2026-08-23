// Tests a hypothesis nothing else in this investigation could have caught:
// every render so far (PSOLACrackleProbe.cpp, PSOLAWaveformDump.cpp,
// PSOLARealPipelineProbe.cpp) used a pure sine wave, jittered or not. A
// pure sine has no harmonics -- any artifact that's specifically about
// *harmonic phase coherence* breaking at a grain transition (the "buzzy"/
// "phasy" character real timbral complaints are often actually about, as
// opposed to a raw time-domain amplitude glitch) is structurally
// impossible to see in a single-frequency signal. This builds a
// harmonically rich, jittered, vowel-like tone instead -- fundamental
// plus four harmonics at realistic falling amplitudes, all jittered
// together (so the harmonic relationship is preserved in the *input*,
// same as a real voice) -- and checks both the time-domain jump/curvature
// methodology already established AND QualityMetrics' artifact-energy
// measure (energy outside the *entire* harmonic comb, not just the
// fundamental -- exactly the metric that would catch inter-harmonic
// phase smearing a pure-sine test never could).

#include <DSP/PSOLAPitchShifter.h>
#include <DSP/PitchShifter.h>
#include <DSP/QualityMetrics.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int analysisFftSize = 16384;
    constexpr float baseFreq = 220.0f;

    // Fundamental + 4 harmonics, falling amplitude (1, 0.5, 0.33, 0.25,
    // 0.2 -- roughly 1/n, a plausible vowel-like spectral tilt), all
    // jittered together via the same instantaneous-frequency wobble so
    // the harmonic relationship (n * f0 exactly) is preserved in the
    // input, the same way a real voice's jitter moves the whole harmonic
    // stack together, not each partial independently.
    std::vector<float> jitteredHarmonicVoice (int totalSamples, float jitterDepth)
    {
        constexpr float jitterRate1 = 11.3f;
        constexpr float jitterRate2 = 17.7f;
        constexpr int numHarmonics = 5;
        const float amps[numHarmonics] = { 1.0f, 0.5f, 0.33f, 0.25f, 0.2f };

        std::vector<float> out ((size_t) totalSamples, 0.0f);
        double phase = 0.0;
        double ampNorm = 0.0f;
        for (float a : amps) ampNorm += a;

        for (int i = 0; i < totalSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            const double wobble = std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate1 * t) * 0.6
                                 + std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate2 * t) * 0.4;
            const double instFreq = (double) baseFreq * (1.0 + (double) jitterDepth * wobble);
            phase += 2.0 * juce::MathConstants<double>::pi * instFreq / sampleRate;

            float sample = 0.0f;
            for (int h = 0; h < numHarmonics; ++h)
                sample += amps[h] * (float) std::sin (phase * (double) (h + 1));
            out[(size_t) i] = sample / (float) ampNorm;
        }
        return out;
    }
}

TEST_CASE ("PSOLA harmonic-content probe: does real harmonic structure expose something a pure sine can't?", "[psola][harmonic]")
{
    std::cout << std::fixed << std::setprecision (4);

    // Control 0: QualityMetrics on the raw, completely unprocessed input
    // -- no PSOLA, no phase vocoder, nothing. If this alone already shows
    // high "artifact energy," the metric itself can't distinguish this
    // jittered multi-harmonic signal's own FM sidebands (each harmonic
    // spreads proportionally more in absolute Hz than the fundamental
    // does, for the same percentage jitter) from genuine processing
    // artifacts -- the metric assumes stationary harmonics.
    {
        const auto input = jitteredHarmonicVoice (blockSize * 20, 0.02f);
        QualityMetrics metrics (analysisFftSize);
        const auto result = metrics.analyze (input, sampleRate, baseFreq);
        std::cout << "\n=== CONTROL: raw unprocessed input, harmonic signal, no shifting at all ===\n";
        std::cout << "THD+N%: " << result.thdPlusNPercent << "  artifact energy%: " << result.artifactEnergyPercent << "\n";

        constexpr float threshold = 0.045f;
        int rawJumps = 0;
        float maxSlope = 0.0f;
        for (size_t i = 1; i < input.size(); ++i)
        {
            const float d = std::abs (input[i] - input[(size_t) (i - 1)]);
            if (d > threshold) ++rawJumps;
            maxSlope = std::max (maxSlope, d);
        }
        std::cout << "jumps >" << threshold << " in raw input: " << rawJumps << " (measured max slope: " << maxSlope << ")\n";
    }

    // Control 1: unison (no shift at all) on the same harmonic signal --
    // if this is ALSO badly distorted, the problem is in the test signal
    // or the metric, not PSOLA's shift mechanism specifically.
    {
        const auto input = jitteredHarmonicVoice (blockSize * 20, 0.02f);
        PSOLAPitchShifter shifter (sampleRate);
        std::vector<float> shifterOutput (input.size(), 0.0f);
        const int totalBlocks = (int) (input.size() / (size_t) blockSize);
        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            std::vector<float> out (blockSize, 0.0f);
            shifter.shiftPitch (baseFreq, 0.0f, in, out);
            std::copy (out.begin(), out.end(), shifterOutput.begin() + block * blockSize);
        }
        QualityMetrics metrics (analysisFftSize);
        const auto result = metrics.analyze (shifterOutput, sampleRate, baseFreq);
        std::cout << "\n=== CONTROL: shift=0st (unison), harmonic input, PSOLA ===\n";
        std::cout << "THD+N%: " << result.thdPlusNPercent << "  artifact energy%: " << result.artifactEnergyPercent << "\n";
    }

    // Control 2: phase vocoder on the identical harmonic signal, same
    // +-1st shifts -- if this stays clean where PSOLA doesn't, the
    // problem is specific to PSOLA's mechanism, not harmonic content in
    // general or the metric itself.
    for (float shift : { -1.0f, 1.0f })
    {
        const auto input = jitteredHarmonicVoice (blockSize * 20, 0.02f);
        PitchShifter shifter (30, sampleRate);
        std::vector<float> shifterOutput (input.size(), 0.0f);
        const int totalBlocks = (int) (input.size() / (size_t) blockSize);
        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            std::vector<float> out (blockSize, 0.0f);
            shifter.shiftPitch (16, shift, in, out);
            std::copy (out.begin(), out.end(), shifterOutput.begin() + block * blockSize);
        }
        QualityMetrics metrics (analysisFftSize);
        const float targetFreq = baseFreq * std::pow (2.0f, shift / 12.0f);
        const auto result = metrics.analyze (shifterOutput, sampleRate, targetFreq);
        std::cout << "\n=== CONTROL: shift=" << shift << "st, harmonic input, PHASE VOCODER ===\n";
        std::cout << "THD+N%: " << result.thdPlusNPercent << "  artifact energy%: " << result.artifactEnergyPercent << "\n";
    }

    for (float shift : { -1.0f, 1.0f })
    {
        const auto input = jitteredHarmonicVoice (blockSize * 20, 0.02f);
        std::vector<float> output (input.size(), 0.0f);

        // Drives PSOLAPitchShifter directly with a controlled shift,
        // bypassing quantization -- same pattern QualityMetricsProbe.cpp
        // uses, since the question here is harmonic content, not
        // quantization interaction (already covered by
        // PSOLARealPipelineProbe.cpp).
        PSOLAPitchShifter shifter (sampleRate);
        std::vector<float> shifterOutput (input.size(), 0.0f);
        const int totalBlocks = (int) (input.size() / (size_t) blockSize);
        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            std::vector<float> out (blockSize, 0.0f);
            shifter.shiftPitch (baseFreq, shift, in, out);
            std::copy (out.begin(), out.end(), shifterOutput.begin() + block * blockSize);
        }

        std::cout << "\n=== shift=" << shift << "st, harmonic input (5 partials) ===\n";

        // Time-domain: same jump + curvature scan as every other render.
        constexpr float threshold = 0.045f;
        int jumpCount = 0;
        int curvatureCount = 0;
        for (size_t i = 2; i < shifterOutput.size(); ++i)
        {
            const float d = std::abs (shifterOutput[i] - shifterOutput[(size_t) (i - 1)]);
            if (d > threshold) ++jumpCount;
            const float predicted = 2.0f * shifterOutput[i - 1] - shifterOutput[i - 2];
            if (std::abs (shifterOutput[i] - predicted) > 0.05f) ++curvatureCount;
        }
        std::cout << "jumps >" << threshold << ": " << jumpCount << "\n";
        std::cout << "curvature outliers >0.05: " << curvatureCount << "\n";

        // Frequency-domain: QualityMetrics artifact energy -- catches
        // energy outside the *entire harmonic comb*, which a pure-sine
        // test structurally cannot exercise (there's only one harmonic).
        QualityMetrics metrics (analysisFftSize);
        const float targetFreq = baseFreq * std::pow (2.0f, shift / 12.0f);
        const auto result = metrics.analyze (shifterOutput, sampleRate, targetFreq);
        std::cout << "THD+N%: " << result.thdPlusNPercent << " (valid=" << (result.thdPlusNValid ? "true" : "false") << ")\n";
        std::cout << "artifact energy%: " << result.artifactEnergyPercent << "\n";

        const std::string filename = "/tmp/psola_harmonic_" + std::string (shift > 0 ? "+" : "") + std::to_string ((int) shift) + "st.csv";
        std::ofstream f (filename);
        f << "sample,input,output\n";
        for (size_t i = 0; i < input.size(); ++i)
            f << i << "," << input[i] << "," << shifterOutput[i] << "\n";
        std::cout << "Wrote " << filename << "\n";
    }
    std::cout << std::endl;
}
