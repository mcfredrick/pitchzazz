//! Pure DSP for real-time pitch correction: pitch detection, scale
//! quantization, and pitch shifting.
//!
//! This crate deliberately has **no I/O and spawns no threads**. Real-time
//! audio callbacks (cpal, JUCE's `processBlock`, etc.) must never block,
//! allocate, or take locks — but pitch detection and pitch shifting
//! algorithms do all three internally (FFT/autocorrelation buffers, window
//! buffers). The only way to reconcile those two facts is to keep this code
//! off the audio thread entirely and hand data across a lock-free boundary,
//! which is what `pitch-cli` (or, later, a JUCE plugin) is responsible for.
//! See `docs/ARCHITECTURE.md` for the full real-time-safety design.

pub mod corrector;
pub mod scale;

pub use corrector::{hz_to_midi, midi_to_hz, CorrectionResult, PitchCorrector, StageTimings};
pub use scale::nearest_in_scale_midi;

// Re-exported so downstream crates build `Scale` values without taking a
// separate, possibly version-mismatched dependency on rust-music-theory.
pub use rust_music_theory::note::PitchClass;
pub use rust_music_theory::scale::{Direction, Mode, Scale, ScaleType};
