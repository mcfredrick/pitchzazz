//! Measures this engine's actual algorithmic pipeline latency — the thing
//! docs/ARCHITECTURE.md's Latency section and docs/PERFORMANCE_LOG.md's
//! SOTA-comparison entry only ever estimated analytically (~70-100ms),
//! never measured. Method: feed a single-sample impulse through the same
//! ring-buffer + worker-thread pipeline shape `main.rs` uses (not calling
//! `PitchCorrector::process` directly — block-accumulation delay is part
//! of what we're measuring), then find the peak of the output response.
//! For an impulse probe, cross-correlating against the output is
//! mathematically equivalent to just finding the output's peak magnitude
//! sample — the standard "impulse response peak as a group-delay proxy"
//! technique, not something invented for this test.
//!
//! Deliberately does NOT include `main.rs`'s `--startup-latency-ms`
//! silence pre-fill: that's a deployment robustness choice (absorb worker
//! scheduling jitter without an audible dropout), not part of the
//! pipeline's inherent algorithmic latency, and including it would
//! inflate this measurement with an arbitrary, separately-tunable number.
//! Also does not include real audio-hardware I/O latency (ADC/DAC buffer
//! delay) — this measures the software pipeline only, which is the part
//! this project's code controls and the part the analytical estimate was
//! about.
//!
//! Driven synchronously (no real worker thread) on purpose: this is a
//! deterministic offline measurement, and spinning a real OS thread would
//! only add nondeterministic scheduling noise to a number that should be
//! reproducible run to run.

use pitch_core::{Direction, Mode, PitchClass, PitchCorrector, Scale, ScaleType};
use ringbuf::HeapRb;

const BLOCK_SIZE: usize = 2048;

fn measure_latency_ms(sample_rate: usize) -> (f64, usize) {
    let scale = Scale::new(
        ScaleType::Diatonic,
        PitchClass::C,
        4,
        Some(Mode::Ionian),
        Direction::Ascending,
    )
    .unwrap();
    let mut corrector = PitchCorrector::new(BLOCK_SIZE, sample_rate, 50, scale);

    // One block of lead-in silence (clean initial state), an impulse at
    // the start of the second block, then generous trailing silence so
    // the phase vocoder's windowed response has room to fully emerge.
    let impulse_index = BLOCK_SIZE;
    let total_len = BLOCK_SIZE * 12;
    let mut input = vec![0.0f32; total_len];
    input[impulse_index] = 1.0;

    let ring_capacity = total_len + BLOCK_SIZE;
    let (mut input_producer, mut input_consumer) = HeapRb::<f32>::new(ring_capacity).split();
    let (mut output_producer, mut output_consumer) = HeapRb::<f32>::new(ring_capacity).split();
    let _ = input_producer.push_slice(&input);

    let mut analysis_buf = vec![0.0f32; BLOCK_SIZE];
    let mut filled = 0usize;
    loop {
        let read = input_consumer.pop_slice(&mut analysis_buf[filled..]);
        filled += read;
        if filled < BLOCK_SIZE {
            break; // input exhausted before filling another block
        }
        let result = corrector.process(&analysis_buf, sample_rate);
        let _ = output_producer.push_slice(&result.samples);
        filled = 0;
    }

    let mut output = vec![0.0f32; total_len];
    let n = output_consumer.pop_slice(&mut output);
    output.truncate(n);

    let peak_index = output
        .iter()
        .enumerate()
        .max_by(|(_, a), (_, b)| a.abs().partial_cmp(&b.abs()).unwrap())
        .map(|(i, _)| i)
        .unwrap_or(0);

    let latency_samples = peak_index as isize - impulse_index as isize;
    let latency_ms = (latency_samples.max(0) as f64 / sample_rate as f64) * 1000.0;
    (latency_ms, latency_samples.max(0) as usize)
}

fn main() {
    for sample_rate in [44100, 48000, 96000] {
        let (latency_ms, latency_samples) = measure_latency_ms(sample_rate);
        println!(
            "{sample_rate}Hz: {latency_ms:.1}ms ({latency_samples} samples) — block accumulation ~{:.1}ms + phase vocoder window",
            (BLOCK_SIZE as f64 / sample_rate as f64) * 1000.0
        );
    }
}
