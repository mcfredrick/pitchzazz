// Mirrors PSOLALatencyProbe.cpp's shape: a Catch2 test case that exists to
// print real, measured numbers (via std::cout) rather than to assert
// anything itself — this is the data source docs/TESTING.md's new
// "Objective quality metrics" section and QualityMetricsTests.cpp's
// thresholds are both set from, not a guessed/estimated table.
//
// Deliberately drives the three shifter classes directly
// (PitchShifter::shiftPitch / PSOLAPitchShifter::shiftPitch /
// VarispeedShifter::shiftPitch), not the full PitchEngine::process()
// pipeline: all three take an explicit, controllable semitoneShift
// directly, matching every existing shifter-level test in this codebase
// (PitchShifterTests.cpp, PSOLAPitchShifterTests.cpp,
// VarispeedShifterTests.cpp all do the same). RustCorrectorEngine has no
// such raw-shift entry point — its only exposed call is the full
// detect+quantize+shift process() — so it's intentionally left out of
// this sweep rather than forced through an indirect, less-controllable
// path; it shares its actual DSP with the native phase-vocoder engine
// (ported, not reimplemented — docs/COMPARISON.md), so the native
// engine's numbers below are the representative ones for that mechanism.

#include <DSP/PitchShifter.h>
#include <DSP/PSOLAPitchShifter.h>
#include <DSP/QualityMetrics.h>
#include <DSP/VarispeedShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int analysisFftSize = 16384;
    // 20 blocks = 40960 samples, several times over every engine's own
    // algorithmic latency (phase vocoder ~2048, PSOLA ~1378 worst-case,
    // Varispeed's WSOLA+resampler combined) — the trailing analysisFftSize
    // samples QualityMetrics::analyze() actually reads are comfortably
    // past warm-up for all three, not just the ones with the shortest
    // latency.
    constexpr int warmupBlocks = 20;
    constexpr float testFreq = 220.0f; // A3, this project's standard test tone

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

    using RenderFn = std::function<void (const std::vector<float>&, std::vector<float>&)>;

    QualityMetrics::Result measure (const RenderFn& render, float semitoneShift)
    {
        std::vector<float> allOutput;
        allOutput.reserve ((size_t) blockSize * warmupBlocks);

        for (int block = 0; block < warmupBlocks; ++block)
        {
            const auto input = sineBlock (testFreq, block);
            std::vector<float> output (blockSize, 0.0f);
            render (input, output);
            allOutput.insert (allOutput.end(), output.begin(), output.end());
        }

        QualityMetrics metrics (analysisFftSize);
        const float targetFreq = testFreq * std::pow (2.0f, semitoneShift / 12.0f);
        return metrics.analyze (allOutput, sampleRate, targetFreq);
    }
}

TEST_CASE ("Quality-metrics probe: THD+N and artifact energy across engines and shift amounts", "[quality]")
{
    std::cout << std::fixed << std::setprecision (4);
    std::cout << "\nengine,semitoneShift,thdPlusNPercent,thdPlusNValid,artifactEnergyPercent\n";

    const auto printRow = [] (const char* engine, float shift, const QualityMetrics::Result& result)
    {
        std::cout << engine << "," << shift << "," << result.thdPlusNPercent << "," << (result.thdPlusNValid ? "true" : "false")
                   << "," << result.artifactEnergyPercent << "\n";
    };

    for (float shift : { -12.0f, -3.0f, 0.0f, 3.0f, 12.0f })
    {
        {
            PitchShifter shifter (50, sampleRate);
            const auto result = measure (
                [&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (16, shift, in, out); }, shift);
            printRow ("phase-vocoder", shift, result);
        }
        {
            PSOLAPitchShifter shifter (sampleRate);
            const auto result = measure (
                [&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (testFreq, shift, in, out); }, shift);
            printRow ("psola", shift, result);
        }
        {
            VarispeedShifter shifter (sampleRate);
            const auto result = measure (
                [&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (shift, in, out); }, shift);
            printRow ("varispeed", shift, result);
        }
    }
    std::cout << std::endl;
}

TEST_CASE ("Quality-metrics probe: PSOLA before/after chooseGrainWidthMultiplierForShift", "[quality][grain-width-fix]")
{
    std::cout << std::fixed << std::setprecision (4);
    std::cout << "\nshift,grainWidthMultiplier,thdPlusNPercent,thdPlusNValid,artifactEnergyPercent\n";

    // 0.5/1.0/1.5 semitones added specifically to check the actual
    // operating range this plugin's scale-quantization ever requests:
    // nearestInScaleMidi() over any of the 7 currently-implemented modes
    // (all rotations of the same step pattern, max 2-semitone gap
    // between scale degrees) mathematically cannot request more than 1
    // semitone of scale-driven correction -- confirmed by exhaustive
    // enumeration over all 12 tonics x 7 modes x 12 chromatic notes, not
    // just typical-case reasoning. 1.5 covers that 1 semitone plus
    // realistic intonation slop on top (the *continuous*, unrounded
    // pitch can add up to another ~0.5 semitone in the same direction).
    for (float shift : { -12.0f, -3.0f, -1.5f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.5f, 3.0f, 6.0f, 9.0f, 12.0f })
    {
        // Before: default multiplier (1.0), same as every number in the
        // table above.
        {
            PSOLAPitchShifter shifter (sampleRate);
            const auto result = measure (
                [&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (testFreq, shift, in, out); }, shift);
            std::cout << shift << ",1.0000(before)," << result.thdPlusNPercent << "," << (result.thdPlusNValid ? "true" : "false")
                       << "," << result.artifactEnergyPercent << "\n";
        }
        // After: the theory-derived multiplier for this shift.
        {
            const float multiplier = chooseGrainWidthMultiplierForShift (shift);
            PSOLAPitchShifter shifter (sampleRate);
            shifter.setGrainWidthMultiplier (multiplier);
            const auto result = measure (
                [&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (testFreq, shift, in, out); }, shift);
            std::cout << shift << "," << multiplier << "(after)," << result.thdPlusNPercent << "," << (result.thdPlusNValid ? "true" : "false")
                       << "," << result.artifactEnergyPercent << "\n";
        }
    }
    std::cout << std::endl;
}
