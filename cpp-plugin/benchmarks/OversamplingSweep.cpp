// Mirrors crates/pitch-core/examples/oversampling_sweep.rs exactly — see
// that file's doc comment for why this measures cost and latency
// together rather than assuming from the algorithm's structure how
// over-sampling affects them.

#include <DSP/PitchShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <iostream>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    void measure (int overSampling)
    {
        // --- Cost: mean of 100 calls on a steady tone, warm-up excluded.
        PitchShifter costShifter (50, sampleRate);
        std::vector<float> signal ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
            signal[(size_t) i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sampleRate);
        std::vector<float> out ((size_t) blockSize, 0.0f);

        for (int i = 0; i < 5; ++i)
            costShifter.shiftPitch (overSampling, 0.0f, signal, out);

        double totalUs = 0.0;
        for (int i = 0; i < 100; ++i)
        {
            const auto start = std::chrono::steady_clock::now();
            costShifter.shiftPitch (overSampling, 0.0f, signal, out);
            totalUs += std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count();
        }
        const double meanCostUs = totalUs / 100.0;

        // --- Latency: impulse-response peak, same method as LatencyProbe.cpp.
        PitchShifter latencyShifter (50, sampleRate);
        const int impulseIndex = blockSize;
        const int totalLen = blockSize * 12;
        std::vector<float> input ((size_t) totalLen, 0.0f);
        input[(size_t) impulseIndex] = 1.0f;
        std::vector<float> output ((size_t) totalLen, 0.0f);

        for (int offset = 0; offset + blockSize <= totalLen; offset += blockSize)
        {
            std::vector<float> blockIn (input.begin() + offset, input.begin() + offset + blockSize);
            std::vector<float> blockOut ((size_t) blockSize, 0.0f);
            latencyShifter.shiftPitch (overSampling, 0.0f, blockIn, blockOut);
            std::copy (blockOut.begin(), blockOut.end(), output.begin() + offset);
        }

        int peakIndex = 0;
        float peakValue = 0.0f;
        for (int i = 0; i < totalLen; ++i)
        {
            if (std::abs (output[(size_t) i]) > peakValue)
            {
                peakValue = std::abs (output[(size_t) i]);
                peakIndex = i;
            }
        }
        const int latencySamples = juce::jmax (0, peakIndex - impulseIndex);
        const double latencyMs = ((double) latencySamples / sampleRate) * 1000.0;

        const double budgetUs = ((double) blockSize / sampleRate) * 1'000'000.0;
        std::cout << "over_sampling=" << overSampling << ": cost " << meanCostUs << "us ("
                   << (meanCostUs / budgetUs) * 100.0 << "% of " << budgetUs << "us budget), latency "
                   << latencyMs << "ms (" << latencySamples << " samples)" << std::endl;
    }
}

TEST_CASE ("Oversampling sweep: cost and latency", "[oversampling]")
{
    for (int overSampling : { 4, 8, 16, 32, 64 })
        measure (overSampling);
}
