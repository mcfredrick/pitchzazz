#include <DSP/PSOLACorrector.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <juce_core/juce_core.h>

using namespace pitchzazz;

// No pre-existing test file for PSOLACorrector itself (PSOLAPitchShifterTests.cpp
// only covers the shift stage) -- these cover the orchestration-level
// correction amount / retune speed controls (docs/ROADMAP.md Phase 5),
// mirroring CorrectorTests.cpp's equivalent cases so both engines are held
// to the same behavioural contract for these controls.

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

TEST_CASE ("PSOLACorrector: correction amount attenuates the applied shift", "[psola-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLACorrector fullCorrection (blockSize, sampleRate, scale);
    PSOLACorrector halfCorrection (blockSize, sampleRate, scale);
    halfCorrection.setCorrectionAmount (0.5f);
    PSOLACorrector noCorrection (blockSize, sampleRate, scale);
    noCorrection.setCorrectionAmount (0.0f);

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float fullShift = fullCorrection.process (input, sampleRate).semitoneShift;
    const float halfShift = halfCorrection.process (input, sampleRate).semitoneShift;
    const float noShift = noCorrection.process (input, sampleRate).semitoneShift;

    REQUIRE (std::abs (fullShift) > 0.05f);
    CHECK (std::abs (noShift) < 0.01f);
    CHECK (std::abs (halfShift - fullShift * 0.5f) < std::abs (fullShift) * 0.05f);
}

TEST_CASE ("PSOLACorrector: zero retune speed reproduces instant full-snap behaviour", "[psola-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLACorrector corrector (blockSize, sampleRate, scale);
    corrector.setRetuneSpeedMs (0.0f);

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const auto result = corrector.process (input, sampleRate);

    CHECK (std::abs (result.semitoneShift) > 0.05f);
}

TEST_CASE ("PSOLACorrector: positive retune speed glides towards the target instead of snapping instantly", "[psola-corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLACorrector instant (blockSize, sampleRate, scale);
    PSOLACorrector glided (blockSize, sampleRate, scale);
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
