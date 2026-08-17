# Rust vs. C++/JUCE: implementation comparison

The Phase 3 deliverable this project was actually building toward
(`docs/ROADMAP.md`): not just a benchmark number, but an honest account of
what changed and didn't porting the same real-time pitch-correction
algorithm from Rust to C++/JUCE, why, and what the data says about where
to spend time next. Companion reading: `docs/FINDINGS.md` (every bug, in
one table) and `docs/PERFORMANCE_LOG.md` (the full measurement trail this
doc's numbers are pulled from).

## What stayed identical

Deliberately, not incidentally — the point of a language comparison only
holds if both sides solve the same problem the same way:

- **Algorithm**: McLeod pitch detection (MPM) and a Bernsee-style phase
  vocoder for pitch shifting, both ported from the actual Rust crate
  source (`pitch-detection`, `pitch_shift`) rather than re-derived from
  the underlying papers — same peak-picking, same quadratic
  interpolation, same STFT/overlap-add math.
- **Concurrency architecture**: lock-free SPSC ring buffer (mic thread →
  worker), a worker thread with no real-time deadline doing all the
  allocating DSP work, a second lock-free ring buffer (worker → speaker
  thread) with silence-on-underrun as the failure mode. `ringbuf` on the
  Rust side, `juce::AbstractFifo` on the C++ side — both audited by
  reading actual source for atomic-index-based, lock-free behavior before
  use, not assumed from documentation.
- **Constants**: same block size (2048), same phase-vocoder window
  (50ms, subject to the power-of-two rounding below), same over-sampling
  factor (8), same power/clarity detection thresholds.

## Where they had to diverge, and why

Every divergence below was forced by a real difference between the
ecosystems, not a design preference — each is documented in more depth at
its source location, linked below.

### FFT library normalization convention

`rustfft`/`realfft` (Rust) normalize **neither** direction of a
forward+inverse FFT round trip — the caller must divide by N. JUCE's
`dsp::FFT::perform(..., inverse)` **auto-normalizes** the inverse by 1/N.
Missing this would have made pitch detection silently never cross its
clarity threshold, and pitch-shifted audio silently come out far too
quiet — no error, no crash, just wrong audio. Caught by source audit
before it shipped (`docs/FINDINGS.md` #10), confirmed by two new tests
that don't exist on the Rust side (a sine-wave detection test, an
output-energy sanity check) written specifically because the Rust-mirrored
tests don't exercise the FFT path at all.

*Detail:* `cpp-plugin/Source/DSP/PitchDetector.cpp` and `PitchShifter.cpp`.

### FFT size constraints

`rustfft`/`realfft` accept arbitrary lengths. JUCE's `dsp::FFT` requires a
power of two. Two different adaptations were needed:

- **Pitch detector**: Rust zero-pads to `blockSize + blockSize/2` (3072
  for a 2048-sample block); C++ rounds up to the next power of two (4096).
  Verified harmless by working through the actual math (the extra padding
  gets swamped by the same normalization term regardless of its exact
  size — see the code comment for the full derivation) rather than
  assumed.
- **Phase vocoder**: Rust's frame size is `sampleRate * windowMs / 1000`
  rounded to even (e.g. 2205 at 44.1kHz); C++ rounds to the *nearest*
  power of two (2048 at 44.1/48kHz, 4096 at 96kHz) rather than the *next*
  one, specifically to avoid silently doubling the window/latency. This
  is the one place the two engines' actual analysis window size provably
  differs — worth saying out loud in the demo, not hiding it.

*Detail:* `cpp-plugin/Source/DSP/PitchDetector.cpp`'s
`normalizedSquareDifference` comment, `PitchShifter.cpp`'s
`computeFrameSize` comment.

### Toolchain fragility on Apple Silicon — same problem class, different symptom

Both engines hit an x86_64-under-Rosetta drift on this dev machine, from
different root causes: Rust's `rustup` self-detection failed because
`rustup` itself was an x86_64 binary running under Rosetta (fixed with
`rustup default stable-aarch64-apple-darwin`); C++'s build picked up an
Intel Homebrew `cmake` (this machine has two Homebrew installs, Intel at
`/usr/local` and Apple Silicon-native at `/opt/homebrew`, with the Intel
one earlier in `PATH`) which reports itself as x86_64 under Rosetta and
produced an x86_64-only plugin binary that built successfully but failed
to *load* on this arm64 machine (fixed by explicitly pinning
`CMAKE_OSX_ARCHITECTURES=arm64`). Same underlying "Apple Silicon Mac with
a dual-architecture toolchain somewhere in the chain" failure mode,
independently rediscovered in both language ecosystems — worth
highlighting as a pattern, not two unrelated bugs.

*Detail:* root `CLAUDE.md`'s Build/test/run section (Rust);
`docs/FINDINGS.md` #7 and the comment in `cpp-plugin/CMakeLists.txt`
(C++).

### Thread lifecycle: fails-safe vs. asserts

The one difference that's genuinely about the languages/frameworks
themselves, not just library quirks: Rust's `std::thread::JoinHandle`, if
dropped without calling `.join()`, **detaches silently** — the thread
keeps running as a daemon, no crash, no UB. `juce::Thread`, if destroyed
while still running, **asserts (and is undefined behavior otherwise)** —
the caller is required to call `stopThread()` first, and nothing in the
type system enforces it. This project's C++ port hit exactly that: the
`AudioProcessor` destructor didn't stop the worker thread, relying on
`releaseResources()` being called first — which isn't guaranteed by every
host (`auval` doesn't). Rust's `pitch-cli` has the analogous shutdown
path (`worker.join()` at the end of `main()`) but wouldn't have hit a
*crash* if that line were deleted, only a leaked thread — a real
example of "fails safe by default" vs. "fails loud if you don't
explicitly get it right," which is a genuinely fair, specific thing to
say about the two languages' concurrency primitives rather than a vague
"Rust is safer" claim.

*Detail:* `docs/FINDINGS.md` #11.

## Performance: no consistent winner, once compared fairly

Full data and methodology: `docs/PERFORMANCE_LOG.md`'s 2026-08-17
"Rust-vs-C++ per-block cost" entry. Summary: a first-pass comparison
looked like C++ was 20-30x faster — that comparison was wrong (Rust
numbers were from a debug build, C++ from Release). Controlled for build
profile, block size (2048), and identical synthetic input:

| Sample rate | Rust (release) | C++ (Release) |
|---|---|---|
| 44100Hz | 461.5us | 371.4us |
| 48000Hz | 349.1us | 368.3us |
| 96000Hz | 307.0us | 375.6us |

Each engine wins at a different sample rate; all six numbers are within
noise of each other and every one sits under 2% of the real-time budget.
The honest conclusion isn't "language X is faster" — it's that a
build-profile mismatch is an easy, large (20-30x observed here), and easy
to *not notice* source of a misleading benchmark, and that properly
optimized, both engines are essentially real-time-trivial for this
workload. The most likely source of the small remaining gap is FFT
backend choice (JUCE's Apple-optimized vDSP engine vs. `rustfft`'s
portable implementation), not the language.

## Where we'd spend time next, and how the data would justify it

In priority order, each tied to a specific open question rather than a
vague "more polish":

1. **Re-evaluate `OVER_SAMPLING = 8`.** It was chosen against Rust
   *debug*-build costs (`PERFORMANCE_LOG.md`'s 32→8 entry). Now that
   release-build costs are known (both engines finish an entire
   correction pass in under 500us against a 21,000-46,000us budget), there's
   headroom to try a higher oversampling value for better pitch-shift
   quality and measure whether it's still affordable — against both the
   CPU-cost budget and, now that it's measured
   (`docs/PERFORMANCE_LOG.md`'s "Measured pipeline latency" entry), the
   phase vocoder's own latency, since oversampling and frame size are
   coupled in that algorithm. Currently a follow-up flag, not a
   re-measurement, specifically noted as such so it doesn't get mistaken
   for already-done work.
2. **Resolve the tail-latency question.** Two timing-spike outliers from
   the original Rust debug-build profiling run were never confirmed as
   one-off jitter vs. a recurring pattern (`PERFORMANCE_LOG.md`). This
   sprint's controlled comparison used short (100-iteration) runs that
   can't observe rare scheduling jitter — a longer run, on both engines,
   is the concrete next step, not a repeat of the short benchmark.
3. **Done, 2026-08-17**: measured pipeline software latency for both
   engines via matching impulse-response probes
   (`docs/PERFORMANCE_LOG.md`'s "Measured pipeline latency" entry) —
   50.0ms (Rust) vs. 42.7-46.4ms (C++), both exactly equal to each
   engine's own phase-vocoder frame size, confirming the ~70-100ms
   analytical estimate had assumed block-accumulation delay and the
   phase vocoder's window stack additively when they don't. Real
   audio-hardware I/O latency is still not measured (would need a
   physical loopback setup not available in this environment), and the
   "block accumulation never dominates" reasoning is argued from the
   data rather than independently verified with a real-time-paced probe
   — both flagged as remaining gaps in that entry, not implied done.
4. **Hot-swap runtime backend switching** (Phase 3's harder half) —
   deliberately deferred for this sprint; the benchmarking half of Phase
   3 is what this document covers.
5. **MIDI-controlled pitch target ("vocoder mode")** — a real feature
   idea, not scope creep noticed too late; see `docs/ROADMAP.md` Phase 5
   for why the plugin scaffold briefly defaulted to a MIDI-effect type.

## What this comparison is, and isn't, evidence of

Worth stating directly for the demo: this is not evidence that Rust and
C++ perform identically in general, or that either language is
"better" — it's evidence that for *this specific, FFT-dominated DSP
workload*, with comparable optimization effort on both sides, the
language choice mattered far less than (a) whether the build was
optimized at all, and (b) which underlying FFT implementation got linked
in. The things that were genuinely hard and genuinely different between
the two ports — FFT normalization conventions, power-of-two constraints,
thread lifecycle discipline — were ecosystem/library details, not
language-performance differences. That's arguably a more useful,
more honest engineering finding than "we benchmarked X and it won."
