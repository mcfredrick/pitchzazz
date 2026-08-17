//! Controlled Rust-vs-C++ comparison point for docs/ROADMAP.md Phase 3 —
//! mirrors cpp-plugin/benchmarks/CorrectorPerformance.cpp as closely as
//! possible: same block size (2048), same window (50ms), same synthetic
//! 220Hz test tone (not silence — silence hits the power-threshold
//! early-exit and never runs the real DSP), same three sample rates, same
//! C major scale. Run with `cargo run --release --example bench_corrector`
//! — NOT plain `cargo run`, which builds the unoptimized debug profile;
//! see docs/PERFORMANCE_LOG.md's entries, which were gathered via the
//! debug-profile `cargo run` documented in the root CLAUDE.md. Comparing
//! those numbers directly against a C++ Release build would overstate the
//! gap by whatever debug/release does to Rust's numbers — this example
//! exists specifically to get an apples-to-apples number instead.

use pitch_core::{Direction, Mode, PitchClass, PitchCorrector, Scale, ScaleType};
use std::time::Instant;

fn test_tone(sample_rate: usize, num_samples: usize) -> Vec<f32> {
    (0..num_samples)
        .map(|i| {
            0.5 * (2.0 * std::f32::consts::PI * 220.0 * i as f32 / sample_rate as f32).sin()
        })
        .collect()
}

fn bench_at(sample_rate: usize) {
    const BLOCK_SIZE: usize = 2048;
    const ITERATIONS: usize = 100;

    let scale = Scale::new(
        ScaleType::Diatonic,
        PitchClass::C,
        4,
        Some(Mode::Ionian),
        Direction::Ascending,
    )
    .unwrap();

    let mut corrector = PitchCorrector::new(BLOCK_SIZE, sample_rate, 50, scale);
    let signal = test_tone(sample_rate, BLOCK_SIZE);

    // Warm-up, not measured — matches Catch2's benchmark warm-up behavior.
    for _ in 0..5 {
        corrector.process(&signal, sample_rate);
    }

    let mut total_us = Vec::with_capacity(ITERATIONS);
    for _ in 0..ITERATIONS {
        let start = Instant::now();
        std::hint::black_box(corrector.process(&signal, sample_rate));
        total_us.push(start.elapsed().as_secs_f64() * 1_000_000.0);
    }

    let mean: f64 = total_us.iter().sum::<f64>() / total_us.len() as f64;
    let min = total_us.iter().cloned().fold(f64::INFINITY, f64::min);
    let max = total_us.iter().cloned().fold(f64::NEG_INFINITY, f64::max);
    let budget_us = (BLOCK_SIZE as f64 / sample_rate as f64) * 1_000_000.0;

    println!(
        "{sample_rate}Hz, block {BLOCK_SIZE}: mean {mean:.1}us, min {min:.1}us, max {max:.1}us, budget {budget_us:.1}us ({:.2}% of budget)",
        (mean / budget_us) * 100.0
    );
}

fn main() {
    for sample_rate in [44100, 48000, 96000] {
        bench_at(sample_rate);
    }
}
