//! cxx FFI bridge exposing `pitch-core`'s `PitchCorrector` to the
//! C++/JUCE plugin, for Phase 3's hot-swap feature (docs/ROADMAP.md).
//! `pitch-core` itself stays FFI-free by design (see its `lib.rs` doc
//! comment) — this crate is the seam, kept separate rather than adding
//! `cxx` as a dependency of `pitch-core` directly.
//!
//! Sample data crosses the boundary via pre-sized slices (`&[f32]` in,
//! `&mut [f32]` out), not `Vec`, so no allocation happens at the FFI call
//! site itself — consistent with this project's general "no surprise
//! allocation on a latency-sensitive path" discipline, even though this
//! specific call happens on `CorrectorWorker`'s non-real-time thread
//! (docs/ARCHITECTURE.md), not the audio callback itself.

#[cxx::bridge(namespace = "pitchzazz_rust")]
mod ffi {
    /// Mirrors pitch-core's `StageTimings` plus the detection results
    /// from `CorrectionResult` — flattened into one POD struct since cxx
    /// shared structs must be trivial (no `Vec`/`String` fields).
    struct FfiTimings {
        detected_hz: f32,
        detected_clarity: f32,
        semitone_shift: f32,
        detect_us: f64,
        quantize_us: f64,
        shift_us: f64,
    }

    extern "Rust" {
        type RustCorrector;

        /// `tonic_pitch_class`: 0-11, 0 = C — same convention as the
        /// C++ side's `pitchzazz::Scale::tonicPitchClass`.
        fn new_corrector(
            block_size: usize,
            sample_rate: usize,
            window_size_ms: usize,
            tonic_pitch_class: u8,
            is_minor: bool,
        ) -> Box<RustCorrector>;

        fn set_scale(corrector: &mut RustCorrector, tonic_pitch_class: u8, is_minor: bool);

        /// `out.len()` must equal `block_size` passed to `new_corrector`.
        fn process(
            corrector: &mut RustCorrector,
            samples: &[f32],
            sample_rate: usize,
            out: &mut [f32],
        ) -> FfiTimings;

        fn latency_samples(corrector: &RustCorrector) -> usize;
    }
}

use pitch_core::{Direction, Mode, PitchClass, PitchCorrector, Scale, ScaleType};

pub struct RustCorrector {
    inner: PitchCorrector,
}

fn build_scale(tonic_pitch_class: u8, is_minor: bool) -> Scale {
    let pitch_class = PitchClass::from_u8(tonic_pitch_class);
    let mode = if is_minor { Mode::Aeolian } else { Mode::Ionian };
    Scale::new(
        ScaleType::Diatonic,
        pitch_class,
        4,
        Some(mode),
        Direction::Ascending,
    )
    .expect("Scale::new with a fixed, valid ScaleType/Mode combination should not fail")
}

fn new_corrector(
    block_size: usize,
    sample_rate: usize,
    window_size_ms: usize,
    tonic_pitch_class: u8,
    is_minor: bool,
) -> Box<RustCorrector> {
    let scale = build_scale(tonic_pitch_class, is_minor);
    Box::new(RustCorrector {
        inner: PitchCorrector::new(block_size, sample_rate, window_size_ms, scale),
    })
}

fn set_scale(corrector: &mut RustCorrector, tonic_pitch_class: u8, is_minor: bool) {
    corrector
        .inner
        .set_scale(build_scale(tonic_pitch_class, is_minor));
}

fn latency_samples(corrector: &RustCorrector) -> usize {
    corrector.inner.latency_samples()
}

fn process(
    corrector: &mut RustCorrector,
    samples: &[f32],
    sample_rate: usize,
    out: &mut [f32],
) -> ffi::FfiTimings {
    // Unwinding a Rust panic across an FFI boundary is undefined
    // behavior — catch it explicitly rather than relying on any
    // automatic handling cxx may or may not provide for non-Result-
    // returning bridge functions (not verified either way; audited-
    // and-explicit beats assumed, matching this project's general
    // "read the actual source, don't trust the docs" rule applied here
    // to a claim about a dependency's behavior instead of its code).
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        corrector.inner.process(samples, sample_rate)
    }));

    match result {
        Ok(result) => {
            let n = out.len().min(result.samples.len());
            out[..n].copy_from_slice(&result.samples[..n]);
            ffi::FfiTimings {
                detected_hz: result.detected_hz,
                detected_clarity: result.detected_clarity,
                semitone_shift: result.semitone_shift,
                detect_us: result.timings.detect_us,
                quantize_us: result.timings.quantize_us,
                shift_us: result.timings.shift_us,
            }
        }
        Err(_) => {
            // A panic here would mean a real bug in pitch-core, but this
            // boundary must fail safe rather than propagate UB — silence
            // and zeroed timings, the same "safe fallback over crash"
            // philosophy corrector.rs already applies one layer down for
            // a failed pitch detection.
            out.fill(0.0);
            ffi::FfiTimings {
                detected_hz: 0.0,
                detected_clarity: 0.0,
                semitone_shift: 0.0,
                detect_us: 0.0,
                quantize_us: 0.0,
                shift_us: 0.0,
            }
        }
    }
}
