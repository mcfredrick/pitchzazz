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
