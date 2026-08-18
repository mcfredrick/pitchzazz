#include "Scale.h"
#include <array>

namespace pitchzazz
{

namespace
{
    // Semitone offsets from the tonic for each of the seven modes of the
    // major scale — each is the major scale's own step pattern
    // (W W H W W W H) rotated to start at a different degree, e.g. Dorian
    // is the major pattern starting from its 2nd degree. Cross-checked by
    // construction: rotating from the 6th degree reproduces minorIntervals
    // exactly, confirming the derivation matches this file's pre-existing
    // (independently authored) major/minor pair.
    constexpr std::array<int, 7> majorIntervals { 0, 2, 4, 5, 7, 9, 11 };      // Ionian
    constexpr std::array<int, 7> dorianIntervals { 0, 2, 3, 5, 7, 9, 10 };
    constexpr std::array<int, 7> phrygianIntervals { 0, 1, 3, 5, 7, 8, 10 };
    constexpr std::array<int, 7> lydianIntervals { 0, 2, 4, 6, 7, 9, 11 };
    constexpr std::array<int, 7> mixolydianIntervals { 0, 2, 4, 5, 7, 9, 10 };
    constexpr std::array<int, 7> minorIntervals { 0, 2, 3, 5, 7, 8, 10 };      // Aeolian
    constexpr std::array<int, 7> locrianIntervals { 0, 1, 3, 5, 6, 8, 10 };

    const std::array<int, 7>& intervalsFor (ScaleMode mode) noexcept
    {
        switch (mode)
        {
            case ScaleMode::major:      return majorIntervals;
            case ScaleMode::minor:      return minorIntervals;
            case ScaleMode::dorian:     return dorianIntervals;
            case ScaleMode::phrygian:   return phrygianIntervals;
            case ScaleMode::lydian:     return lydianIntervals;
            case ScaleMode::mixolydian: return mixolydianIntervals;
            case ScaleMode::locrian:    return locrianIntervals;
        }
        return majorIntervals; // unreachable for a valid ScaleMode; see containsPitchClass's caller doc
    }

    int pitchClassOf (int midiNote) noexcept
    {
        return ((midiNote % 12) + 12) % 12;
    }
}

bool Scale::containsPitchClass (int pitchClass) const noexcept
{
    const auto& intervals = intervalsFor (mode);
    const int relative = pitchClassOf (pitchClass - tonicPitchClass);
    for (auto interval : intervals)
        if (interval == relative)
            return true;
    return false;
}

int nearestInScaleMidi (int midiNote, const Scale& scale) noexcept
{
    if (scale.containsPitchClass (pitchClassOf (midiNote)))
        return midiNote;

    for (int distance = 1; distance <= 6; ++distance)
    {
        const int below = midiNote - distance;
        if (below >= 0 && scale.containsPitchClass (pitchClassOf (below)))
            return below;

        const int above = midiNote + distance;
        if (above <= 127 && scale.containsPitchClass (pitchClassOf (above)))
            return above;
    }

    // Only reachable if a scale somehow contained zero pitch classes,
    // which containsPitchClass() never produces (both modes have 7) —
    // kept as a safe fallback rather than UB, mirroring the Rust
    // version's same defensive fallback.
    return midiNote;
}

} // namespace pitchzazz
