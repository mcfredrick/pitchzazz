//! Data for docs/ROADMAP.md/COMPARISON.md's flagged-but-not-yet-done
//! follow-up: OVER_SAMPLING=8 was chosen against debug-build CPU costs
//! (docs/PERFORMANCE_LOG.md's 32->8 entry); release-build costs turned
//! out to have far more headroom than assumed. This sweeps over_sampling
//! directly against `PitchShifter` (not the full `PitchCorrector` — only
//! the shift stage is affected by this parameter) to measure both its
//! CPU cost *and* its effect on pipeline latency in one pass, at 44100Hz,
//! rather than assuming from the algorithm's structure how it behaves.
//!
//! Run with `cargo run --release --example oversampling_sweep -p pitch-core`.

use pitch_shift::PitchShifter;
use std::time::Instant;

const SAMPLE_RATE: usize = 44100;
const BLOCK_SIZE: usize = 2048;

fn measure(over_sampling: usize) {
    // --- Cost: mean of 100 calls on a steady tone, warm-up excluded.
    let mut shifter = PitchShifter::new(50, SAMPLE_RATE);
    let signal: Vec<f32> = (0..BLOCK_SIZE)
        .map(|i| 0.5 * (2.0 * std::f32::consts::PI * 220.0 * i as f32 / SAMPLE_RATE as f32).sin())
        .collect();
    let mut out = vec![0.0f32; BLOCK_SIZE];

    for _ in 0..5 {
        shifter.shift_pitch(over_sampling, 0.0, &signal, &mut out);
    }
    let mut costs_us = Vec::with_capacity(100);
    for _ in 0..100 {
        let start = Instant::now();
        shifter.shift_pitch(over_sampling, 0.0, &signal, &mut out);
        costs_us.push(start.elapsed().as_secs_f64() * 1_000_000.0);
    }
    let mean_cost_us: f64 = costs_us.iter().sum::<f64>() / costs_us.len() as f64;

    // --- Latency: impulse-response peak, same method as latency_probe.rs.
    let mut shifter = PitchShifter::new(50, SAMPLE_RATE);
    let impulse_index = BLOCK_SIZE;
    let total_len = BLOCK_SIZE * 12;
    let mut input = vec![0.0f32; total_len];
    input[impulse_index] = 1.0;
    let mut output = vec![0.0f32; total_len];

    let mut offset = 0;
    while offset + BLOCK_SIZE <= total_len {
        let mut block_out = vec![0.0f32; BLOCK_SIZE];
        shifter.shift_pitch(
            over_sampling,
            0.0,
            &input[offset..offset + BLOCK_SIZE],
            &mut block_out,
        );
        output[offset..offset + BLOCK_SIZE].copy_from_slice(&block_out);
        offset += BLOCK_SIZE;
    }

    let peak_index = output
        .iter()
        .enumerate()
        .max_by(|(_, a), (_, b)| a.abs().partial_cmp(&b.abs()).unwrap())
        .map(|(i, _)| i)
        .unwrap_or(0);
    let latency_samples = (peak_index as isize - impulse_index as isize).max(0);
    let latency_ms = (latency_samples as f64 / SAMPLE_RATE as f64) * 1000.0;

    let budget_us = (BLOCK_SIZE as f64 / SAMPLE_RATE as f64) * 1_000_000.0;
    println!(
        "over_sampling={over_sampling:>3}: cost {mean_cost_us:>7.1}us ({:>5.2}% of {budget_us:.0}us budget), latency {latency_ms:>5.1}ms ({latency_samples} samples)",
        (mean_cost_us / budget_us) * 100.0
    );
}

fn main() {
    for over_sampling in [4, 8, 16, 32, 64] {
        measure(over_sampling);
    }
}
