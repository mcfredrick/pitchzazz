//! Scale quantization: snapping a detected pitch to the nearest note in a
//! musical scale.
//!
//! # Design note (fixed a real bug here)
//!
//! An earlier prototype of this project built a lookup table of in-scale
//! MIDI notes by walking `Scale::intervals` and writing into a `Vec`
//! pre-sized to `scale.notes().len()`. That silently assumed
//! `intervals.len() == notes.len()`, which does not hold for every
//! `ScaleType`/`Mode` combination in `rust-music-theory` — depending on
//! whether the octave-closing interval is included, the loop either wrote
//! past the end of the vector or left trailing zeroed (bogus) entries,
//! producing wrong "nearest note" results. That was the actual reason this
//! project stalled originally: the pitch detection and pitch shifting both
//! worked, but note-snapping was silently wrong for some scales.
//!
//! The fix here sidesteps the interval bookkeeping entirely: instead of
//! reconstructing scale-degree MIDI numbers from intervals, we read the
//! scale's pitch classes directly from `Scale::notes()` (the crate's own
//! source of truth) and do a simple outward semitone search from the target
//! note. Fewer invariants to get right, and it's trivially testable against
//! every scale type.

use rust_music_theory::note::Notes;
use rust_music_theory::scale::Scale;
use std::collections::HashSet;

fn pitch_class_of_midi(midi_note: u8) -> u8 {
    midi_note % 12
}

// `rust_music_theory::note::PitchClass` doesn't implement `Hash`, so the
// set is keyed on its `u8` encoding instead — cheap, and sidesteps needing
// to newtype-wrap a type we don't own just to add a derive.
fn scale_pitch_classes(scale: &Scale) -> HashSet<u8> {
    scale
        .notes()
        .iter()
        .map(|n| n.pitch_class.into_u8())
        .collect()
}

/// Returns the nearest MIDI note (0-127) whose pitch class is in `scale`,
/// searching outward from `midi_note` by increasing semitone distance.
/// On ties (a note equidistant above and below), the lower note wins —
/// documented and tested below, since "which way ties break" is exactly
/// the kind of detail that's invisible until someone asks about it.
///
/// Returns `midi_note` unchanged if it is already in the scale, or if the
/// scale contains all twelve pitch classes (chromatic).
pub fn nearest_in_scale_midi(midi_note: u8, scale: &Scale) -> u8 {
    let allowed = scale_pitch_classes(scale);

    if allowed.contains(&pitch_class_of_midi(midi_note)) {
        return midi_note;
    }

    for distance in 1..=6u8 {
        if let Some(below) = midi_note.checked_sub(distance) {
            if allowed.contains(&pitch_class_of_midi(below)) {
                return below;
            }
        }
        let above = midi_note as u16 + distance as u16;
        if above <= 127 && allowed.contains(&pitch_class_of_midi(above as u8)) {
            return above as u8;
        }
    }

    // Only reachable if `allowed` is empty, which Scale::notes() never
    // produces — kept as a safe fallback rather than a panic in case a
    // future scale type surprises us.
    midi_note
}

#[cfg(test)]
mod tests {
    use super::*;
    use rust_music_theory::note::PitchClass;
    use rust_music_theory::scale::{Direction, Mode, Scale, ScaleType};

    fn c_major() -> Scale {
        Scale::new(
            ScaleType::Diatonic,
            PitchClass::C,
            4,
            Some(Mode::Ionian),
            Direction::Ascending,
        )
        .unwrap()
    }

    // Same tonic (C) as c_major() above, different mode: C Dorian is
    // {C,D,Eb,F,G,A,Bb} vs. C major's {C,D,E,F,G,A,B} — differs at the
    // 3rd and 7th degrees, so this actually exercises the mode-rotation
    // logic rather than just re-testing "does the crate accept a tonic."
    fn c_dorian() -> Scale {
        Scale::new(
            ScaleType::Diatonic,
            PitchClass::C,
            4,
            Some(Mode::Dorian),
            Direction::Ascending,
        )
        .unwrap()
    }

    #[test]
    fn note_already_in_scale_is_unchanged() {
        let scale = c_major();
        // C4 = MIDI 60, already in C major.
        assert_eq!(nearest_in_scale_midi(60, &scale), 60);
        // G4 = MIDI 67, also in C major.
        assert_eq!(nearest_in_scale_midi(67, &scale), 67);
    }

    #[test]
    fn out_of_scale_note_snaps_within_one_semitone() {
        let scale = c_major();
        // C#4 = MIDI 61, not in C major; nearest is C4 (60) or D4 (62),
        // both one semitone away. Ties break toward the lower note.
        assert_eq!(nearest_in_scale_midi(61, &scale), 60);
    }

    #[test]
    fn result_is_always_in_scale() {
        let scale = c_major();
        for midi in 48u8..84 {
            let snapped = nearest_in_scale_midi(midi, &scale);
            let pc = pitch_class_of_midi(snapped);
            assert!(
                scale_pitch_classes(&scale).contains(&pc),
                "snapped note {snapped} (from {midi}) is not in scale"
            );
        }
    }

    #[test]
    fn dorian_mode_snaps_to_a_different_note_than_ionian_at_the_same_tonic() {
        let dorian = c_dorian();
        // E4 = MIDI 64: in C major (Ionian) but not in C Dorian, whose 3rd
        // degree is Eb, not E. If mode rotation were silently ignored (the
        // exact class of bug this project's Phase 0 stalled on for a
        // different reason — see this file's module doc), this would
        // wrongly report E as already in-scale instead of snapping it.
        assert_ne!(nearest_in_scale_midi(64, &dorian), 64);
        // Eb4 = MIDI 63 is C Dorian's 3rd degree and should be unchanged.
        assert_eq!(nearest_in_scale_midi(63, &dorian), 63);
    }

    #[test]
    fn works_across_octaves_not_just_the_scales_home_octave() {
        // Regression check for the original bug: it built a table sized to
        // one octave's worth of scale degrees, so notes far from that
        // octave could index out of range or match garbage.
        let scale = c_major();
        assert_eq!(nearest_in_scale_midi(24, &scale), 24); // C1, in scale
        assert_eq!(nearest_in_scale_midi(120, &scale), 120); // C9, in scale
        assert_eq!(nearest_in_scale_midi(121, &scale), 120); // C#9 -> C9
    }
}
