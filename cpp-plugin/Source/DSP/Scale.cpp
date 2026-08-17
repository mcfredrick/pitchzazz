#include "Scale.h"
#include <array>

namespace pitchzazz
{

namespace
{
    // Semitone offsets from the tonic. Ionian (major) and Aeolian
    // (natural minor) — the only two modes pitch-cli exposes.
    constexpr std::array<int, 7> majorIntervals { 0, 2, 4, 5, 7, 9, 11 };
    constexpr std::array<int, 7> minorIntervals { 0, 2, 3, 5, 7, 8, 10 };

    int pitchClassOf (int midiNote) noexcept
    {
        return ((midiNote % 12) + 12) % 12;
    }
}

bool Scale::containsPitchClass (int pitchClass) const noexcept
{
    const auto& intervals = (mode == ScaleMode::major) ? majorIntervals : minorIntervals;
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
