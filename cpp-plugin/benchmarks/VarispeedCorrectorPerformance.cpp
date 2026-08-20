// Per-block DSP cost of pitchzazz::VarispeedCorrector — same methodology
// as CorrectorPerformance.cpp, comparable unit. Added specifically to
// investigate a live-measured budget overrun at 96kHz (docs/PERFORMANCE_LOG.md)
// — the 96kHz case here is the one that actually matters for that
// investigation, 44.1/48kHz included for the same cross-rate comparison
// the other two engines already have.

#include <DSP/VarispeedCorrector.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <cmath>

using namespace pitchzazz;

namespace
{
    std::vector<float> testTone (double sampleRate, int numSamples)
    {
        std::vector<float> samples ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            samples[(size_t) i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sampleRate);
        return samples;
    }

    void reportBudget (double sampleRate, int blockSize)
    {
        const double budgetUs = ((double) blockSize / sampleRate) * 1'000'000.0;
        INFO ("Real-time budget at " << sampleRate << "Hz, block " << blockSize << ": " << budgetUs << "us");
    }
}

TEST_CASE ("VarispeedCorrector::process cost at 44100Hz", "[benchmark]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    VarispeedCorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);

    BENCHMARK ("process (44100Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate).semitoneShift;
    };
}

TEST_CASE ("VarispeedCorrector::process cost at 48000Hz", "[benchmark]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    VarispeedCorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);

    BENCHMARK ("process (48000Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate).semitoneShift;
    };
}

TEST_CASE ("VarispeedCorrector::process cost at 96000Hz", "[benchmark]")
{
    constexpr double sampleRate = 96000.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    VarispeedCorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);

    BENCHMARK ("process (96000Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate).semitoneShift;
    };
}
