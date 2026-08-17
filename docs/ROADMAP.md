# Roadmap

## Phase 0 — original prototype (`rusty-tuner`, 2023, not this repo)

Four partial attempts at the same idea: a Python prototype, a working
standalone Rust CLI (cpal + McLeod pitch detection + windowed pitch shift,
real producer/consumer threading), a Rust DSP core crate, and a JUCE C++
wrapper that never got fully wired up. The core pitch detection and pitch
shifting worked. It stalled on a scale-quantization bug (an interval-count
mismatch in a third-party music-theory crate produced wrong "nearest note"
results for some scales) and, unrelated to why it stalled, its real-time
plumbing had three real-time-safety violations (lock in the audio callback,
a blocking channel in the callback, allocation in the callback) that this
rebuild fixes. See `docs/ARCHITECTURE.md` for the full writeup — that
history is good material for explaining the design decisions here, not just
a footnote.

## Phase 1 — Rust MVP (this repo, current)

**Status:** builds clean, `cargo test` and `cargo clippy` pass, and it runs
live end-to-end against a real mic (verified 2026-08-17) — pitch detection,
scale quantization, and pitch shifting are all functioning correctly.
Worker-thread processing time was root-caused (oversampling factor, see
`docs/PERFORMANCE_LOG.md`) and fixed; most blocks now clear the real-time
budget with margin. Two things keep this phase from being "done":

1. Two outlier timing spikes in the same test run aren't yet explained
   (probably scheduling jitter, not confirmed).
2. Total mic-to-speaker *pipeline* latency (dominated by the phase
   vocoder's window, not the worker thread's CPU time — these are
   different numbers, see `docs/ARCHITECTURE.md`) has only been estimated
   analytically (~70-100ms), never actually measured.

Per project direction: the target here is an honest, well-understood
comparison against real commercial products' latency (researched in
`docs/PERFORMANCE_LOG.md` — sub-10ms is a different algorithm family,
TD-PSOLA vs. this project's phase vocoder, and chasing it isn't a
one-week-timeline goal), not an arbitrary lower number. Don't mark this
phase done until the latency is a measured number instead of an estimate
and you can explain where each millisecond of it comes from — that
explanation is worth more for the presentation than the number itself.

**Immediate next task:** measure actual end-to-end latency (e.g. an
impulse/loopback test: feed a click in, measure sample-accurate offset to
when it appears corrected at the output) to replace the analytical
estimate with a real one, and run long enough to see whether the two
timing outliers recur or were one-off. Log both as new dated entries in
`docs/PERFORMANCE_LOG.md`, not as edits to the existing entries.

## Phase 2 — C++ port

Port the *same algorithm* (McLeod pitch detection, same windowed pitch
shift approach) to C++ — not a different algorithm. The point of Phase 3's
comparison is language/implementation, and that claim only holds if both
sides are solving the identical problem the identical way. Reuse the
real-time-safety lessons from Phase 1 directly: same lock-free
producer/consumer shape, implemented with `juce::AbstractFifo` (or an
equivalent hand-rolled SPSC ring buffer) instead of `ringbuf`. Also port
the per-stage timing shape (`StageTimings` in `pitch-core`) so Phase 3's
benchmarking has identical measurement points on both sides.

*Separate idea, not this phase:* swapping the shift algorithm family from
phase vocoder to TD-PSOLA (or similar) is the actual lever for closing the
gap to commercial low-latency products' sub-10ms tier — see
`docs/PERFORMANCE_LOG.md`'s SOTA comparison entry. That's a genuinely
different algorithm, not a C++ port of this one, and isn't scoped into any
phase yet. Worth its own phase later if there's time, not a blocker for
anything above.

## Phase 3 — hot-swappable backend + benchmarking

A clean backend interface (Rust engine vs. C++ engine) that can be swapped
at runtime without audio dropouts — reusing the same atomic
message-thread-to-audio-thread handoff pattern proven out in the `fruce`
project. Add real benchmarking: per-block processing time and CPU load for
each backend, logged and visualized (even a simple bar chart is enough) so
the comparison is backed by data, not a claim.

## Phase 4 — GUI

A minimal JUCE-based UI: backend selector (Rust/C++), tonic/scale controls,
a live processing-time meter. Doesn't need to be elaborate — it needs to
make the hot-swap and the performance comparison visible and demoable live.

## Phase 5 — polish

- Note-matching refinement: handle edge cases beyond the current nearest-
  semitone search (e.g. weighting toward the previous detected note to
  reduce warble on sustained notes near a scale boundary).
- Scale-selection UI: live key/mode switching from the GUI, not just a CLI
  flag at startup.

## Presentation walkthrough tooling

Before the actual presentation: set up a guided code walkthrough using one
of the "code tour" style tools (e.g. the VS Code CodeTour extension and its
`.tours` JSON format, or an equivalent) that steps through the codebase in
a fixed order hitting the points that actually demonstrate audio-engineering
depth rather than requiring live narration to find them:

1. The three real-time-safety violations found in the original prototype
   and their fixes (`docs/ARCHITECTURE.md`) — the single strongest piece of
   evidence of real-time-systems understanding in this repo.
2. The `ringbuf` dependency audit — reading the actual crate source to
   verify the lock-free claim rather than trusting the docs.
3. The scale-quantization bug and its fix (`scale.rs` module docs + tests)
   — a concrete "found a subtle correctness bug, understood why, fixed it
   robustly" story.
4. The block-size decoupling between the DSP engine and the host callback
   size (`main.rs` module docs).
5. Whatever the Phase 1 performance investigation turns up — the
   before/after numbers, once they exist, are probably the single most
   persuasive "software performance optimization" evidence in the whole
   project.
6. (Once Phase 2/3 land) the hot-swap architecture and the C++/Rust
   benchmark comparison.

The tour should be runnable both live (stepping through in an editor during
the actual presentation) and asynchronously (a reviewer working through it
alone), since it's not yet clear which format the interview will want.
