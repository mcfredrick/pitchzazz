#include <DSP/VarispeedCorrector.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <juce_core/juce_core.h>

using namespace pitchzazz;

// Mirrors PSOLACorrectorTests.cpp's cases exactly, holding Varispeed to
// the same orchestration-level behavioural contract (correction amount,
// retune speed) as both other engines — same controls, same formulas
// (RetuneSmoothing.h), so this is about verifying the wiring, not
// re-deriving the math.

namespace
{
    constexpr float detunedFreq = 450.0f; // see CorrectorTests.cpp for why this specific value

    std::vector<float> sineBlock (float freq, double sampleRate, int blockSize, int blockIndex)
    {
        std::vector<float> block ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (blockIndex * blockSize + i) / (float) sampleRate;
            block[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }
}

TEST_CASE ("VarispeedCorrector: correction amount attenuates the applied shift", "[varispeed-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    VarispeedCorrector fullCorrection (blockSize, sampleRate, scale);
    VarispeedCorrector halfCorrection (blockSize, sampleRate, scale);
    halfCorrection.setCorrectionAmount (0.5f);
    VarispeedCorrector noCorrection (blockSize, sampleRate, scale);
    noCorrection.setCorrectionAmount (0.0f);

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float fullShift = fullCorrection.process (input, sampleRate).semitoneShift;
    const float halfShift = halfCorrection.process (input, sampleRate).semitoneShift;
    const float noShift = noCorrection.process (input, sampleRate).semitoneShift;

    REQUIRE (std::abs (fullShift) > 0.05f);
    CHECK (std::abs (noShift) < 0.01f);
    CHECK (std::abs (halfShift - fullShift * 0.5f) < std::abs (fullShift) * 0.05f);
}

TEST_CASE ("VarispeedCorrector: zero retune speed reproduces instant full-snap behaviour", "[varispeed-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    VarispeedCorrector corrector (blockSize, sampleRate, scale);
    corrector.setRetuneSpeedMs (0.0f);

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const auto result = corrector.process (input, sampleRate);

    CHECK (std::abs (result.semitoneShift) > 0.05f);
}

TEST_CASE ("VarispeedCorrector: positive retune speed glides towards the target instead of snapping instantly", "[varispeed-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    VarispeedCorrector instant (blockSize, sampleRate, scale);
    VarispeedCorrector glided (blockSize, sampleRate, scale);
    glided.setRetuneSpeedMs (500.0f);

    const auto firstInput = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float instantShift = instant.process (firstInput, sampleRate).semitoneShift;
    const float glidedFirstBlockShift = glided.process (firstInput, sampleRate).semitoneShift;

    REQUIRE (std::abs (instantShift) > 0.05f);
    CHECK (std::abs (glidedFirstBlockShift) < std::abs (instantShift) * 0.5f);

    float lastShift = glidedFirstBlockShift;
    for (int block = 1; block < 40; ++block)
    {
        const auto blockInput = sineBlock (detunedFreq, sampleRate, blockSize, block);
        lastShift = glided.process (blockInput, sampleRate).semitoneShift;
    }
    CHECK (std::abs (lastShift - instantShift) < std::abs (instantShift) * 0.1f);
}

TEST_CASE ("VarispeedCorrector: getLatencySamples is the sum of both shift stages, fixed and ratio-independent", "[varispeed-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    VarispeedCorrector corrector (blockSize, sampleRate, scale);
    const int before = corrector.getLatencySamples();
    CHECK (before > 0);

    // Feed a strongly detuned, then a barely-detuned block (large vs.
    // small applied shift) — latency must not move either way, the whole
    // point of the WSOLA-then-resample ordering (VarispeedShifter's doc).
    const auto detuned = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const auto inTune = sineBlock (440.0f, sampleRate, blockSize, 1);
    corrector.process (detuned, sampleRate);
    corrector.process (inTune, sampleRate);

    CHECK (corrector.getLatencySamples() == before);
}
