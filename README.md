# pitchzazz

Real-time pitch correction ("autotune") over your default audio
input/output: McLeod pitch detection, scale quantization to the nearest
note in a chosen key, and phase-vocoder pitch shifting — architected to be
genuinely real-time-safe, not just functionally correct. See
`docs/ARCHITECTURE.md` for why that distinction took real work.

A clean rebuild of an earlier 2023 prototype (`rusty-tuner`) whose core DSP
worked but whose concurrency design had three real-time-safety bugs (a
lock, a blocking channel, and an allocation, all inside the real-time audio
callback) and one correctness bug (wrong "nearest note" for some scales).
Both classes of bug, and their fixes, are documented in `docs/ARCHITECTURE.md`
and `crates/pitch-core/src/scale.rs`.

## Quickstart

```
cargo build
cargo test
cargo run -- --tonic C --mode major --verbose
```

Requires a native `aarch64-apple-darwin` Rust toolchain on Apple Silicon —
see `CLAUDE.md` if `coreaudio-sys`/`bindgen` fails to build with a libclang
architecture-mismatch error.

## Project layout

- `crates/pitch-core` — pure DSP (detection, scale quantization, pitch
  shifting). No I/O, no threads, real-time-safety-agnostic by design.
- `crates/pitch-cli` — all real-time audio plumbing: cpal streams,
  lock-free ring buffers, the worker thread. This is where the
  real-time-safety rules actually get applied.

## Status and where this is going

- `docs/ARCHITECTURE.md` — the real-time-safety design and why it looks
  the way it does.
- `docs/PERFORMANCE_LOG.md` — dated profiling journal: findings,
  hypotheses, data, fixes, including a researched comparison against
  commercial pitch-correction products' real-world latency.
- `docs/ROADMAP.md` — phased plan from here (C++ port, a hot-swappable
  Rust/C++ backend with benchmarking, a GUI) and current status.
- `CLAUDE.md` — project context and working rules for an AI coding agent
  picking this up; also a reasonable map of the repo for a human.
