#include <DSP/Corrector.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <juce_core/juce_core.h>

using namespace pitchzazz;

namespace
{
    // A5-ish tone deliberately detuned sharp: A is diatonic in C major (no
    // quantization needed), but the ~40-cent detune still produces a
    // real, small, nonzero raw semitone shift to correct -- the "singer
    // is slightly pitchy" case correction amount/retune speed are meant
    // for, as opposed to a big jump between two different scale notes.
    constexpr float detunedFreq = 450.0f;

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

// Port of pitch-core's corrector.rs test suite (crates/pitch-core/src/corrector.rs).

TEST_CASE ("hz/midi roundtrip", "[corrector]")
{
    const float midi = hzToMidi (440.0f);
    CHECK (std::abs (midi - 69.0f) < 1e-4f);

    const float hz = midiToHz (69.0f);
    CHECK (std::abs (hz - 440.0f) < 1e-3f);
}

TEST_CASE ("silence produces no shift", "[corrector]")
{
    const Scale scale { 0, ScaleMode::major }; // C major
    const int blockSize = 1024;
    Corrector corrector (blockSize, 44100.0, 50, scale);

    const std::vector<float> silence (blockSize, 0.0f);
    const auto result = corrector.process (silence, 44100.0);

    CHECK (result.semitoneShift == 0.0f);
    CHECK (result.samples.size() == (size_t) blockSize);
}

TEST_CASE ("correction amount attenuates the applied shift", "[corrector]")
{
    const Scale scale { 0, ScaleMode::major }; // C major
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector fullCorrection (blockSize, sampleRate, 50, scale);
    Corrector halfCorrection (blockSize, sampleRate, 50, scale);
    halfCorrection.setCorrectionAmount (0.5f);
    Corrector noCorrection (blockSize, sampleRate, 50, scale);
    noCorrection.setCorrectionAmount (0.0f);

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float fullShift = fullCorrection.process (input, sampleRate).semitoneShift;
    const float halfShift = halfCorrection.process (input, sampleRate).semitoneShift;
    const float noShift = noCorrection.process (input, sampleRate).semitoneShift;

    REQUIRE (std::abs (fullShift) > 0.05f); // sanity: the tone really is off-pitch enough to produce a real shift
    CHECK (std::abs (noShift) < 0.01f);
    // All three correctors detect the same input deterministically (no
    // internal randomness), so half-amount should land close to exactly
    // half of full-amount's shift -- a loose-but-real tolerance, not a
    // hand-picked pass threshold.
    CHECK (std::abs (halfShift - fullShift * 0.5f) < std::abs (fullShift) * 0.05f);
}

TEST_CASE ("zero retune speed reproduces instant full-snap behaviour on the very first block", "[corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector corrector (blockSize, sampleRate, 50, scale);
    corrector.setRetuneSpeedMs (0.0f); // the default -- explicit here for clarity

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const auto result = corrector.process (input, sampleRate);

    // No glide state to warm up: the very first block already reflects
    // the full computed shift rather than a partial approach to it.
    CHECK (std::abs (result.semitoneShift) > 0.05f);
}

TEST_CASE ("positive retune speed glides towards the target instead of snapping instantly", "[corrector]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector instant (blockSize, sampleRate, 50, scale);
    Corrector glided (blockSize, sampleRate, 50, scale);
    glided.setRetuneSpeedMs (500.0f); // slow, clearly audible glide

    const auto firstInput = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float instantShift = instant.process (firstInput, sampleRate).semitoneShift;
    const float glidedFirstBlockShift = glided.process (firstInput, sampleRate).semitoneShift;

    REQUIRE (std::abs (instantShift) > 0.05f);
    // First block hasn't had time to glide anywhere close to the full
    // shift yet -- the entire behavioural difference this feature exists
    // to produce.
    CHECK (std::abs (glidedFirstBlockShift) < std::abs (instantShift) * 0.5f);

    // Feed enough further blocks of the same steady tone for the glide to
    // converge -- confirms it actually reaches the target eventually
    // rather than permanently lagging behind it.
    float lastShift = glidedFirstBlockShift;
    for (int block = 1; block < 40; ++block)
    {
        const auto blockInput = sineBlock (detunedFreq, sampleRate, blockSize, block);
        lastShift = glided.process (blockInput, sampleRate).semitoneShift;
    }
    CHECK (std::abs (lastShift - instantShift) < std::abs (instantShift) * 0.1f);
}
