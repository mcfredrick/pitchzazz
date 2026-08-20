#include <DSP/PitchDetector.h>
#include <DSP/VarispeedShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <juce_core/juce_core.h>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    std::vector<float> sineBlock (float freq, int blockIndex)
    {
        std::vector<float> block ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (blockIndex * blockSize + i) / (float) sampleRate;
            block[(size_t) i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }

    float maxAdjacentDelta (const std::vector<float>& signal)
    {
        float maxDelta = 0.0f;
        for (size_t i = 1; i < signal.size(); ++i)
            maxDelta = std::max (maxDelta, std::abs (signal[i] - signal[i - 1]));
        return maxDelta;
    }
}

TEST_CASE ("VarispeedShifter: no internal discontinuity during its own startup fill-up", "[varispeed-shifter]")
{
    // Isolates whether a startup-period glitch is inherent to this class
    // (its own zero-fill for "not enough buffered output yet") or only
    // shows up via CorrectorWorker's hot-swap crossfade — run it
    // standalone, continuously, from a cold construction, with no swap
    // involved at all.
    VarispeedShifter shifter (sampleRate);
    std::vector<float> allOutput;
    allOutput.reserve ((size_t) blockSize * 10);

    for (int block = 0; block < 10; ++block)
    {
        const auto input = sineBlock (220.0f, block);
        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (0.0f, input, output);
        allOutput.insert (allOutput.end(), output.begin(), output.end());
    }

    const float delta = maxAdjacentDelta (allOutput);
    INFO ("max adjacent-sample delta across cold-start + 10 blocks: " << delta);
    // A continuous 0.5-amplitude sine's own natural max adjacent delta is
    // small (well under 0.1 at 220Hz/44.1kHz); a hard cut to/from silence
    // mid-waveform would show up as a jump on the order of the signal's
    // own amplitude (up to ~1.0 for two opposite-sign samples).
    CHECK (delta < 0.1f);
}

TEST_CASE ("VarispeedShifter: round-trip target frequency after warm-up", "[varispeed-shifter]")
{
    VarispeedShifter shifter (sampleRate);
    std::vector<float> lastOutput;

    for (int block = 0; block < 12; ++block)
    {
        const auto input = sineBlock (220.0f, block);
        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (12.0f, input, output); // +1 octave
        lastOutput = output;
    }

    PitchDetector detector (blockSize);
    const auto detected = detector.detect (lastOutput, sampleRate);
    INFO ("detected: " << detected.frequencyHz);
    CHECK (detected.frequencyHz > 400.0f);
    CHECK (detected.frequencyHz < 480.0f);
}
