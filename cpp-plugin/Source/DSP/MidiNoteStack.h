#pragma once

#include <array>

namespace pitchzazz
{

/// Last-note-priority monophonic MIDI note tracking — no JUCE dependency
/// (plain `int` note numbers in, not `juce::MidiMessage`) so it's
/// unit-testable like the rest of DSP/, and reusable if a future caller
/// wants note-priority logic without a JUCE MidiBuffer to feed it. The
/// standard synth technique confirmed against JUCE's own API before
/// writing this: `juce::MidiKeyboardState` only tracks which keys are
/// down for UI purposes, and `juce::Synthesiser`'s voice allocator is
/// built for polyphonic instrument voices, not a single "what note is
/// active right now" scalar — neither one solves this directly.
///
/// Fixed-capacity (128 possible MIDI notes, `std::array`, no heap
/// allocation) so it's safe to drive from real-time-adjacent code —
/// PluginProcessor::processBlock calls `noteOn`/`noteOff` directly from
/// the audio thread, not the worker thread, the first state write in this
/// codebase to originate there rather than the message thread (see
/// PluginProcessor.cpp's processBlock for why that's fine: no allocation,
/// no lock, same real-time-safety bar as everything else in that
/// function).
class MidiNoteStack
{
public:
    /// Marks `noteNumber` (0-127) as held and makes it the active note —
    /// most-recently-pressed always wins, matching how a monophonic synth
    /// or Auto-Tune's own "Target Notes Via MIDI" mode behaves (confirmed
    /// against Auto-Tune's documented MIDI-mode behaviour, not assumed).
    void noteOn (int noteNumber) noexcept
    {
        if (noteNumber < 0 || noteNumber >= 128 || held[(size_t) noteNumber])
            return; // already held (a duplicate note-on) — order shouldn't change

        held[(size_t) noteNumber] = true;
        order[count++] = noteNumber; // pushed to the back == most recent
    }

    /// Marks `noteNumber` released. If it wasn't the active note, releasing
    /// it must not disturb whichever note *is* active — this removes it
    /// from wherever it sits in the priority order, not just the front.
    void noteOff (int noteNumber) noexcept
    {
        if (noteNumber < 0 || noteNumber >= 128 || ! held[(size_t) noteNumber])
            return;

        held[(size_t) noteNumber] = false;
        for (int i = 0; i < count; ++i)
        {
            if (order[(size_t) i] == noteNumber)
            {
                for (int j = i; j < count - 1; ++j)
                    order[(size_t) j] = order[(size_t) j + 1];
                --count;
                break;
            }
        }
    }

    /// The currently-held, highest-priority note, or -1 if none is held.
    [[nodiscard]] int activeNote() const noexcept
    {
        return count > 0 ? order[(size_t) (count - 1)] : -1;
    }

private:
    std::array<bool, 128> held {};
    std::array<int, 128> order {}; // order[0..count) oldest-to-newest; back is active
    int count = 0;
};

} // namespace pitchzazz
