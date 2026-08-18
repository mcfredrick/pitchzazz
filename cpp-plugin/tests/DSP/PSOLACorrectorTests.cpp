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

// Monophonic MIDI vocoder mode (docs/ROADMAP.md Phase 5) -- mirrors
// CorrectorTests.cpp's equivalent cases so both engines are held to the
// same behavioural contract.

TEST_CASE ("PSOLACorrector: held MIDI note overrides scale quantization", "[psola-corrector][midi-vocoder]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLACorrector scaleOnly (blockSize, sampleRate, scale);
    PSOLACorrector midiTargeted (blockSize, sampleRate, scale);
    midiTargeted.setMidiTargetNote (72); // C5

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float scaleShift = scaleOnly.process (input, sampleRate).semitoneShift;
    const float midiShift = midiTargeted.process (input, sampleRate).semitoneShift;

    REQUIRE (std::abs (scaleShift) < 1.0f);
    CHECK (midiShift > 2.0f);
}

TEST_CASE ("PSOLACorrector: midi fallback bypass applies no correction when no note is held", "[psola-corrector][midi-vocoder]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLACorrector corrector (blockSize, sampleRate, scale);
    corrector.setMidiFallbackMode (MidiFallbackMode::bypass);

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    CHECK (corrector.process (input, sampleRate).semitoneShift == 0.0f);
}

TEST_CASE ("PSOLACorrector: midi fallback silence fades out smoothly rather than cutting abruptly", "[psola-corrector][midi-vocoder]")
{
    // See CorrectorTests.cpp's equivalent case for the full reasoning.
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLACorrector corrector (blockSize, sampleRate, scale);
    corrector.setMidiFallbackMode (MidiFallbackMode::silence);
    corrector.setMidiTargetNote (69);

    float lastHeldBlockMaxAbs = 0.0f;
    for (int block = 0; block < 3; ++block)
    {
        const auto input = sineBlock (detunedFreq, sampleRate, blockSize, block);
        const auto result = corrector.process (input, sampleRate);
        lastHeldBlockMaxAbs = 0.0f;
        for (float s : result.samples)
            lastHeldBlockMaxAbs = std::max (lastHeldBlockMaxAbs, std::abs (s));
    }
    REQUIRE (lastHeldBlockMaxAbs > 0.1f);

    corrector.setMidiTargetNote (-1);

    const auto releaseResult = corrector.process (sineBlock (detunedFreq, sampleRate, blockSize, 3), sampleRate);
    CHECK (std::abs (releaseResult.samples.front()) > lastHeldBlockMaxAbs * 0.1f);

    float lastMaxAbs = 0.0f;
    for (int block = 4; block < 20; ++block)
    {
        const auto input = sineBlock (detunedFreq, sampleRate, blockSize, block);
        const auto result = corrector.process (input, sampleRate);
        lastMaxAbs = 0.0f;
        for (float s : result.samples)
            lastMaxAbs = std::max (lastMaxAbs, std::abs (s));
    }
    CHECK (lastMaxAbs < 1e-3f);
}
