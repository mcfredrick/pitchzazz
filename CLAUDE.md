# pitchzazz — agent context

Real-time pitch correction (an "autotune"), built as a demonstration
project for a senior audio-engineering interview process: the goal is
genuinely correct, well-reasoned real-time audio engineering, not just
code that runs. Treat "best practices, explained, not just applied" as the
actual bar for this repo — every real-time-safety decision should be
documented with *why*, not just implemented.

Read `docs/ARCHITECTURE.md` before touching `pitch-cli/src/main.rs`. Read
`docs/ROADMAP.md` before deciding what to work on next — it has an explicit
"immediate next task" and, as of 2026-08-17, a "Prioritization criteria"
section tying roadmap priority to a specific target job description.
Weigh new work against that section before adding it to a phase.

## Current status (last updated 2026-08-17)

- Phases 0-3 are done. Rust MVP (Phase 1) has a *measured* (impulse-probe,
  not analytical-estimate) end-to-end latency number — see
  `docs/PERFORMANCE_LOG.md`'s "Measured pipeline latency" entry. The
  C++/JUCE port (Phase 2) is a real loadable AU/VST3/Standalone plugin,
  confirmed working in Ableton Live 12, not just a passthrough that
  satisfies `pluginval`/`auval`. Hot-swappable backends — Rust (FFI),
  native C++ (phase vocoder), and C++ TD-PSOLA (Phase 5) — are validated
  by an automated dropout-detection test (`docs/TESTING.md`), not just
  architecture that looks right. Full trail across
  `docs/PERFORMANCE_LOG.md`, `docs/COMPARISON.md`, and `docs/FINDINGS.md`.
- Phase 5 polish is in progress. Done so far: the TD-PSOLA engine (a real
  measured ~19-46% latency win over the phase vocoder depending on rate
  and revision — see `docs/PERFORMANCE_LOG.md`), the dark/neon visual
  redesign, the live detected/corrected pitch display, and — added
  2026-08-17 in one session — the classic Auto-Tune "correction amount" /
  "retune speed" (labeled "Smooth" in the UI) creative controls on both
  C++ engines, plus a full-width tuner-style cents-deviation meter under
  the pitch displays.
- **Modern C++ best-practices audit (2026-08-17): done.** Two real,
  multi-instance categories found and fixed (`unique_ptr` ownership,
  `noexcept`/`[[nodiscard]]` consistency), codified as durable rules in
  `cpp-plugin/CLAUDE.md`'s "C++ code quality standards" section so they
  don't regress. No correctness bugs found.
- **Immediate next task** (`docs/ROADMAP.md`, "Presentation walkthrough
  tooling"): the guided code-tour (CodeTour-style) through the 7 stops
  that demonstrate real-time-audio-engineering depth — explicitly gated
  on the best-practices audit landing first, which it now has. Not yet
  started.

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
7. **Any time you find and fix a real bug — correctness, toolchain,
   scaffolding, real-time-safety/lifecycle, anything — add a row to
   `docs/FINDINGS.md`'s quick-reference table** (bug, what found it, root
   cause, fix) at the same time you fix it, not retroactively at the end
   of a session. Include what tool/test/validation surfaced it and
   whether other validation you'd already run had missed it — that
   cross-tool-coverage angle (e.g. PluginVal missing something `auval`
   caught) is itself part of the evidence this repo is trying to build.
   The full narrative stays in whichever doc already owns that area
   (`ARCHITECTURE.md`, `PERFORMANCE_LOG.md`, `ROADMAP.md`'s dated
   checkpoints) — `FINDINGS.md` is the presentation-ready index across
   all of them, not a replacement for any.

## Key files

| File | What it does |
|------|---------------|
| `crates/pitch-core/src/scale.rs` | Scale quantization — nearest-in-scale-note search. Fixed a real bug from the original prototype here; see module doc. |
| `crates/pitch-core/src/corrector.rs` | Orchestrates detect → quantize → shift for one block. Not real-time-safe to call directly from a callback — see its doc comment. |
| `crates/pitch-cli/src/main.rs` | All real-time audio plumbing: cpal streams, lock-free ring buffers, worker thread. Start here to understand the concurrency architecture; the module doc comment is long on purpose. |
| `docs/ARCHITECTURE.md` | The real-time-safety design rationale: the original prototype's three concurrency bugs and their fixes, the dependency audit, block-size/latency design. |
| `docs/PERFORMANCE_LOG.md` | Dated, append-only profiling journal: findings, hypotheses, data, fixes, in the order they happened. Update it whenever you do performance work — see rule 6 above. |
| `docs/FINDINGS.md` | Presentation-ready index of every bug found (both engines, toolchain, scaffolding) — what found it, root cause, fix, and what other validation had missed it. Update it whenever you find and fix a real bug — see rule 7 above. |
| `docs/COMPARISON.md` | The Phase 2/3 compare-and-contrast writeup: what stayed identical vs. what had to diverge between the Rust and C++/JUCE engines and why, the controlled (build-profile-matched) performance comparison, and the "where we'd spend time next, how the data justifies it" section. |
| `docs/TESTING.md` | Testing-methodology writeup for the demo: the "manual listening → automated invariant" reframe, each already-built test category with what it actually catches (with real bug references), and the planned (not yet built) hot-swap dropout-detection methodology. Update the "planned" section once that test is actually built, rather than leaving it stale. |
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
