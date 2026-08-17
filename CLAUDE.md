# pitchzazz — agent context

Real-time pitch correction (an "autotune"), built as a demonstration
project for a senior audio-engineering interview process: the goal is
genuinely correct, well-reasoned real-time audio engineering, not just
code that runs. Treat "best practices, explained, not just applied" as the
actual bar for this repo — every real-time-safety decision should be
documented with *why*, not just implemented.

Read `docs/ARCHITECTURE.md` before touching `pitch-cli/src/main.rs`. Read
`docs/ROADMAP.md` before deciding what to work on next — it has an explicit
"immediate next task."

## Current status (last updated 2026-08-17)

- Workspace builds clean, `cargo test` (6/6 pass) and `cargo clippy` are
  clean, on a native `aarch64-apple-darwin` toolchain.
- Runs live end-to-end against a real mic: pitch detection, scale
  quantization, and pitch shifting are functionally correct.
- Worker-thread processing time: was structurally slower than real-time
  (40-60% over budget) at default settings; root-caused to the pitch
  shifter's oversampling factor (32, unmeasured leftover from the original
  prototype) and dropped to 8, which brought most blocks comfortably under
  budget. Two outlier spikes in the same test run are not yet explained
  (probably OS scheduling jitter, not confirmed). Full trail in
  `docs/PERFORMANCE_LOG.md` — read it before changing anything
  performance-related, the reasoning matters as much as the current state.
- **Separately, and not yet measured directly**: total mic-to-speaker
  pipeline latency (which includes the phase vocoder's ~50ms window, not
  just the worker thread's CPU time) is analytically estimated at roughly
  70-100ms+ — see `docs/PERFORMANCE_LOG.md`'s SOTA comparison entry. That
  puts this project in line with general-purpose (non-live-optimized)
  commercial pitch-correction plugins, not the specialized sub-10ms live
  tier, which uses a fundamentally different algorithm family
  (time-domain PSOLA vs. this project's phase vocoder). Per project
  direction: don't chase past what real competitors achieve on this
  timeline — measure honestly, explain where the latency comes from, and
  note the algorithm-family swap as a Phase 2+ idea, not an MVP blocker.
- **Immediate next task** (`docs/ROADMAP.md` Phase 1): get an actual
  measured end-to-end latency number (e.g. an impulse/loopback test) to
  replace the analytical estimate above, and characterize whether the two
  outlier timing spikes are one-off jitter or a recurring pattern.

## Rules to always follow in this codebase

These aren't style preferences — violating them is the specific class of
bug this whole project exists to demonstrate understanding of. Full
rationale in `docs/ARCHITECTURE.md`.

1. **Nothing in `pitch-cli`'s `build_input_stream`/`build_output_stream`
   closures may block, allocate, or take a lock.** Before adding anything
   to either closure, ask: does this call allocate? Can it block? If
   either answer might be yes, it doesn't belong in the callback — move it
   to the worker thread and hand data across the existing ring buffers.
2. **Before adding a dependency whose code would run inside a real-time
   callback**, audit its actual source in `~/.cargo/registry/src/` for
   locks/allocation on the hot path — don't trust the crate's docs or
   README claims. `docs/ARCHITECTURE.md`'s "Dependency audit" section is
   the template for how to do this and what it found for `ringbuf`.
3. **`pitch-core` stays I/O-free and thread-free.** It's pure DSP,
   deliberately, so it's callable from a CLI, a future JUCE plugin's FFI
   bridge, or a unit test with no threading model assumptions baked in.
   Real-time plumbing belongs in the consuming crate (`pitch-cli` today,
   later a plugin crate), not in `pitch-core`.
4. **Every non-obvious real-time-safety or DSP-correctness decision gets a
   comment explaining why, not what.** This repo is meant to be read and
   explained, including out loud, during a technical presentation — code
   that's merely correct but unexplained doesn't serve that purpose. See
   `scale.rs`'s module doc and `main.rs`'s module doc for the tone/depth
   to match.
5. **Run `cargo test && cargo clippy --all-targets` before considering any
   change done.**
6. **Any time you profile something, change a performance-relevant
   parameter, or investigate a latency/CPU-cost question, add a dated entry
   to `docs/PERFORMANCE_LOG.md`** — data first, then what you concluded,
   then what you changed. This is append-only lab-notebook material, not a
   status summary: don't edit old entries to match new findings, add a new
   one that supersedes them and say so. This log is the single strongest
   piece of "software performance optimization" and "excited about data"
   evidence in this repo — treat maintaining it as part of the work, not
   an afterthought to do once at the end.

## Key files

| File | What it does |
|------|---------------|
| `crates/pitch-core/src/scale.rs` | Scale quantization — nearest-in-scale-note search. Fixed a real bug from the original prototype here; see module doc. |
| `crates/pitch-core/src/corrector.rs` | Orchestrates detect → quantize → shift for one block. Not real-time-safe to call directly from a callback — see its doc comment. |
| `crates/pitch-cli/src/main.rs` | All real-time audio plumbing: cpal streams, lock-free ring buffers, worker thread. Start here to understand the concurrency architecture; the module doc comment is long on purpose. |
| `docs/ARCHITECTURE.md` | The real-time-safety design rationale: the original prototype's three concurrency bugs and their fixes, the dependency audit, block-size/latency design. |
| `docs/PERFORMANCE_LOG.md` | Dated, append-only profiling journal: findings, hypotheses, data, fixes, in the order they happened. Update it whenever you do performance work — see rule 6 above. |
| `docs/ROADMAP.md` | Phased plan (Phase 0 prototype history through Phase 5 polish), plus a note on building a guided code-tour walkthrough for the actual presentation. |

## Build / test / run

```
cargo build
cargo test
cargo clippy --all-targets
cargo run -- --help
cargo run -- --tonic C --mode major --verbose   # live mic -> speakers
```

Requires a native `aarch64-apple-darwin` Rust toolchain on Apple Silicon —
`rustup show` should report `host: aarch64-apple-darwin`. If a build fails
inside `coreaudio-sys`/`bindgen` with a libclang architecture-mismatch
error, that means the toolchain has drifted back to x86_64-under-Rosetta;
`rustup default stable-aarch64-apple-darwin` (installing it first with
`--force-non-host` if needed — `rustup`'s own self-detection is unreliable
when rustup itself is an x86_64 binary running under Rosetta, which was the
case on this machine before the switch) fixes it at the root rather than
needing a `LIBCLANG_PATH` override.
