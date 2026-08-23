// Per-block DSP cost of pitchzazz::PSOLACorrector against the real-time
// budget -- the same measurement CorrectorPerformance.cpp (phase vocoder)
// and VarispeedCorrectorPerformance.cpp already make, that PSOLA never had.
//
// This directly answers a real methodological question about every other
// PSOLA diagnostic in this investigation (PSOLACrackleProbe.cpp,
// PSOLAWaveformDump.cpp): those call shiftPitch() in a plain synchronous
// loop, giving the algorithm unlimited wall-clock time -- they can only
// ever show pure algorithmic/mathematical behavior given complete, on-time
// input. They cannot expose (or rule out) a genuine real-time underrun:
// if PSOLACorrector::process actually costs more than the block's real-time
// budget on the live worker thread, the ring buffer between it and the
// audio callback falls behind and the output gets silence-filled
// (docs/ARCHITECTURE.md's documented, correct failure mode) -- a
// completely different mechanism from anything the jitter/beat
// investigation could ever find, and one none of this session's probes
// were capable of detecting.

#include <DSP/PSOLACorrector.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <cmath>

using namespace pitchzazz;

namespace
{
    std::vector<float> testTone (double sampleRate, int numSamples)
    {
        // Real tone, not silence -- matches CorrectorPerformance.cpp's own
        // reasoning exactly: silence hits the early-exit power-threshold
        // path and never runs the actual detect/shift DSP.
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

TEST_CASE ("PSOLACorrector::process cost at 44100Hz", "[benchmark][psola]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    PSOLACorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);
    std::vector<float> output (blockSize);

    BENCHMARK ("process (44100Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate, output).semitoneShift;
    };
}

TEST_CASE ("PSOLACorrector::process cost at 48000Hz", "[benchmark][psola]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    PSOLACorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);
    std::vector<float> output (blockSize);

    BENCHMARK ("process (48000Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate, output).semitoneShift;
    };
}

TEST_CASE ("PSOLACorrector::process cost at 96000Hz", "[benchmark][psola]")
{
    constexpr double sampleRate = 96000.0;
    constexpr int blockSize = 2048;
    reportBudget (sampleRate, blockSize);

    PSOLACorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
    const auto signal = testTone (sampleRate, blockSize);
    std::vector<float> output (blockSize);

    BENCHMARK ("process (96000Hz, block 2048)")
    {
        return corrector.process (signal, sampleRate, output).semitoneShift;
    };
}
