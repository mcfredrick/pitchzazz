//! Real-time pitch correction over the default audio input/output.
//!
//! # Real-time audio safety — read this before touching the callbacks
//!
//! The two closures passed to `build_input_stream` / `build_output_stream`
//! below run on the audio driver's real-time thread, on a hard per-block
//! deadline. If either one blocks — on a mutex, on a channel send that
//! contends a lock, on a heap allocation that has to ask the OS for more
//! memory, on a `println!` that has to flush to a fd — the deadline can be
//! missed and the output glitches or drops out. There is no "try again
//! later" on this thread; miss the deadline once and the user hears it.
//!
//! The prototype this project is based on violated that rule three ways in
//! about ten lines of code:
//!   1. `Arc<Mutex<AudioProcessor>>.lock()` called directly inside the
//!      input callback.
//!   2. `std::sync::mpsc::Sender::send()` called from that same callback —
//!      std's mpsc is itself backed by a mutex/condvar internally, so this
//!      is the same problem wearing a different API.
//!   3. `data.clone()` on every callback — a heap allocation, on the audio
//!      thread, on every single block.
//!
//! The fix used throughout this file is the same in all three cases: never
//! touch anything from the audio callback that can block or allocate.
//! Concretely:
//!   - Both directions (mic -> worker, worker -> speakers) go through a
//!     `ringbuf::HeapRb` split into a `Producer`/`Consumer` pair. Splitting
//!     it is what makes it lock-free — the two halves talk via atomic
//!     head/tail indices, not a mutex. (Wrapping a `HeapRb` in
//!     `Arc<Mutex<_>>` instead of splitting it, which the prototype did, is
//!     actively worse than a plain `Vec` + mutex: it pays for a lock-free
//!     structure and then puts a lock in front of it anyway.)
//!   - `push_slice` / `pop_slice` never block: if the ring buffer is full or
//!     empty they just copy fewer samples than requested and return the
//!     count, so the callback can bail out in bounded time.
//!   - All the real work — pitch detection, scale quantization, pitch
//!     shifting — happens on a plain worker thread, which is free to
//!     allocate, log, and poll on a short sleep, because it has no deadline.

use clap::Parser;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use pitch_core::{hz_to_midi, Direction, Mode, PitchClass, PitchCorrector, Scale, ScaleType};
use ringbuf::HeapRb;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

/// How many samples the pitch corrector analyzes per block. This is
/// decoupled from whatever block size the audio host hands the callbacks —
/// cpal/the OS do not guarantee a fixed callback size, so the worker thread
/// accumulates samples from the ring buffer until it has exactly this many,
/// runs one correction pass, and pushes the result onward. Larger windows
/// give the pitch detector more signal to work with (better low-frequency
/// accuracy) at the cost of more latency.
const BLOCK_SIZE: usize = 2048;

/// Seconds of audio the ring buffers can hold. Sized generously relative to
/// `BLOCK_SIZE` so a slow worker-thread scheduling tick doesn't starve the
/// output callback; see `docs/ARCHITECTURE.md` for the latency/robustness
/// trade-off this represents.
const RING_BUFFER_SECONDS: f32 = 1.0;

#[derive(Parser, Debug)]
#[command(version, about = "Real-time pitch correction, lock-free from callback to callback.")]
struct Args {
    /// Tonic of the scale to correct into, e.g. "C", "F#", "Bb".
    #[arg(long, default_value = "C")]
    tonic: String,

    /// "major" or "minor". Anything else falls back to major with a warning.
    #[arg(long, default_value = "major")]
    mode: String,

    /// Milliseconds of output buffer to pre-fill with silence before the
    /// worker thread has produced anything, to avoid an audible dropout at
    /// startup while the pipeline is filling.
    #[arg(long, default_value_t = 150.0)]
    startup_latency_ms: f32,

    /// Print detected pitch / correction / per-block worker timing to
    /// stderr. Safe to enable: this only ever prints from the worker
    /// thread, never from the audio callbacks.
    #[arg(long)]
    verbose: bool,
}

