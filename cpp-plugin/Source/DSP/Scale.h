#pragma once

namespace pitchzazz
{

/// Which diatonic mode a Scale uses. Mirrors the two modes pitch-cli
/// actually exposes via --mode (major/minor) — see parse_scale() in the
/// Rust crates/pitch-cli/src/main.rs. Not a general music-theory library:
/// this project only ever needs Ionian/Aeolian, so that's all that's
/// ported from rust-music-theory's much larger surface.
enum class ScaleMode
{
    major, // Ionian
    minor  // Aeolian
};

/// A diatonic scale: a tonic pitch class (0 = C .. 11 = B) plus a mode.
struct Scale
{
    int tonicPitchClass = 0; // 0-11, 0 = C
    ScaleMode mode = ScaleMode::major;

    /// True if `pitchClass` (0-11) is a member of this scale.
    [[nodiscard]] bool containsPitchClass (int pitchClass) const noexcept;
};

/// Returns the nearest MIDI note (0-127) whose pitch class is in `scale`,
/// searching outward from `midiNote` by increasing semitone distance. On
/// ties (a note equidistant above and below), the lower note wins.
///
/// Direct port of pitch-core's nearest_in_scale_midi
/// (crates/pitch-core/src/scale.rs) — see that file's module doc for the
/// scale-quantization bug this design avoids: a prior prototype built a
/// lookup table from Scale::intervals() sized to notes().len(), which
/// silently mismatched for some scale/mode combinations and produced
/// wrong "nearest note" results. This port sidesteps that class of bug by
/// construction — pitch-class membership is a fixed interval pattern
/// rotated by the tonic, not a reconstructed table — the same fix
/// rationale as the Rust side, just with no table to get wrong in the
/// first place.
///
/// Returns `midiNote` unchanged if it is already in the scale.
[[nodiscard]] int nearestInScaleMidi (int midiNote, const Scale& scale) noexcept;

} // namespace pitchzazz
