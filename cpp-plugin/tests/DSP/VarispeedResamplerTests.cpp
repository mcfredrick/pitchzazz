#include <DSP/PitchDetector.h>
#include <DSP/VarispeedResampler.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;

    std::vector<float> sineBlock (float freq, int count, int startSampleIndex)
    {
        std::vector<float> block ((size_t) count);
        for (int i = 0; i < count; ++i)
        {
            const float t = (float) (startSampleIndex + i) / (float) sampleRate;
            block[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }

    // Detected frequency of a resampled block, using this project's own
    // PitchDetector rather than an external analysis tool — same
    // round-trip-verification pattern PSOLAPitchShifterTests.cpp uses.
    float detectFrequency (const std::vector<float>& signal)
    {
        PitchDetector detector ((int) signal.size());
        return detector.detect (signal, sampleRate).frequencyHz;
    }
}

TEST_CASE ("ratio 1.0 reproduces the input exactly", "[varispeed-resampler]")
{
    // At ratio 1.0, readPos advances by exactly 1 sample per output
    // sample and starts at 0.0, so the fractional part of every read
    // position stays exactly 0.0 forever — the Catmull-Rom kernel
    // evaluated at t=0 returns y1 (the sample at the integer position)
    // exactly, with no interpolation error at all. This is a genuine
    // bit-exact identity, not a "close enough" tolerance case — except
    // for the last `kernelLookahead` positions, which correctly can't be
    // produced yet since they'd need samples one past what's been pushed.
    VarispeedResampler resampler;
    const auto input = sineBlock (220.0f, 512, 0);
    resampler.push (input.data(), (int) input.size());

    std::vector<float> output (input.size(), 0.0f);
    const int produced = resampler.pull (1.0f, output.data(), (int) output.size());

    REQUIRE (produced == (int) input.size() - 2);
    for (int i = 0; i < produced; ++i)
        CHECK (output[(size_t) i] == input[(size_t) i]);
}

TEST_CASE ("ratio 2.0 shifts the detected frequency up by an octave", "[varispeed-resampler]")
{
    VarispeedResampler resampler;
    // Push several periods' worth so the detector has enough voiced
    // signal to work with after resampling halves the sample count.
    const auto input = sineBlock (220.0f, 4096, 0);
    resampler.push (input.data(), (int) input.size());

    std::vector<float> output (input.size(), 0.0f);
    const int produced = resampler.pull (2.0f, output.data(), (int) output.size());
    output.resize ((size_t) produced);

    const float detected = detectFrequency (output);
    INFO ("detected: " << detected);
    CHECK (detected > 400.0f);
    CHECK (detected < 480.0f); // target 440Hz, loose tolerance — see PSOLA tests for precedent
}

TEST_CASE ("ratio 0.5 shifts the detected frequency down an octave", "[varispeed-resampler]")
{
    VarispeedResampler resampler;
    const auto input = sineBlock (220.0f, 4096, 0);
    resampler.push (input.data(), (int) input.size());

    std::vector<float> output (input.size(), 0.0f);
    const int produced = resampler.pull (0.5f, output.data(), (int) output.size());
    output.resize ((size_t) produced);

    const float detected = detectFrequency (output);
    INFO ("detected: " << detected);
    CHECK (detected > 100.0f);
    CHECK (detected < 120.0f); // target 110Hz
}

TEST_CASE ("pull returns fewer samples than requested when input runs out", "[varispeed-resampler]")
{
    VarispeedResampler resampler;
    const auto input = sineBlock (220.0f, 16, 0);
    resampler.push (input.data(), (int) input.size());

    // Requesting far more output than 16 pushed samples can support at
    // ratio 1.0 must stop short rather than reading past what's been
    // pushed — this is the class's whole contract for how a caller knows
    // to push more before pulling again.
    std::vector<float> output (64, 0.0f);
    const int produced = resampler.pull (1.0f, output.data(), (int) output.size());

    CHECK (produced < (int) output.size());
    CHECK (produced <= (int) input.size());
}

TEST_CASE ("push/pull remains correct across history-buffer wraparound", "[varispeed-resampler]")
{
    // Pushes and pulls one block at a time, well past the internal
    // history buffer's capacity, at ratio 1.0 (exact identity per the
    // first test above) — if circular-buffer indexing were wrong, this
    // would drift or corrupt once totalPushed wraps past history.size().
    VarispeedResampler resampler;
    constexpr int blockSize = 256;
    constexpr int numBlocks = 200; // 51200 samples, well past the 16384-sample history

    std::vector<float> allInput;
    std::vector<float> allOutput;
    allInput.reserve ((size_t) blockSize * numBlocks);
    allOutput.reserve (allInput.capacity());

    for (int b = 0; b < numBlocks; ++b)
    {
        const auto block = sineBlock (220.0f, blockSize, b * blockSize);
        resampler.push (block.data(), (int) block.size());
        allInput.insert (allInput.end(), block.begin(), block.end());

        std::vector<float> out (blockSize, 0.0f);
        const int produced = resampler.pull (1.0f, out.data(), blockSize);
        allOutput.insert (allOutput.end(), out.begin(), out.begin() + produced);
    }

    // Allow the fixed kernel lookahead's worth of trailing samples to be
    // un-pulled by the end (pull() only produces once idx0+2 is
    // available) rather than asserting the counts match exactly.
    REQUIRE (allOutput.size() >= allInput.size() - 8);
    for (size_t i = 0; i < allOutput.size(); ++i)
        CHECK (allOutput[i] == allInput[i]);
}
