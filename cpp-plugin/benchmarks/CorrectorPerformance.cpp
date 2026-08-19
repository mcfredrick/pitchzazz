// Per-block DSP cost of pitchzazz::Corrector — the C++ engine's
// equivalent of pitch-cli's --verbose StageTimings output
// (crates/pitch-cli/src/main.rs), at the same analysis block size (2048)
// and the sample rates PluginVal/auval exercised this plugin at. This is
// the comparable unit for the Phase 3 Rust-vs-C++ writeup — NOT
// PluginProcessor::processBlock (see AudioProcessing.cpp's updated
// comment for why that measures something different).

#include <DSP/Corrector.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <cmath>

using namespace pitchzazz;

namespace
{
    std::vector<float> testTone (double sampleRate, int numSamples)
    {
        // A real tone, not silence — silence hits Corrector::process's
        // early-exit power-threshold path and never runs the actual
        // detect/shift DSP, which would make this benchmark measure
        // nothing meaningful. 220Hz matches the sine used in
        // tests/DSP/PitchDetectorTests.cpp for consistency.
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

TEST_CASE ("Corrector::process cost at 44100Hz", "[benchmark]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    Corrector corrector (blockSize, sampleRate, 50, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);
    std::vector<float> output (blockSize);

    BENCHMARK ("process (44100Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate, output).semitoneShift;
    };
}

TEST_CASE ("Corrector::process cost at 48000Hz", "[benchmark]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    Corrector corrector (blockSize, sampleRate, 50, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);
    std::vector<float> output (blockSize);

    BENCHMARK ("process (48000Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate, output).semitoneShift;
    };
}

TEST_CASE ("Corrector::process cost at 96000Hz", "[benchmark]")
{
    constexpr double sampleRate = 96000.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    Corrector corrector (blockSize, sampleRate, 50, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);
    std::vector<float> output (blockSize);

    BENCHMARK ("process (96000Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate, output).semitoneShift;
    };
}
