//! Orchestrates detection -> scale quantization -> pitch shifting for one
//! block of audio. See the module docs in `lib.rs` for why this type is
//! deliberately kept off the real-time audio thread.

use crate::scale::nearest_in_scale_midi;
use pitch_detection::detector::internals::Pitch;
use pitch_detection::detector::mcleod::McLeodDetector;
use pitch_detection::detector::PitchDetector;
use pitch_shift::PitchShifter;
use rust_music_theory::scale::Scale;
use std::time::Instant;

const POWER_THRESHOLD: f32 = 0.15;
const CLARITY_THRESHOLD: f32 = 0.1;
// Was 32 in the original prototype, carried over unmeasured. Live
// profiling (docs/PERFORMANCE_LOG.md, 2026-08-17 entries) showed the shift
// stage alone consistently exceeding the entire real-time budget
// regardless of input content — a fixed cost, not content-dependent, which
// pointed at this parameter rather than the algorithm's input-dependent
// behavior. Dropping to 8 brought most blocks comfortably under budget
// (~4x reduction in shift-stage cost, matching the 32/8 ratio). See the
// log for the full before/after data and the still-open tail-latency
// question before changing this again.
const OVER_SAMPLING: usize = 8;

/// Standard equal-temperament Hz -> fractional MIDI note number, A4 = 69 = 440Hz.
pub fn hz_to_midi(freq_hz: f32) -> f32 {
    69.0 + 12.0 * (freq_hz / 440.0).log2()
}

/// Inverse of [`hz_to_midi`].
pub fn midi_to_hz(midi: f32) -> f32 {
    440.0 * 2f32.powf((midi - 69.0) / 12.0)
}

/// Wall-clock time spent in each stage of one [`PitchCorrector::process`]
/// call, in microseconds.
///
/// Kept as its own type rather than inline fields on [`CorrectionResult`]
/// so the same shape can be mirrored by the future C++ port (Phase 2) —
/// the Rust-vs-C++ comparison in Phase 3 only means something if both
/// sides report cost the same way. See `docs/ROADMAP.md`.
#[derive(Debug, Clone, Copy, Default)]
pub struct StageTimings {
    pub detect_us: f64,
    pub quantize_us: f64,
    pub shift_us: f64,
}

impl StageTimings {
    pub fn total_us(&self) -> f64 {
        self.detect_us + self.quantize_us + self.shift_us
    }
}

/// What one call to [`PitchCorrector::process`] produced, plus the analysis
/// data behind it — useful for on-screen meters, `--verbose` logging, and
/// (later) the benchmarking harness.
pub struct CorrectionResult {
    pub samples: Vec<f32>,
    pub detected_hz: f32,
    pub detected_clarity: f32,
    pub semitone_shift: f32,
    pub timings: StageTimings,
}

/// Detects pitch, snaps it to the nearest note in a [`Scale`], and pitch-shifts
/// the block accordingly.
///
/// This type is **not** meant to be called from an audio I/O callback: it
/// allocates (the shifter's internal window buffers, the output `Vec`) and
/// its per-block cost depends on input content. It's meant to run on a
/// dedicated worker thread, fed and drained via the lock-free ring buffers
/// in `pitch-cli`. See `docs/ARCHITECTURE.md` for the full real-time-safety
/// rationale.
pub struct PitchCorrector {
    detector: McLeodDetector<f32>,
    shifter: PitchShifter,
    block_size: usize,
    scale: Scale,
}

impl PitchCorrector {
    /// `block_size` is fixed for the lifetime of this corrector. The
    /// detector's internal autocorrelation buffers are sized once here,
    /// rather than reallocated on every `process()` call — the original
    /// prototype this project is based on constructed a fresh
    /// `McLeodDetector` per block, which is wasted work on a thread that's
    /// still expected to keep up with real-time audio.
    pub fn new(block_size: usize, sample_rate: usize, window_size_ms: usize, scale: Scale) -> Self {
        let padding = block_size / 2;
        Self {
            detector: McLeodDetector::new(block_size, padding),
            shifter: PitchShifter::new(window_size_ms, sample_rate),
            block_size,
            scale,
        }
    }

    pub fn set_scale(&mut self, scale: Scale) {
        self.scale = scale;
    }

    /// `samples.len()` must equal the `block_size` passed to [`Self::new`].
    pub fn process(&mut self, samples: &[f32], sample_rate: usize) -> CorrectionResult {
        debug_assert_eq!(
            samples.len(),
            self.block_size,
            "PitchCorrector block size mismatch"
        );

        let detect_start = Instant::now();
        let pitch = self
            .detector
            .get_pitch(samples, sample_rate, POWER_THRESHOLD, CLARITY_THRESHOLD)
            .unwrap_or(Pitch {
                frequency: 0.0,
                clarity: 0.0,
            });
        let detect_us = detect_start.elapsed().as_secs_f64() * 1_000_000.0;

        let quantize_start = Instant::now();
        let mut semitone_shift = 0.0f32;
        if pitch.frequency > 0.0 {
            let current_note = hz_to_midi(pitch.frequency);
            let target_note =
                nearest_in_scale_midi(current_note.round() as u8, &self.scale) as f32;
            semitone_shift = target_note - current_note;
            if !semitone_shift.is_finite() {
                semitone_shift = 0.0;
            }
        }
        let quantize_us = quantize_start.elapsed().as_secs_f64() * 1_000_000.0;

        let shift_start = Instant::now();
        let mut out = vec![0.0f32; samples.len()];
        self.shifter
            .shift_pitch(OVER_SAMPLING, semitone_shift, samples, &mut out);
        let shift_us = shift_start.elapsed().as_secs_f64() * 1_000_000.0;

        CorrectionResult {
            samples: out,
            detected_hz: pitch.frequency,
            detected_clarity: pitch.clarity,
            semitone_shift,
            timings: StageTimings {
                detect_us,
                quantize_us,
                shift_us,
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hz_midi_roundtrip() {
        let midi = hz_to_midi(440.0);
        assert!((midi - 69.0).abs() < 1e-4);
        let hz = midi_to_hz(69.0);
        assert!((hz - 440.0).abs() < 1e-3);
    }

    #[test]
    fn silence_produces_no_shift() {
        use rust_music_theory::note::PitchClass;
        use rust_music_theory::scale::{Direction, Mode, ScaleType};

        let scale = Scale::new(
            ScaleType::Diatonic,
            PitchClass::C,
            4,
            Some(Mode::Ionian),
            Direction::Ascending,
        )
        .unwrap();
        let block_size = 1024;
        let mut corrector = PitchCorrector::new(block_size, 44100, 50, scale);
        let silence = vec![0.0f32; block_size];
        let result = corrector.process(&silence, 44100);
        assert_eq!(result.semitone_shift, 0.0);
        assert_eq!(result.samples.len(), block_size);
    }
}
