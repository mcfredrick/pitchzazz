#include <DSP/Scale.h>
#include <catch2/catch_test_macros.hpp>

using namespace pitchzazz;

namespace
{
    Scale cMajor() { return { 0, ScaleMode::major }; } // C = pitch class 0

    int pitchClassOf (int midiNote) { return ((midiNote % 12) + 12) % 12; }
}

// Port of pitch-core's scale.rs test suite (crates/pitch-core/src/scale.rs)
// — same cases, same expected values, so parity between the Rust and C++
// engines is verified the same way on both sides.

TEST_CASE ("note already in scale is unchanged", "[scale]")
{
    const auto scale = cMajor();
    CHECK (nearestInScaleMidi (60, scale) == 60); // C4
    CHECK (nearestInScaleMidi (67, scale) == 67); // G4
}

TEST_CASE ("out of scale note snaps within one semitone, ties break low", "[scale]")
{
    const auto scale = cMajor();
    // C#4 = 61, not in C major; C4 (60) and D4 (62) are equidistant.
    CHECK (nearestInScaleMidi (61, scale) == 60);
}

TEST_CASE ("result is always in scale", "[scale]")
{
    const auto scale = cMajor();
    for (int midi = 48; midi < 84; ++midi)
    {
        const int snapped = nearestInScaleMidi (midi, scale);
        CHECK (scale.containsPitchClass (pitchClassOf (snapped)));
    }
}

TEST_CASE ("works across octaves, not just the scale's home octave", "[scale]")
{
    // Regression check for the original bug this design avoids: a
    // lookup table sized to one octave's worth of scale degrees would
    // mishandle notes far from that octave.
    const auto scale = cMajor();
    CHECK (nearestInScaleMidi (24, scale) == 24);   // C1, in scale
    CHECK (nearestInScaleMidi (120, scale) == 120); // C9, in scale
    CHECK (nearestInScaleMidi (121, scale) == 120); // C#9 -> C9
}
