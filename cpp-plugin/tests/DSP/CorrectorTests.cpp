#include <DSP/Corrector.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace pitchzazz;

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
