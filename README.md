# pitchzazz

Real-time pitch correction ("autotune"): McLeod pitch detection, scale
quantization to the nearest note in a chosen key, and pitch shifting —
architected to be genuinely real-time-safe, not just functionally
correct. Two implementations that stayed in lockstep on purpose: a Rust
CLI (`crates/`) and a C++/JUCE plugin (`cpp-plugin/`, AU/VST3/Standalone)
with three hot-swappable pitch-shift engines (phase vocoder, TD-PSOLA,
Varispeed + WSOLA) you can switch between live with no audio dropout.

**[Read the illustrated notebook](https://mcfredrick.github.io/pitchzazz/)**
— every DSP mechanism (windowing, the transforms, the actual math) plus
the real-time engine and hot-swap architecture, each section linking
straight to source on GitHub.

A clean rebuild of an earlier 2023 prototype (`rusty-tuner`) whose core
DSP worked but whose concurrency design had three real-time-safety bugs
(a lock, a blocking channel, and an allocation, all inside the real-time
audio callback) and one correctness bug (wrong "nearest note" for some
scales). Both classes of bug, and every one found since, are documented
in `docs/ARCHITECTURE.md`, `crates/pitch-core/src/scale.rs`, and
`docs/FINDINGS.md`'s full ledger.

## Build & run

### Rust CLI (`pitch-cli`)

```
cargo build
cargo test
cargo run -- --tonic C --mode major --verbose   # live mic -> speakers
```

Requires a native `aarch64-apple-darwin` Rust toolchain on Apple Silicon
— see `CLAUDE.md` if `coreaudio-sys`/`bindgen` fails to build with a
libclang architecture-mismatch error.

### C++/JUCE plugin (`cpp-plugin`)

Builds AU, VST3, and a Standalone app for macOS.

```
cd cpp-plugin
./scripts/generate_and_open_xcode.sh   # first time, or after a CMakeLists.txt/.env change
./scripts/build.sh standalone          # build + launch the Standalone app
./scripts/build.sh au vst3             # or build the plugin formats
./scripts/build.sh all test            # build everything, run Catch2 + PluginVal
```

Requires Xcode, and the same `aarch64-apple-darwin` Rust toolchain as
above — the plugin links a compiled Rust engine in over FFI as one of
its three hot-swappable pitch-shift engines. Full detail, including the
Corrosion/cxx-bridge setup, in `cpp-plugin/README.md`.

No signed release is published yet — `./scripts/build.sh` variants exist
for signing, notarizing, and packaging (see `cpp-plugin/README.md`) once
that's set up.

## Explore the code

- **[Illustrated notebook](https://mcfredrick.github.io/pitchzazz/)** —
  start here. Every mechanism, with diagrams, real math, and links to
  the exact source line for each one.
- **`.tours/pitchzazz-live-demo.tour`** / **`.tours/pitchzazz-walkthrough.tour`**
  — guided [CodeTour](https://marketplace.visualstudio.com/items?itemName=vsls-contrib.codetour)
  walkthroughs (7-stop live-demo path and 12-stop full version) through
  the same material, steppable inside an editor.
- **`docs/ALGORITHMS.md`** — the Q&A-depth study sheet behind the
  notebook: an acronym glossary up top, then every mechanism's deep
  debugging history, data, and reverted attempts, not just the current
  state.

## Project layout

- `crates/pitch-core` — pure DSP (detection, scale quantization, pitch
  shifting). No I/O, no threads, real-time-safety-agnostic by design.
- `crates/pitch-cli` — all real-time audio plumbing for the Rust build:
  cpal streams, lock-free ring buffers, the worker thread.
- `crates/pitch-core-ffi` — the cxx bridge exposing `pitch-core` to C++.
- `cpp-plugin/` — the JUCE plugin: three hot-swappable DSP engines
  (`Source/DSP/`), the real-time plumbing (`PluginProcessor`,
  `CorrectorWorker`), and the UI (`PluginEditor`,
  `PitchzazzLookAndFeel`).

## Status and where this is going

- `docs/ARCHITECTURE.md` — the real-time-safety design and why it looks
  the way it does.
- `docs/COMPARISON.md` — what stayed identical vs. diverged between the
  Rust and C++ engines, and a controlled build-profile-matched
  performance comparison between them.
- `docs/PERFORMANCE_LOG.md` — dated, append-only profiling journal:
  findings, hypotheses, data, fixes, in the order they happened.
- `docs/FINDINGS.md` — presentation-ready index of every bug found
  across both engines, what caught it, and the fix.
- `docs/TESTING.md` — the testing methodology, including the automated
  hot-swap dropout-detection test.
- `docs/ROADMAP.md` — phased plan and current status. A browser/
  WebAssembly build of the DSP engines, for a live no-install demo, is
  in progress as a follow-on to this notebook.
- `CLAUDE.md` — project context and working rules for an AI coding agent
  picking this up; also a reasonable map of the repo for a human.
