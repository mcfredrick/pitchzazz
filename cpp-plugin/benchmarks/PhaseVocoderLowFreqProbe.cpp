// Checks whether the phase vocoder has an unexamined low-frequency
// compromise analogous to PSOLA's explicit, by-ear-validated minHz=80Hz
// floor (PSOLAPitchShifter.h). Grepping the phase vocoder's source found
// no documented frequency floor at all -- worth checking whether that's
// because it genuinely doesn't need one (frequency-domain phase
// manipulation is pitch-agnostic, PSOLAPitchShifter.h's own doc), or
// because windowSizeMs=30's by-ear confirmation (docs/PERFORMANCE_LOG.md's
// 2026-08-19 entry) was never actually checked against bass-register
// content specifically.

#include <DSP/PitchShifter.h>
#include <DSP/QualityMetrics.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <iomanip>
#include <iostream>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int analysisFftSize = 16384;
    constexpr int warmupBlocks = 20;

    std::vector<float> sineBlock (float freq, int blockIndex)
    {
        std::vector<float> block ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (blockIndex * blockSize + i) / (float) sampleRate;
            block[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }

    QualityMetrics::Result measure (float testFreq, float semitoneShift)
    {
        PitchShifter shifter (30, sampleRate); // windowSizeMs=30, matches PluginProcessor.h's current value
        std::vector<float> allOutput;
        allOutput.reserve ((size_t) blockSize * warmupBlocks);

        for (int block = 0; block < warmupBlocks; ++block)
        {
            const auto input = sineBlock (testFreq, block);
            std::vector<float> output (blockSize, 0.0f);
            shifter.shiftPitch (16, semitoneShift, input, output);
            allOutput.insert (allOutput.end(), output.begin(), output.end());
        }

        QualityMetrics metrics (analysisFftSize);
        const float targetFreq = testFreq * std::pow (2.0f, semitoneShift / 12.0f);
        return metrics.analyze (allOutput, sampleRate, targetFreq);
    }
}

TEST_CASE ("Phase vocoder: does artifact energy degrade at bass-register frequencies?", "[phase-vocoder][lowfreq]")
{
    std::cout << std::fixed << std::setprecision (4);
    std::cout << "\nPhase vocoder (windowSizeMs=30, frame=1024 samples @ 44.1kHz), unison (0st):\n";
    std::cout << "freqHz,periodsInFrame,thdPlusNPercent,thdPlusNValid,artifactEnergyPercent\n";

    // 1024-sample frame / period-at-freq = how many full cycles the
    // analysis window actually contains -- the same "does the window see
    // enough of the waveform" question PSOLA's minHz answers structurally.
    for (float freq : { 220.0f, 110.0f, 82.4f, 65.4f, 55.0f })
    {
        const auto result = measure (freq, 0.0f);
        const float periodsInFrame = 1024.0f / ((float) sampleRate / freq);
        std::cout << freq << "," << periodsInFrame << "," << result.thdPlusNPercent << ","
                   << (result.thdPlusNValid ? "true" : "false") << "," << result.artifactEnergyPercent << "\n";
    }
    std::cout << std::endl;
}
