//! Rust counterpart to `cpp-plugin/benchmarks/RenderDemoAudio.cpp` — feeds
//! a WAV file through `PitchCorrector` block-by-block with the exact
//! defaults `pitch-cli`'s live mic path uses (`BLOCK_SIZE = 2048`,
//! `window_size_ms = 50` — see `crates/pitch-cli/src/main.rs`), so the
//! output is genuine engine output, not a simulation of it.
//!
//! No retune-speed "smoothing" parameter exists on this side at all (see
//! the root CLAUDE.md's engine-scoping note) — this renders at
//! `PitchCorrector`'s only mode, instant-snap, which is this engine's
//! actual default.
//!
//! Run with: PITCHZAZZ_DEMO_INPUT=/path/to/input.wav
//!           cargo run --release --example render_demo_audio -p pitch-core

use pitch_core::{Direction, Mode, PitchClass, PitchCorrector, Scale, ScaleType};
use std::env;

const BLOCK_SIZE: usize = 2048;
const WINDOW_SIZE_MS: usize = 50;

fn main() {
    let Ok(input_path) = env::var("PITCHZAZZ_DEMO_INPUT") else {
        println!("Set PITCHZAZZ_DEMO_INPUT to a WAV path to run this render.");
        return;
    };
    let out_dir = env::var("PITCHZAZZ_DEMO_OUTDIR").unwrap_or_else(|_| "/tmp".to_string());

    let mut reader = hound::WavReader::open(&input_path).expect("failed to open input WAV");
    let spec = reader.spec();
    let sample_rate = spec.sample_rate as usize;
    let num_channels = spec.channels as usize;

    // Downmix to mono by averaging channels, matching the C++ harness and
    // the plugin's own internal mono representation.
    let interleaved: Vec<f32> = match spec.sample_format {
        hound::SampleFormat::Float => reader.samples::<f32>().map(|s| s.unwrap()).collect(),
        hound::SampleFormat::Int => reader
            .samples::<i32>()
            .map(|s| s.unwrap() as f32 / i16::MAX as f32)
            .collect(),
    };
    let mut mono: Vec<f32> = interleaved
        .chunks(num_channels)
        .map(|frame| frame.iter().sum::<f32>() / num_channels as f32)
        .collect();

    println!(
        "Loaded {input_path}: {} samples @ {sample_rate}Hz",
        mono.len()
    );

    // Pad to a whole number of blocks, same rationale as the C++ harness.
    let remainder = mono.len() % BLOCK_SIZE;
    if remainder != 0 {
        mono.resize(mono.len() + (BLOCK_SIZE - remainder), 0.0);
    }

    let scale = Scale::new(
        ScaleType::Diatonic,
        PitchClass::G,
        4,
        Some(Mode::Ionian),
        Direction::Ascending,
    )
    .unwrap();
    let mut corrector = PitchCorrector::new(BLOCK_SIZE, sample_rate, WINDOW_SIZE_MS, scale);

    let mut output = vec![0.0f32; mono.len()];
    for (block_in, block_out) in mono.chunks(BLOCK_SIZE).zip(output.chunks_mut(BLOCK_SIZE)) {
        let result = corrector.process(block_in, sample_rate);
        block_out.copy_from_slice(&result.samples);
    }

    let out_path = format!("{out_dir}/pitchzazz_demo_rust.wav");
    let out_spec = hound::WavSpec {
        channels: 1,
        sample_rate: sample_rate as u32,
        bits_per_sample: 32,
        sample_format: hound::SampleFormat::Float,
    };
    let mut writer = hound::WavWriter::create(&out_path, out_spec).expect("failed to create output WAV");
    for sample in &output {
        writer.write_sample(*sample).unwrap();
    }
    writer.finalize().unwrap();
    println!("Wrote {out_path}");
}
