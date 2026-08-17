#include <DSP/PitchShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace pitchzazz;

namespace
{
    float rms (const std::vector<float>& v)
    {
        float sumSq = 0.0f;
        for (float s : v)
            sumSq += s * s;
        return std::sqrt (sumSq / (float) v.size());
    }
}

// No Rust-side equivalent (the Rust engine relies on the pitch_shift
// crate's own tests, of which there are none — it has no test suite at
// all). This exists specifically to catch the JUCE-vs-realfft FFT
// normalization mismatch documented in PitchShifter.cpp: get the
// compensating accOversamp factor wrong in either direction and this
// either goes silent or blows up, both of which this test would catch.

TEST_CASE ("zero semitone shift roughly preserves signal energy", "[pitch-shifter]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int overSampling = 8;
    constexpr float freq = 220.0f;

    PitchShifter shifter (50, sampleRate);

    std::vector<float> lastOutput;
    // The phase vocoder has internal FIFO latency (frameSize - step
    // samples) before output reflects real processing — run several
    // blocks so we're well past that before asserting anything.
    for (int block = 0; block < 8; ++block)
    {
        std::vector<float> input (blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (block * blockSize + i) / (float) sampleRate;
            input[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }

        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (overSampling, 0.0f, input, output);
        lastOutput = output;
    }

    const float inputRms = 1.0f / std::sqrt (2.0f); // RMS of a unit sine
    const float outputRms = rms (lastOutput);

    // Loose bounds deliberately: this is a normalization sanity check
    // (catching "silent" or "exploding" output), not a precision
    // assertion about phase-vocoder reconstruction quality.
    CHECK (outputRms > inputRms * 0.3f);
    CHECK (outputRms < inputRms * 3.0f);
}