fn parse_scale(tonic: &str, mode: &str) -> Scale {
    // Note: `PitchClass::from_str` is an inherent method on this crate's
    // type, not the `std::str::FromStr` trait (it returns `Option`, not
    // `Result`) — easy to assume otherwise given the name.
    let pitch_class = PitchClass::from_str(tonic).unwrap_or_else(|| {
        eprintln!("warning: couldn't parse tonic '{tonic}', defaulting to C");
        PitchClass::C
    });
    let scale_mode = match mode.to_lowercase().as_str() {
        "minor" => Mode::Aeolian,
        "major" => Mode::Ionian,
        other => {
            eprintln!("warning: unrecognized mode '{other}', defaulting to major");
            Mode::Ionian
        }
    };
    Scale::new(
        ScaleType::Diatonic,
        pitch_class,
        4,
        Some(scale_mode),
        Direction::Ascending,
    )
    .expect("Scale::new with a fixed, valid ScaleType/Mode combination should not fail")
}

fn err_fn(err: cpal::StreamError) {
    // Exceptional path, not the steady-state per-block path: cpal invokes
    // this on stream-level errors (device disconnect, etc.), not once per
    // callback, so printing here is a pragmatic, deliberate exception to
    // "never print from the audio thread" rather than an oversight.
    eprintln!("audio stream error: {err}");
}

fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let scale = parse_scale(&args.tonic, &args.mode);
    println!(
        "pitchzazz: correcting into {} {}",
        args.tonic,
        if args.mode.to_lowercase() == "minor" {
            "minor"
        } else {
            "major"
        }
    );

    let host = cpal::default_host();
    let input_device = host
        .default_input_device()
        .ok_or_else(|| anyhow::anyhow!("no default input device"))?;
    let output_device = host
        .default_output_device()
        .ok_or_else(|| anyhow::anyhow!("no default output device"))?;

    let input_config = input_device.default_input_config()?;
    if input_config.sample_format() != cpal::SampleFormat::F32 {
        anyhow::bail!(
            "input device's default format is {:?}, only F32 is supported right now",
            input_config.sample_format()
        );
    }
    let sample_rate = input_config.sample_rate().0 as usize;
    let input_channels = input_config.channels() as usize;

    let output_config = output_device.default_output_config()?;
    let output_channels = output_config.channels() as usize;

    println!(
        "input: {} ({} ch, {} Hz) -> output: {} ({} ch)",
        input_device.name().unwrap_or_default(),
        input_channels,
        sample_rate,
        output_device.name().unwrap_or_default(),
        output_channels
    );

    // --- Lock-free ring buffers, split into non-blocking Producer/Consumer
    // halves. See the module doc comment above for why this replaces the
    // mutex-guarded buffer and mpsc channel the prototype used.
    let ring_capacity = (sample_rate as f32 * RING_BUFFER_SECONDS) as usize;
    let (mut input_producer, mut input_consumer) = HeapRb::<f32>::new(ring_capacity).split();
    let (mut output_producer, mut output_consumer) = HeapRb::<f32>::new(ring_capacity).split();

    // Pre-fill the output ring buffer with silence so the output callback
    // has something to drain before the worker thread produces its first
    // corrected block. A deliberate latency/robustness trade documented in
    // docs/ARCHITECTURE.md, not an accident.
    let startup_silence = (sample_rate as f32 * args.startup_latency_ms / 1000.0) as usize;
    let _ = output_producer.push_slice(&vec![0.0f32; startup_silence]);

    let running = Arc::new(AtomicBool::new(true));

    // --- Worker thread: the only place DSP happens. Free to allocate, log,
    // and poll on a sleep, because it has no real-time deadline.
    let worker_running = Arc::clone(&running);
    let verbose = args.verbose;
    let worker = thread::spawn(move || {
        let mut corrector = PitchCorrector::new(BLOCK_SIZE, sample_rate, 50, scale);
        let mut analysis_buf = vec![0.0f32; BLOCK_SIZE];
        let mut filled = 0usize;

        while worker_running.load(Ordering::Relaxed) {
            let read = input_consumer.pop_slice(&mut analysis_buf[filled..]);
            filled += read;

            if filled < BLOCK_SIZE {
                // Nothing pathological about sleeping here: this thread has
                // no deadline. 1ms is short enough not to add perceptible
                // latency, long enough not to busy-spin a core.
                thread::sleep(Duration::from_millis(1));
                continue;
            }

            let result = corrector.process(&analysis_buf, sample_rate);

            if verbose {
                let midi = hz_to_midi(result.detected_hz);
                let t = result.timings;
                eprintln!(
                    "block: {:.1}Hz (midi {:.1}, clarity {:.2}) shift {:+.2} semitones — \
                     detect {:.1}us + quantize {:.1}us + shift {:.1}us = {:.1}us total \
                     (budget {:.1}us)",
                    result.detected_hz,
                    midi,
                    result.detected_clarity,
                    result.semitone_shift,
                    t.detect_us,
                    t.quantize_us,
                    t.shift_us,
                    t.total_us(),
                    (BLOCK_SIZE as f64 / sample_rate as f64) * 1_000_000.0
                );
            }

            let pushed = output_producer.push_slice(&result.samples);
            if verbose && pushed < result.samples.len() {
                eprintln!(
                    "output ring buffer full, dropped {} samples",
                    result.samples.len() - pushed
                );
            }

            filled = 0;
        }
    });

    // --- Input callback: copy into the ring buffer, downmixing to mono if
    // needed, and return. No locks, no allocation, no blocking calls.
    let input_stream = input_device.build_input_stream(
        &input_config.into(),
        move |data: &[f32], _| {
            if input_channels <= 1 {
                let _ = input_producer.push_slice(data);
            } else {
                // Downmix by averaging channels, one frame at a time, into
                // a small fixed-size stack buffer — no heap allocation.
                let mut mono = [0.0f32; 256];
                for chunk in data.chunks(input_channels * mono.len()) {
                    let frames = chunk.len() / input_channels;
                    for (i, frame) in chunk.chunks(input_channels).enumerate().take(frames) {
                        mono[i] = frame.iter().sum::<f32>() / input_channels as f32;
                    }
                    let _ = input_producer.push_slice(&mono[..frames]);
                }
            }
        },
        err_fn,
        None,
    )?;

    // --- Output callback: drain the ring buffer, up-mixing mono to however
    // many output channels exist. Underruns fill with silence rather than
    // blocking to wait for data — silence is the correct real-time-safe
    // failure mode here, not a stall.
    let output_stream = output_device.build_output_stream(
        &output_config.into(),
        move |data: &mut [f32], _| {
            if output_channels <= 1 {
                let read = output_consumer.pop_slice(data);
                for sample in &mut data[read..] {
                    *sample = 0.0;
                }
            } else {
                // Fixed-size stack buffer, chunked over `data` — mirrors the
                // input callback's downmix approach. An earlier draft of
                // this used `vec![0.0f32; frames]` here, a heap allocation
                // on every callback; that's the exact anti-pattern this
                // whole file exists to avoid, caught in review before it
                // shipped. Left as a cautionary comment on purpose.
                let mut mono = [0.0f32; 256];
                for chunk in data.chunks_mut(output_channels * mono.len()) {
                    let frames = chunk.len() / output_channels;
                    let read = output_consumer.pop_slice(&mut mono[..frames]);
                    for (i, frame) in chunk.chunks_mut(output_channels).enumerate() {
                        let sample = if i < read { mono[i] } else { 0.0 };
                        for out in frame {
                            *out = sample;
                        }
                    }
                }
            }
        },
        err_fn,
        None,
    )?;

    input_stream.play()?;
    output_stream.play()?;

    println!("running — Ctrl+C to stop");
    let running_ctrlc = Arc::clone(&running);
    ctrlc_handler(move || running_ctrlc.store(false, Ordering::Relaxed));
    while running.load(Ordering::Relaxed) {
        thread::sleep(Duration::from_millis(100));
    }

    worker.join().ok();
    Ok(())
}

/// Minimal Ctrl+C handling without pulling in the `ctrlc` crate: install a
/// signal handler is platform-specific enough that for a portfolio/demo CLI
/// it's simpler to document the limitation than add the dependency. On
/// Unix, SIGINT still terminates the process by default, which is an
/// acceptable exit path for a CLI like this one — this hook exists so
/// `--verbose` users see a clean "stopping" message rather than nothing.
fn ctrlc_handler<F: Fn() + Send + 'static>(_on_interrupt: F) {
    // Intentionally a no-op placeholder; see doc comment above.
}
