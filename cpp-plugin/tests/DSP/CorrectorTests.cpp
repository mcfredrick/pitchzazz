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

TEST_CASE ("centsOffsetFromNearestNote", "[corrector]")
{
    // Exactly on a note: 0 cents.
    CHECK (std::abs (centsOffsetFromNearestNote (440.0f)) < 1e-2f);

    // A known sharp/flat offset: 440Hz * 2^(20/1200) is 20 cents sharp of
    // A4, and 440Hz * 2^(-20/1200) is 20 cents flat — derived directly
    // from the cents definition (100 cents/semitone, 1200 cents/octave)
    // rather than an approximated reference value.
    const float sharpHz = 440.0f * std::pow (2.0f, 20.0f / 1200.0f);
    CHECK (std::abs (centsOffsetFromNearestNote (sharpHz) - 20.0f) < 1e-1f);

    const float flatHz = 440.0f * std::pow (2.0f, -20.0f / 1200.0f);
    CHECK (std::abs (centsOffsetFromNearestNote (flatHz) - (-20.0f)) < 1e-1f);

    // Bounded to [-50, +50] by construction: halfway between two notes
    // rounds to one side or the other, never beyond half a semitone.
    const float halfwayHz = 440.0f * std::pow (2.0f, 0.5f / 12.0f);
    CHECK (std::abs (centsOffsetFromNearestNote (halfwayHz)) <= 50.0f + 1e-1f);
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

// Monophonic MIDI vocoder mode (docs/ROADMAP.md Phase 5).

TEST_CASE ("held MIDI note overrides scale quantization", "[corrector][midi-vocoder]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector scaleOnly (blockSize, sampleRate, 50, scale);
    Corrector midiTargeted (blockSize, sampleRate, 50, scale);
    midiTargeted.setMidiTargetNote (72); // C5 -- deliberately far from what scale-quantization would pick

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float scaleShift = scaleOnly.process (input, sampleRate).semitoneShift;
    const float midiShift = midiTargeted.process (input, sampleRate).semitoneShift;

    REQUIRE (std::abs (scaleShift) < 1.0f); // sanity: scale-quantize's target is close (A4 is already in C major)
    CHECK (midiShift > 2.0f); // MIDI's target (C5) is a real ~3-semitone jump, not the scale's small correction
}

TEST_CASE ("midi fallback scaleQuantize (the default) matches no-MIDI-targeting behaviour exactly", "[corrector][midi-vocoder]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector untouched (blockSize, sampleRate, 50, scale);
    Corrector explicitScaleMode (blockSize, sampleRate, 50, scale);
    explicitScaleMode.setMidiFallbackMode (MidiFallbackMode::scaleQuantize); // the default, set explicitly for clarity

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    CHECK (untouched.process (input, sampleRate).semitoneShift == explicitScaleMode.process (input, sampleRate).semitoneShift);
}

TEST_CASE ("midi fallback holdLastNote keeps correcting to the last-held note after release", "[corrector][midi-vocoder]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector corrector (blockSize, sampleRate, 50, scale);
    corrector.setMidiFallbackMode (MidiFallbackMode::holdLastNote);
    corrector.setMidiTargetNote (72); // C5

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    const float heldShift = corrector.process (input, sampleRate).semitoneShift;

    corrector.setMidiTargetNote (-1); // release
    const float afterReleaseShift = corrector.process (sineBlock (detunedFreq, sampleRate, blockSize, 1), sampleRate).semitoneShift;

    // Same target (C5) before and after release -- holdLastNote's whole
    // point. A loose tolerance, not bit-for-bit: this is about the
    // *target* matching, not incidental floating-point identity between
    // two separate process() calls.
    CHECK (std::abs (afterReleaseShift - heldShift) < 0.05f);
}

TEST_CASE ("midi fallback bypass applies no correction when no note is held", "[corrector][midi-vocoder]")
{
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector corrector (blockSize, sampleRate, 50, scale);
    corrector.setMidiFallbackMode (MidiFallbackMode::bypass);
    // No setMidiTargetNote call -- defaults to -1, i.e. never held.

    const auto input = sineBlock (detunedFreq, sampleRate, blockSize, 0);
    CHECK (corrector.process (input, sampleRate).semitoneShift == 0.0f);
}

TEST_CASE ("midi fallback silence fades out smoothly rather than cutting abruptly", "[corrector][midi-vocoder]")
{
    // Self-calibrated against the signal's own held-note amplitude, same
    // spirit as HotSwapDropoutTests.cpp's baseline comparison: a real
    // click would show up as the very first released sample dropping to
    // near-zero; a smooth ramp does not, even though the ramp is short
    // enough (silenceRampMs = 10ms) to be most of the way to silent well
    // before this block ends.
    const Scale scale { 0, ScaleMode::major };
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    Corrector corrector (blockSize, sampleRate, 50, scale);
    corrector.setMidiFallbackMode (MidiFallbackMode::silence);
    corrector.setMidiTargetNote (69); // A4 held

    float lastHeldBlockMaxAbs = 0.0f;
    for (int block = 0; block < 3; ++block)
    {
        const auto input = sineBlock (detunedFreq, sampleRate, blockSize, block);
        const auto result = corrector.process (input, sampleRate);
        lastHeldBlockMaxAbs = 0.0f;
        for (float s : result.samples)
            lastHeldBlockMaxAbs = std::max (lastHeldBlockMaxAbs, std::abs (s));
    }
    REQUIRE (lastHeldBlockMaxAbs > 0.1f); // sanity: really was audible while held

    corrector.setMidiTargetNote (-1); // release

    const auto releaseResult = corrector.process (sineBlock (detunedFreq, sampleRate, blockSize, 3), sampleRate);
    const float firstReleasedSampleAbs = std::abs (releaseResult.samples.front());
    CHECK (firstReleasedSampleAbs > lastHeldBlockMaxAbs * 0.1f); // did not cut to near-zero instantly

    // Enough further released blocks for the ramp to fully settle --
    // confirms silence mode actually reaches silence, not just avoids a
    // click on the way there.
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
