#include <DSP/Scale.h>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>

using namespace pitchzazz;

namespace
{
    Scale cMajor() { return { 0, ScaleMode::major }; } // C = pitch class 0
    Scale cDorian() { return { 0, ScaleMode::dorian }; }

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

TEST_CASE ("Dorian mode snaps to a different note than Ionian at the same tonic", "[scale]")
{
    // C Dorian is {C,D,Eb,F,G,A,Bb} vs. C major's {C,D,E,F,G,A,B} — differs
    // at the 3rd and 7th degrees, so this exercises intervalsFor()'s mode
    // dispatch rather than re-testing "does containsPitchClass work at
    // all." Mirrors pitch-core's own Dorian test (scale.rs) exactly.
    const auto scale = cDorian();
    CHECK (nearestInScaleMidi (64, scale) != 64); // E4: in C major, not in C Dorian (Eb, not E)
    CHECK (nearestInScaleMidi (63, scale) == 63); // Eb4: C Dorian's 3rd degree, unchanged
}

TEST_CASE ("no currently-implemented mode ever requires more than a 1-semitone correction", "[scale][grain-width-fix]")
{
    // The real invariant docs/FINDINGS.md #27 rests on: every mode this
    // project implements is a rotation of the same major-scale step
    // pattern (W W H W W W H, see Scale.cpp's intervalsFor()), which
    // means the largest gap between two adjacent scale degrees is always
    // 2 semitones -- so the worst a chromatic note can ever be from its
    // nearest in-scale neighbor is exactly half that, 1 semitone. This
    // is what bounds PSOLACorrector's real-world shift magnitude to
    // ~1-1.5 semitones (scale correction plus ordinary intonation slop),
    // never anywhere near the +6/+9/+12 range
    // chooseGrainWidthMultiplierForShift() was built for.
    //
    // Exhaustive, not spot-checked: every tonic pitch class x every mode
    // x every chromatic semitone in a two-octave span, so a future mode
    // with a wider gap (if one were ever added) would fail this test
    // rather than silently violating the assumption the PSOLA decision
    // in docs/FINDINGS.md #27 depends on.
    const ScaleMode modes[] = {
        ScaleMode::major, ScaleMode::minor, ScaleMode::dorian, ScaleMode::phrygian,
        ScaleMode::lydian, ScaleMode::mixolydian, ScaleMode::locrian
    };

    int maxCorrection = 0;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (auto mode : modes)
        {
            const Scale scale { tonic, mode };
            for (int midi = 48; midi < 72; ++midi) // two chromatic octaves, well clear of the range-edge cases the octave test above already covers
            {
                const int snapped = nearestInScaleMidi (midi, scale);
                maxCorrection = std::max (maxCorrection, std::abs (snapped - midi));
            }
        }
    }

    CHECK (maxCorrection == 1);
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
