# Roadmap

## Prioritization criteria (added 2026-08-17)

This project exists to prep for a specific role: Splice, Sr Software
Engineer II, Audio Core team —
https://job-boards.greenhouse.io/splice/jobs/8659434002. When deciding
what to build next or how to scope work, weigh it against that JD's actual
requirements, not just internal roadmap completeness or general
best-practice thoroughness.

**Required, per the JD:** expert C++ developer, production-quality C++;
built software with JUCE or other plugin frameworks; expert audio
engineer — signal processing, real-time systems, multithreading; expert
at software performance optimization; DAW plugin experience specifically
(the application form asks which DAWs you've built plugins for).

**Nice-to-have, per the JD:** Rust, Go, WebAssembly/Emscripten, Angular,
Web Audio.

Practical effect: this repo's current MVP is 100% Rust, which covers the
"nice to have" column but not one line of the "required" column (JUCE,
C++, plugin framework, DAW plugin). That gap matters, but the fix isn't
to sideline Rust — **the goal is to demonstrate equivalent real-time
audio-engineering expertise in both languages**, which is exactly what
Phase 2 (C++ port) and Phase 3 (hot-swap + benchmark) already set out to
do: the same algorithm, the same real-time-safety discipline, solved
twice, then compared with real data. That comparison is itself strong
evidence for the JD's "required" column (expert C++, real-time systems,
performance optimization) precisely because it's backed by the "nice to
have" Rust implementation, not despite it. So: don't let Rust-side work
expand indefinitely past what Phase 1 needs (that's the "time-box it"
guidance below), but the reason to move to C++ now is to build the other
half of the comparison, not to deprioritize Rust. Re-check new work
against this section before adding it to a phase below.

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

**Immediate next task, re-scoped 2026-08-17 per the prioritization
criteria above:** the Rust MVP's remaining Phase 1 item (measured
end-to-end latency, via an impulse/loopback test: feed a click in,
measure sample-accurate offset to when it appears corrected at the
output, and check whether the two timing outliers recur) is still worth
doing — it's the last piece of a performance-optimization story the JD
explicitly wants — but it's now time-boxed to about a day, not an
open-ended research arc. Log the result as a new dated entry in
`docs/PERFORMANCE_LOG.md` (not an edit to existing entries), then treat
Phase 1 as functionally done and move to Phase 2. The Rust side isn't
being abandoned — Phase 3's benchmark comparison depends on it staying
solid — but its remaining value now comes from being the known-good half
of a Rust/C++ comparison, not from further standalone polish. Move to
Phase 2 so that comparison can actually happen.

## Phase 2 — C++/JUCE port (promoted to immediate priority after Phase 1's latency task, 2026-08-17)

**Day 1 checkpoint, 2026-08-17: passed.** Scaffolded via the `juce-dev`
Claude Code plugin (JUCE-Plugin-Starter template) at `cpp-plugin/` — a
sibling of `crates/`, not a nested git repo, VST3-only for now, JUCE
8.0.10 pinned deliberately over the newer 9.0.1 (a major-version bump
isn't worth the risk 4 days out). `Pitchzazz.vst3` builds, installs to
`~/Library/Audio/Plug-Ins/VST3/`, and passes PluginVal at strictness 5.

Hit and root-caused one real bug on the way: the plugin built successfully
but failed to load in-process (`Unable to load VST-3 plug-in file`) —
turned out to be an x86_64/arm64 mismatch, the *same class* of
Rosetta-toolchain bug already documented for the Rust side's
`rustup`/libclang note in the root `CLAUDE.md`. This machine has two
Homebrew installs (Intel-Rosetta at `/usr/local`, Apple Silicon-native at
`/opt/homebrew`), and `/usr/local/bin` is earlier in `PATH`, so the
unqualified `cmake` CMake's Xcode generator ran with was the x86_64 one —
which reports `CMAKE_SYSTEM_PROCESSOR=x86_64` under Rosetta translation,
and JUCE's own `CMakeLists.txt` uses that to decide the default build
architecture when `CMAKE_OSX_ARCHITECTURES` isn't explicitly set. Fixed
by pinning `CMAKE_OSX_ARCHITECTURES=arm64` explicitly in
`cpp-plugin/CMakeLists.txt` rather than fighting the PATH/Homebrew
ordering — see the comment there for the full trail. Worth keeping in the
presentation narrative: same underlying environment-drift failure mode,
independently rediscovered and root-caused in both language stacks.

Port the *same algorithm* (McLeod pitch detection, same windowed pitch
shift approach) to C++ — not a different algorithm. The point of Phase 3's
comparison is language/implementation, and that claim only holds if both
sides are solving the identical problem the identical way. Reuse the
real-time-safety lessons from Phase 1 directly: same lock-free
producer/consumer shape, implemented with `juce::AbstractFifo` (or an
equivalent hand-rolled SPSC ring buffer) instead of `ringbuf`. Also port
the per-stage timing shape (`StageTimings` in `pitch-core`) so Phase 3's
benchmarking has identical measurement points on both sides.

**Day 2 checkpoint, 2026-08-17: passed.** Ported `pitch-core`'s DSP to
`cpp-plugin/Source/DSP/`: `Scale` (nearest-in-scale quantization, same
tie-breaking-toward-lower-note behavior), `PitchDetector` (McLeod/MPM,
ported directly from the `pitch-detection` crate's audited source, not
re-derived from the paper), `PitchShifter` (Bernsee-style phase vocoder,
ported from the `pitch_shift` crate's audited source), and `Corrector`
(orchestration + `StageTimings`, matching pitch-core's shape field-for-
field for Phase 3). All four Rust crates involved
(`pitch-detection`, `pitch_shift`, plus `rustfft`/`realfft` underneath)
were read from `~/.cargo/registry/src/` rather than ported from
intuition — same "audit real source" discipline `CLAUDE.md` rule 2
already requires for real-time-safety, applied here to numerical
fidelity instead.

The one subtle bug this surfaced: JUCE's `dsp::FFT::perform(..., inverse)`
auto-normalizes by `1/N`; `rustfft`/`realfft` (which both crates use)
normalize neither direction, leaving that to the caller — confirmed by
reading `rustfft`'s own doc comment and the vDSP-backed Apple engine in
JUCE's `juce_FFT.cpp`, not assumed. Both DSP classes compensate
explicitly (see the comments in `PitchDetector.cpp`'s
`normalizedSquareDifference` and `PitchShifter.cpp`'s `accOversamp`) —
missing this would have made pitch detection silently fail to ever cross
`CLARITY_THRESHOLD` and made the pitch-shifted output silently far too
quiet, neither of which would have thrown an error, just produced wrong
audio. Caught by two new tests with no Rust-side equivalent
(`PitchDetectorTests.cpp`'s sine-wave detection test,
`PitchShifterTests.cpp`'s energy-bounds test) written specifically
because the existing ported tests (`ScaleTests.cpp`, `CorrectorTests.cpp`
— direct ports of pitch-core's own suite) don't exercise the FFT path at
all. Also fixed two unrelated scaffolding defects while in
`CMakeLists.txt`: the `juce-dev`-generated project had defaulted to a
MIDI-effect plugin type (`IS_MIDI_EFFECT TRUE`,
`AU_MAIN_TYPE kAudioUnitType_MIDIProcessor`) rather than an audio effect,
and hardcoded the template's own `PLUGIN_CODE`/`PLUGIN_MANUFACTURER_CODE`
instead of reading ours from `.env`.

All 12 C++ test cases pass (56 assertions) via the `Tests` target; a
synthetic 220Hz sine is detected at 220.58Hz with 0.90 clarity. Rust's
`cargo test` (6/6) still passes, untouched. Not yet wired into
`processBlock` — that's Day 3.

**Day 3 checkpoint, 2026-08-17: passed.** Wired the ported `Corrector`
into real audio I/O, mirroring pitch-cli's architecture directly (see
`docs/ARCHITECTURE.md`) rather than inventing a different concurrency
shape for the plugin: `PluginProcessor::processBlock` downmixes to mono
into a fixed-size stack buffer (no heap allocation) and pushes into a
lock-free input `juce::AbstractFifo`; a new `CorrectorWorker`
(`Source/DSP/CorrectorWorker.{h,cpp}`) — a plain `juce::Thread` with no
deadline — accumulates 2048-sample blocks (matching pitch-cli's
`BLOCK_SIZE` exactly), runs one correction pass, and pushes the result to
a lock-free output FIFO; `processBlock` drains whatever's ready back out,
up-mixed to the host's channel count, filling silence on underrun rather
than blocking. `juce::AbstractFifo` was audited before use the same way
`ARCHITECTURE.md` audits `ringbuf` — read its actual header/cpp in
`~/.juce_cache/juce-src`, confirmed `Atomic<int>` index fields and no
internal mutex, not assumed from the class doc comment.

One real bug caught by that audit discipline, not by luck: `juce::Thread`
asserts (and is UB) if destroyed while still running — the first draft of
`prepareToPlay`/`releaseResources` called `worker.reset()` directly, which
would have hit that assertion the moment the host called either during
playback. Fixed by calling `worker->stopThread(1000)` before every
`reset()`. `setLatencySamples()` is called with the analysis block size as
a documented lower-bound estimate (block-accumulation delay only, not the
phase vocoder's own window latency on top) — consistent with the Rust
side's "measure honestly, don't just claim a number" standard; an actual
measurement isn't in this sprint's scope (see Day 4 below).

Added a minimal editor (tonic + major/minor combo boxes, a bypass toggle)
wired to `PitchzazzAudioProcessor::setScale`/`setBypassed`, which forward
to the worker via relaxed atomics — eventual consistency is fine for a
UI-driven parameter, unlike the audio-data FIFOs, so no lock or
message-thread synchronization needed there either.

Rebuilt clean, all 12 C++ tests still pass, and PluginVal (strictness 5)
passes again — now meaningfully harder than the Day 1 passthrough check,
since PluginVal exercises many different host block sizes/sample rates
against a plugin whose *internal* analysis block size (2048) is fixed and
decoupled from whatever the host hands it, which is exactly the
block-size-decoupling design `ARCHITECTURE.md` documents and the thing
most likely to break under a naive port.

Not yet done: loading in an actual DAW (PluginVal uses its own host
harness, not a real one) and profiling — both Day 4.

**Day 4, in progress, 2026-08-17.** Built AU and Standalone formats
(previously only VST3 had been built/validated) — both needed since
Logic Pro/GarageBand are AU-only and Ableton Live 12 (confirmed installed
on this machine) takes either. AU passes PluginVal too.

Then ran Apple's own `auval` (the validator Logic Pro/GarageBand use
internally) as a second, independently-implemented check against the AU
build — deliberately not relying on PluginVal alone. It caught a real bug
PluginVal missed entirely: `JUCE Assertion failure in juce_Thread.cpp:55`,
right before the final "AU VALIDATION SUCCEEDED" line. Root cause:
`juce::Thread`'s destructor asserts (undefined behavior) if the thread
object is destroyed while still running, and `PitchzazzAudioProcessor`'s
own destructor never stopped the worker — it relied entirely on the host
calling `releaseResources()` first, which isn't guaranteed (`auval`
doesn't). Fixed with a `stopWorker()` helper called from all three
lifecycle points that can destroy or rebuild the worker: `prepareToPlay`,
`releaseResources`, and the destructor itself. Re-ran `auval`: clean pass,
assertion gone. Full bug ledger — this one plus everything else found
across both engines and the toolchain — now lives in `docs/FINDINGS.md`,
added specifically for demo presentation per project direction.

**Day 4, continued.** Basic per-block profiling done: a new
`cpp-plugin/benchmarks/CorrectorPerformance.cpp` (Catch2 `Benchmarks`
target, Release build) measures the real DSP cost — the existing
`AudioProcessing.cpp` template benchmark measured `processBlock` in
isolation, which in this architecture only measures lock-free FIFO
push/pop overhead, not DSP cost (comment fixed to say so, see
`docs/FINDINGS.md` and `docs/PERFORMANCE_LOG.md`'s 2026-08-17
"Rust-vs-C++ per-block cost" entry for the full controlled comparison and
why the first-pass version of that comparison was wrong before it was
fixed). Full compare-and-contrast writeup — the actual Phase 3
deliverable — is done: `docs/COMPARISON.md`.

**Confirmed by manual check, 2026-08-17:** the **C++/JUCE plugin**
(`Pitchzazz.vst3`) loads and processes audio in Ableton Live 12 — the
real-DAW claim `auval`/PluginVal alone couldn't make. (The Rust engine has
no plugin form to test this way at all — `pitch-cli` is a standalone
`cpal` CLI with no VST3/AU packaging, so this check is C++-only by
construction, not by choice.) User-reported: "definitely audible
latency." Not a surprise and not a new bug — consistent with
`docs/ARCHITECTURE.md`'s Latency section and `docs/PERFORMANCE_LOG.md`'s
SOTA-comparison entry, which already put the *Rust* engine's analytical
mic-to-speaker latency estimate at ~70-100ms (block-accumulation delay +
the phase vocoder's ~50ms window) — and since the C++ port uses the same
block size, window size, and oversampling factor, the same magnitude
estimate applies to it too, not yet independently derived for C++
specifically. Human latency perceptibility becomes noticeable around
10-12ms, so an estimate in the 70-100ms range should be very obviously
audible, and it was, on the C++ build specifically. This is qualitative
confirmation the *magnitude* of the (Rust-derived, assumed-transferable)
estimate is in the right ballpark for C++ too, not a measurement of the
C++ engine on its own — `setLatencySamples()` in
`PluginProcessor::prepareToPlay` is still only the documented lower-bound
estimate (block-accumulation only), and the actual impulse/loopback
measurement (`docs/COMPARISON.md`'s "where we'd spend time next" item 3)
is still not done, for either engine. Don't let "it's audible, as
predicted" get mistaken for "it's measured."

**Open scope question, not yet decided:** the JD's application form asks
specifically which DAWs you've built plugins for and what plugin-framework
integrations you've done. A JUCE console/CLI app that merely links against
JUCE is weaker evidence for that than an actual loadable DAW plugin
(`juce::AudioProcessor`, built as VST3/AU, opens in a real host). Decide
before starting Phase 2 whether it targets a real plugin target from the
outset rather than a CLI-shaped C++ port with plugin work deferred to
Phase 4 — this changes Phase 2's scope materially and should be a
deliberate call, not a default.

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

**Deliverable, refined 2026-08-17:** the end product of this phase isn't
just a benchmark chart — it's a compare-and-contrast writeup of the two
implementations (design differences, where each language's tooling
helped or got in the way, where the measured performance actually
diverged and why) plus an explicit "where we'd spend time next and how
the data would justify it" section. That second part only gets written
once Phase 2/3 numbers exist and it's clear where the project actually
lands — don't speculate at it now, but keep it as the concrete goal this
phase is building toward, since it's what turns two implementations into
an interview narrative rather than two side projects. Feeds directly into
item 6 of the presentation walkthrough below.

**Done, 2026-08-17: `docs/COMPARISON.md`.** Written once real, controlled
numbers existed (Phase 2 Day 4's per-block benchmarking, see above) rather
than speculatively — covers what stayed identical between the two
engines, what had to diverge and why (FFT normalization convention, FFT
size constraints, toolchain fragility, thread-lifecycle semantics), the
controlled performance comparison, and a prioritized "where we'd spend
time next" section. The full runtime hot-swap half of this phase (backend
switching without dropouts) is still not done — this deliverable is the
benchmarking/writeup half only, as scoped in the 4-day sprint plan.

**Hot-swap, in progress, 2026-08-17.** Architecture built generically on
purpose, per project direction — a `PitchEngine` interface
(`cpp-plugin/Source/DSP/PitchEngine.h`) any algorithm implements, and an
explicit `PitchEngineRegistry` (name → factory function list) that both
the eventual UI and `CorrectorWorker` enumerate rather than hardcoding
"Rust vs. C++" as a special case — adding a third algorithm later (a
TD-PSOLA implementation, or a same-algorithm different-window-size
variant per the Phase 5 latency idea above) means implementing the
interface and adding one registry line, nothing else. `CorrectorWorker`
now holds an engine behind an atomic pointer handoff (`requestEngineSwap`)
instead of owning a fixed `Corrector` — same lock-free
single-producer/single-consumer shape as the audio FIFOs, but for handing
off one owned object instead of a sample stream; picked up between
blocks, never mid-`process()` call, so `processBlock` is unaffected by a
swap in progress.

The Rust engine is now reachable from C++ for real: a new
`crates/pitch-core-ffi` crate bridges `pitch-core` via
[`cxx`](https://cxx.rs/), built and linked into the JUCE plugin via
[Corrosion](https://github.com/corrosion-rs/corrosion) (both chosen after
checking what existed already, per project direction, rather than
hand-rolling raw `extern "C"`/`cbindgen`/manual `cargo` invocation from
CMake — see `docs/COMPARISON.md`-adjacent research). Hit two real
Corrosion-specific naming gotchas along the way (`docs/FINDINGS.md` #12,
#13) — both root-caused by inspecting actual generated output rather than
trusting either tool's stated conventions. `RustCorrectorEngine` now
wraps the FFI bridge behind `PitchEngine`, alongside the existing
`NativeCorrectorEngine`; a new smoke test
(`tests/DSP/RustCorrectorEngineTests.cpp`) proves the bridge produces
*correct* DSP output, not just that it links — a synthetic 220Hz sine
through the actual Rust engine via FFI detects at 220.578186035Hz,
matching the native C++ engine's 220.57824707Hz to float-precision noise,
genuinely the same algorithm producing the same answer through two
different call paths. All 14 C++ test cases (65 assertions) pass,
PluginVal still passes.

**Not yet done:** the actual atomic swap has never been exercised live
(no UI control to trigger it yet), and the dropout-detection test
`docs/TESTING.md` planned for validating it hasn't been built. Both
engines existing and being individually correct is necessary but not
sufficient for the hot-swap claim — the swap mechanism itself is still
unproven.

**Hot-swap validated, 2026-08-17.** Both closed out:

- **Engine selector added** to the editor (`PluginEditor.h`/`.cpp`) —
  populated generically from `pitchzazz::availableEngines()`, no
  hardcoded engine names, so it needs no changes when a third algorithm
  module is added later.
- **Dropout-detection test built and, along the way, found a real bug**:
  `tests/DSP/HotSwapDropoutTests.cpp` (`docs/TESTING.md`) caught an actual
  audible-scale discontinuity at the swap boundary — each engine's
  independent internal phase-vocoder state meant an instant swap started
  the incoming engine cold. Fixed with a crossfade in `CorrectorWorker`
  (both engines run in parallel briefly after a swap, blended, before
  fully switching over); iterated twice against real measured numbers
  (1-block crossfade: partial improvement, still failing; 4-block: passes
  comfortably) rather than adjusted by loosening the test threshold — see
  `docs/FINDINGS.md` #14 and `docs/TESTING.md` for the full numbers and
  the diagnostic reasoning at each step.

All 17 C++ test cases (235 assertions) pass, PluginVal passes. The
hot-swap claim is now backed by a test that would fail if it regressed,
not just by the architecture existing.

**Real-DAW listening test, 2026-08-17: engine selector confirmed working
in Ableton** (after two unrelated build hygiene issues along the way —
the AU format hadn't been rebuilt alongside VST3 during this work, and a
JUCE Standalone properties file needed clearing — neither a code bug).
The listening test itself found something the automated test didn't: a
quiet audible click at the swap point survived the passing 4-block
crossfade. Switched the blend to equal-power (cos/sin gains, fixes the
loudness dip a linear amplitude crossfade causes) — user-confirmed
"better, but still there." Root cause of the residual: the two engines'
phase accumulators evolve independently with no shared reference point,
which amplitude-blending alone can't fully resolve regardless of curve
shape. **Decision, per project direction: document as a known, understood
limitation and move on**, rather than keep pulling the same lever — full
detail and numbers in `docs/FINDINGS.md` #14 and `docs/TESTING.md`. A
fundamentally different technique (e.g. a brief duck/mute at the swap
point, or synchronizing phase state before swapping) would be the next
real lever if this gets revisited, not a longer crossfade.

## Phase 4 — GUI

A minimal JUCE-based UI: backend selector (Rust/C++), tonic/scale controls,
a live processing-time meter. Doesn't need to be elaborate — it needs to
make the hot-swap and the performance comparison visible and demoable live.

## Phase 5 — polish

**Algorithm playground, started 2026-08-17.** Reskin (below) done, then
picked up the "make this a playground for pitch-manipulation algorithms"
direction explicitly: scoped to TD-PSOLA first (the concrete lowest-
latency claim, most directly tied to the JD's real-time-systems angle),
C++ only (the Rust/C++ parity story is already proven by the existing two
engines — a third algorithm doesn't need to re-prove it, per this file's
own prioritization criteria). LPC-based formant shifting and granular
shifting (the timbre-changing/creative-tool directions) are scoped but
not started — natural next additions to the same registry, not attempted
in this pass.

**TD-PSOLA, done, 2026-08-17.** `PSOLACorrectorEngine` (id `psola-cpp`,
display name "TD-PSOLA (C++)") — one more `PitchEngine` registry entry,
no changes anywhere else (UI, worker, hot-swap), exactly the extensibility
the registry was built for. Reuses the existing `PitchDetector` and
`Scale` quantization unchanged; only the shift stage is new
(`PSOLAPitchShifter`, a separate class from `Corrector`/`PitchShifter`
rather than a template — see `PSOLACorrector.h`'s doc for why duplication
was the safer call at N=2 shifters). All new tests pass
(`tests/DSP/PSOLAPitchShifterTests.cpp`: energy preservation, a frequency-
round-trip correctness check via re-detection, silence handling, and a
latency-formula assertion), PluginVal and `auval` both pass on the
rebuilt AU/VST3.

Real measured latency win — see `docs/PERFORMANCE_LOG.md`'s dated entry
for the full numbers and the genuinely-real-not-just-derived-latency
verification: constant in milliseconds across sample rates unlike the
phase vocoder's sample-count-based window (a real property of the
algorithm, not a limitation of this implementation). Finding the first
version of this number required fixing a genuine floating-point
robustness bug in the grain-read-position math, found via direct
instrumentation after an initial measurement looked like an unexplained
overshoot — full story in `docs/FINDINGS.md` #18, worth reading for the
"increasing a safety margin didn't change the size of the gap, and that
itself was the diagnostic clue" narrative.

**Revisited same day, three times, all user-directed.** (1) Tightened
`minHz` 60->80 (still below typical bass vocal range, less headroom) for
a pure, host-safe latency reduction — a fully *adaptive* (pitch-tracked)
alternative was raised and explicitly rejected: a DAW host's plugin-delay-
compensation needs one constant reported latency that the actual output
delay always matches, and varying it at runtime risks the same
host-compatibility problem this file's own "dynamic/hybrid window sizing"
idea below already flagged for the phase vocoder. (2) Real listening on
live audio (not the synthetic tones the unit tests use) found a crackle/
low-frequency-beat artifact; root-caused to `placeGrainAt` snapping to a
single analysis bucket instead of cross-fading between the two nearest,
and "fixed" — at the cost of one more period of lookahead. (3) Real
listening on the rebuilt cross-fade version found the artifact **still
present, possibly worse** — reverted back to single-bucket entirely
rather than keep tuning without being able to verify by ear.
**Current, final state:** single-bucket `placeGrainAt`, `latencySamples
= 2 * maxPeriodSamples`, ~25.0ms at both 44.1kHz and 48kHz — ~46%/~41%
less than the phase vocoder, the full gain from (1) alone, since (3)
gave none of it back. The crackle/beat artifact is a documented, accepted
limitation of this simplified PSOLA design, not something worth chasing
further within this project's timeline — the real fix (correlation-based
grain alignment before blending) is bigger scope, and this engine's
demonstrated value is the latency comparison against the phase vocoder,
not production-grade reconstruction quality. Full accounting — including
three automated-test approaches tried for the cross-fade specifically,
none of which actually verified it (matches this project's own finding
#14 precedent that a passing metric and real perceptual quality aren't
always the same bar), the comb-filtering hypothesis for why cross-fading
didn't help, and why a high/low-pass filter was considered and rejected
as a fix — in `docs/PERFORMANCE_LOG.md`'s two follow-up dated entries and
`docs/FINDINGS.md` #19-20.

Separately, the same day: microphone input in the Standalone app, broken
for the entire session up to this point (blocking any live-mic testing),
was root-caused and fixed — `CMakeLists.txt`'s `juce_add_plugin()` never
set `MICROPHONE_PERMISSION_ENABLED`, so the built app's `Info.plist` was
missing `NSMicrophoneUsageDescription`, which modern macOS treats as
silent permission denial (no crash, no error, no samples). See
`docs/FINDINGS.md` #21.

- Note-matching refinement: handle edge cases beyond the current nearest-
  semitone search (e.g. weighting toward the previous detected note to
  reduce warble on sustained notes near a scale boundary).
- Scale-selection UI: live key/mode switching from the GUI, not just a CLI
  flag at startup.
- **Live detected/corrected pitch display, done 2026-08-17:** two
  LCD-style readouts (`pitchzazz::LCDDisplay`, `MeterComponents.h`) show
  the detected pitch and the pitch it's being corrected to, each as note
  name + Hz. Corrected Hz is derived in the editor
  (`detectedHz * 2^(semitoneShift/12)`) rather than computed separately
  on the DSP side — `CorrectionResult` already carries both inputs to
  that formula. Deliberately not EMA-smoothed like the timing meters:
  smoothing a pitch reading would visually fabricate a glide between
  notes that never happened in the actual audio. New `CorrectorWorker`/
  `PluginProcessor` getters (`getLastDetectedHz`/`getLastSemitoneShift`)
  mirror the existing timing-getter pattern exactly.
- **Tuner-style cents meter, done 2026-08-17** — a natural follow-on to
  the live pitch display above, prompted by a reference screenshot of
  Antares Auto-Tune Pro's central "how in tune is the input" dial.
  `pitchzazz::centsOffsetFromNearestNote` (`Corrector.h`/`.cpp`) is a new
  free function, tested in `CorrectorTests.cpp` alongside `hzToMidi`/
  `midiToHz`: signed cents offset of the detected pitch from the
  *nearest chromatic* note, bounded to [-50, +50] by construction of
  nearest-integer rounding — deliberately not measured against
  `Corrector::process()`'s scale-corrected target (`semitoneShift`),
  since that can be a several-hundred-cent jump whenever the nearest
  chromatic note isn't in the active scale, which would pin a "how in
  tune" reading at an extreme for most out-of-scale content instead of
  showing genuine intonation. `pitchzazz::CentsMeter`
  (`MeterComponents.h`) renders it as a thin horizontal gauge under the
  DETECTED LCD display (a linear strip rather than Auto-Tune's circular
  dial — this plugin's window is compact) — ticks at -50/-25/0/+25/+50,
  a filled span from centre to the current reading, green once within
  +-5 cents (the rough tolerance hardware tuners typically use for their
  in-tune LED) and dimmed to near-invisible on silence/unvoiced input,
  so "no signal" can't be mistaken for "singing exactly in tune." Lightly
  EMA-smoothed in the editor (same alpha as the performance meters) but
  explicitly *not* the same no-smoothing choice the note-name display
  above makes: a name is categorical, so smoothing it fabricates a fake
  glide between notes, but a cents offset is continuous, so smoothing it
  only damps block-to-block jitter, the same reasoning real hardware
  tuner needles rely on — snaps instantly (skips one tick of blending)
  on voicing onset and on an engine switch rather than blending from a
  stale reading that belongs to a different note or engine. All 23 C++
  test cases (247 assertions) still pass.
- **Additional musical modes, raised 2026-08-17, done 2026-08-18:** `Scale`
  already generalized to any diatonic mode as a fixed interval pattern
  rotated by the tonic (not a lookup table), so adding the other five
  church modes (Dorian, Phrygian, Lydian, Mixolydian, Locrian) turned out
  to be exactly as cheap as flagged — confirmed, not assumed, by reading
  `rust_music_theory`'s actual source (`~/.cargo/registry/src/`) rather
  than trusting it has full church-mode coverage: `scale::Mode` already
  has all seven variants, and `Scale::notes()`/`ScaleType::from_mode()`
  handle them generically via interval rotation, so `pitch-core`'s
  `scale.rs` needed zero algorithm changes. The actual work was
  `crates/pitch-cli/src/main.rs`'s `--mode` parsing (extended to accept
  all seven names) and, on the C++ side, `Scale.h`/`Scale.cpp` (five new
  `ScaleMode` enum values + five new interval-pattern arrays, derived by
  independently rotating the major scale's own step pattern from each
  degree — cross-checked by construction, since rotating from the 6th
  degree reproduces the pre-existing, independently-authored
  `minorIntervals` exactly) and the GUI's mode combo box (7 items now,
  major/minor kept at their original IDs 1/2 so existing muscle memory
  still works).

  **One real scope gap this surfaced, not anticipated when the item was
  queued:** `RustCorrectorEngine`'s FFI bridge to the Rust engine only
  ever passed a binary `is_minor: bool` across to Rust, not a full mode —
  `Scale`/`ScaleMode` is shared across all three engines (unlike PSOLA,
  which is deliberately C++-only), so leaving that bridge binary would
  have meant picking e.g. Dorian while the Rust engine was active
  silently corrected into the wrong scale, no crash, just wrong music —
  exactly the class of silent-wrong-audio failure this project exists to
  catch, not introduce. Fixed by widening the bridge
  (`crates/pitch-core-ffi/src/lib.rs`) from `is_minor: bool` to a shared
  `mode: u8` index convention (cxx can't share a Rust enum with C++
  directly), mirrored explicitly — not via a `static_cast` that could
  silently break if either enum's declaration order ever changes — in
  `RustCorrectorEngine.cpp`'s new `toModeIndex()`. Not treated as a
  `docs/FINDINGS.md` entry: no shipped behavior was ever wrong (Dorian
  selection didn't exist until this same change), so this is scope
  discovered and closed within one feature, not a bug found in existing
  code.

  New tests: one Dorian-vs-Ionian test per side (C major and C Dorian
  share a tonic but differ at the 3rd/7th degrees, so this actually
  exercises mode-rotation logic, unlike testing two scales that happen to
  share the same pitch-class set). All Rust tests pass (7/7,
  `cargo test --workspace`) and `cargo clippy --all-targets --workspace`
  is clean; all C++ tests pass (33/33 cases, 272 assertions, up from
  32/270 — the one new Dorian test — via the `Tests` target built
  headlessly, no Xcode GUI).
- **MIDI-controlled pitch target ("vocoder mode"), added 2026-08-17:**
  instead of (or in addition to) snapping to the nearest note in a fixed
  scale, let an incoming MIDI note set the target pitch directly —
  classic vocoder/talk-box-style behavior (play a chord, the voice
  harmonizes to it). This is *why* the `cpp-plugin` scaffold initially
  came up configured as a MIDI effect (`docs/FINDINGS.md` #8) — not a
  scaffolding mistake so much as an early signal of this feature, just
  configured as the wrong plugin *type* for it (a JUCE "MIDI effect" is
  MIDI-only, no audio I/O, which can't host audio pitch correction
  regardless). Implementing this needs `NEEDS_MIDI_INPUT TRUE` plus real
  logic in `Corrector`/`CorrectorWorker` to read incoming MIDI notes and
  override `nearestInScaleMidi`'s target — deliberately out of scope for
  the 4-day sprint (`docs/ROADMAP.md`'s prioritization criteria).
- **Expose `overSampling` as a user-facing quality control, added
  2026-08-17:** it meaningfully affects phase-vocoder reconstruction
  quality (`docs/PERFORMANCE_LOG.md`'s "OVER_SAMPLING re-evaluated"
  entry) and, unlike most quality parameters, is already passed per-call
  to `shiftPitch` rather than fixed at construction — so it can be
  changed live without reconstructing the shifter or risking a dropout,
  cheaper to wire up than it might look. Blocked on the listening test
  that entry flagged as missing: without knowing whether e.g. 8 vs. 16
  vs. 32 is actually audibly distinguishable, there's no basis for
  choosing a control's range (raw numeric knob vs. something like
  "Draft/Good/Best" tiers). Do the listening test before building the UI
  for this, not after.
- **Per-algorithm creative parameter exposure, raised 2026-08-17.** Two
  different tiers, not equally valuable:
  1. **The classic Auto-Tune controls — done, 2026-08-17.** Correction
     amount (0-100%, blends the applied shift towards "no correction") and
     retune speed (0-1000ms one-pole glide time constant, 0 = this
     project's original instant snap) — see `cpp-plugin/Source/DSP/
     RetuneSmoothing.h` for the two formulas (`applyCorrectionAmount`,
     `glideTowards`), kept in one shared header specifically so the
     control means the same thing on both C++ engines even though
     `Corrector`/`PSOLACorrector` themselves stay deliberately separate,
     near-duplicate classes (see `PSOLACorrector.h`'s own doc for why).
     `glideTowards`'s time constant is derived from `blockPeriodMs` on
     every call rather than cached, so it stays correct across sample-rate
     changes without extra plumbing. `PitchEngine::setCorrectionAmount`/
     `setRetuneSpeedMs`/`supportsRetuneControls` are default no-op virtuals
     (not pure virtual), so the Rust engine (`RustCorrectorEngine`) needed
     zero changes to keep compiling — same "C++ only for now" precedent
     already set for the PSOLA engine itself. The GUI's engine selector
     already disables the two new sliders when the active engine reports
     `supportsRetuneControls() == false`, rather than accepting input that
     would silently do nothing. Reporting choice worth noting: `Corrector`/
     `PSOLACorrector::process()` now put the *applied* (amount- and
     glide-adjusted) shift into `CorrectionResult::semitoneShift`, not the
     raw full-snap target — which means the existing "CORRECTED" LCD
     readout (`PluginEditor.cpp`, added in the previous session) now
     automatically reflects what the audio is actually doing under partial
     correction/an in-progress glide, with no further wiring needed.
     Verified: 9 new test cases across `tests/DSP/RetuneSmoothingTests.cpp`
     (the two formulas in isolation, exact values), `CorrectorTests.cpp`,
     and the new `PSOLACorrectorTests.cpp` (amount attenuation, instant
     snap at speed 0, gradual convergence at speed > 0) — full suite now
     31 cases / 266 assertions, all passing. VST3 rebuilt and re-passes
     PluginVal at strictness 5.
  2. **Internal DSP constants, lower creative value but cheaper to wire —
     grain width done, 2026-08-18; `overSampling` remains blocked on a
     listening test.** PSOLA's grain half-width, previously locked to
     exactly one period, is now a 0.5x-1.5x multiplier
     (`PSOLAPitchShifter::setGrainWidthMultiplier`,
     `grainWidthMultiplierMin`/`Max` in `PSOLAPitchShifter.h`) — a "Width"
     slider in the GUI, disabled (not hidden) on any engine besides PSOLA
     via a new `supportsGrainWidthControl()` capability flag on
     `PitchEngine` (same default-no-op-virtual shape as the retune
     controls above, so `NativeCorrectorEngine`/`RustCorrectorEngine`
     needed zero changes). C++-only, matching PSOLA's own existing
     precedent.

     **The upper bound is not a free musical choice — it directly sets
     this engine's fixed worst-case latency**, and that constraint changed
     the shipped range: `getLatencySamples()` must report one constant
     number sized to safely cover every value the multiplier could ever
     be set to (`2 * ceil(maxPeriodSamples * grainWidthMultiplierMax)`,
     generalizing the pre-existing `2 * maxPeriodSamples` formula from
     "period" to "half-width"), not whatever it happens to be set to right
     now. A first attempt at a 3.0x ceiling was caught by
     `tests/DSP/PSOLAPitchShifterTests.cpp`'s existing latency-formula
     test before it shipped: worst-case latency at 44.1/48kHz jumped to
     3312/3600 samples (~75ms) — *worse* than the phase vocoder's
     ~42.7-46.4ms, which would have silently defeated this entire engine's
     reason for existing (docs/PERFORMANCE_LOG.md's SOTA-comparison
     entry) the moment a user so much as saw the control, regardless of
     what they set it to. Recomputed against 1.5x instead: 1656/1800
     samples, ~37.5ms at both rates — still a real, meaningful win over
     the phase vocoder (in the same ballpark as the ~19%/~12% net
     reduction this project already accepted as legitimate after the
     cross-fade fix cost latency back, per `docs/PERFORMANCE_LOG.md`'s
     "tighter floor" entry), at the cost of a narrower creative range than
     a first instinct might have picked. The lower bound (0.5x) has no
     such constraint — narrowing the grain only ever *reduces* the worst
     case — so it's the ordinary "no listening-test tooling to validate a
     precise number" judgment call already used for `overSampling`/
     `retuneSpeedMs` elsewhere in this codebase.

     Also closed a latent (pre-existing, not introduced by this feature)
     gap while in this class: `grainWindow`'s capacity is now reserved
     once at construction to the worst case, so a grain-width change at
     runtime — which can now need a bigger grain than pitch detection
     alone ever demanded, even after this vector already "warmed up" at
     the 1.0x default — can never trigger a reallocation on the worker
     thread. Not itself an observed bug (nothing was seen to break), so
     no `docs/FINDINGS.md` row — a proactive tightening of an existing
     "shiftPitch() itself doesn't allocate" claim that this feature made
     more likely to be tested.

     Verified: 4 new test cases in `tests/DSP/PSOLAPitchShifterTests.cpp`
     (the corrected latency formula; out-of-range clamping; 1.0x
     reproduces the original fixed-width output exactly, sample-for-sample,
     against a shifter that never calls the new setter at all; energy
     bounds at the multiplier floor) — full suite now 35 cases / 2323
     assertions (the per-sample equality check accounts for most of that
     count), all passing, run headlessly via `cmake --build` + the `Tests`
     binary directly (no Xcode GUI, no display in this environment). A
     dated `docs/PERFORMANCE_LOG.md` entry has the full latency numbers,
     per this codebase's rule 6 — this is a performance-relevant parameter
     change, not just a feature addition. Not yet done:
     `benchmarks/PSOLALatencyProbe.cpp`'s onset probe predates this
     control and needs re-running against the new worst-case formula to
     confirm the derivation is exact (not just internally consistent),
     the same "measurement over argument" standard this project applies
     elsewhere — flagged, not done in this pass.
  (1) done first, per this section's own original ordering call — it's the
  more recognizable, more requested category of control for this kind of
  tool. (2) is now also done (grain width; `overSampling` remains queued
  behind the listening-test blocker).
- **Latency improvement ideas, added 2026-08-17 — revisit after hot-swap
  is solid, not before.** Two directions identified while reviewing the
  measured-latency work (`docs/PERFORMANCE_LOG.md`'s "Measured pipeline
  latency" entry, which established that latency = the phase vocoder's
  `window_size_ms` directly, nothing else in the pipeline currently adds
  to it):
  1. **Content-aware static window size.** Pitch detection needs the
     window to span at least ~1-2 periods of the lowest expected
     fundamental to work reliably — a bass voice (~80-150Hz) needs a
     meaningfully longer window than a soprano or a higher-pitched
     instrument (~250Hz+). If the expected material is known ahead of
     time (a user-selected voice-type/instrument preset, similar in
     spirit to the `overSampling` control above), a shorter window — and
     therefore directly lower latency — becomes safe to use without
     sacrificing detection quality for that material. Low complexity:
     mostly parameter exposure, not new DSP.
  2. **Dynamic/hybrid window sizing.** Instead of a fixed window chosen
     upfront, continuously estimate the actual pitch range present in
     the incoming audio on a non-real-time thread (the existing worker
     thread already has no deadline — see `docs/ARCHITECTURE.md`), and
     retune the real-time-facing window size to the shortest one that
     safely covers the currently-detected range. Meaningfully higher
     complexity than idea 1: `PitchShifter`/`PitchDetector`'s internal
     buffers are sized once at construction (`frameSize` isn't
     reconfigurable in place), so changing window size live means
     switching between pre-built instances at different sizes without a
     dropout — **the same atomic-swap problem the hot-swap feature is
     about to solve, just swapping between differently-configured
     instances of one engine instead of between the two engines.**
     Building hot-swap's swap mechanism generically (parameterized over
     "which engine instance," not hardcoded to "Rust vs. C++") would
     make this idea meaningfully cheaper later — worth keeping in mind
     during the hot-swap implementation even though this feature itself
     is out of scope now. Also needs hysteresis (avoid oscillating
     window size when content straddles a boundary) and a real answer
     for `setLatencySamples()` — most DAW hosts handle a plugin's
     reported latency changing at runtime poorly or not at all, which
     could be a hard blocker rather than a polish item; would need
     researching before committing to this direction, not assumed
     workable.
- **Richer developer telemetry in the GUI, added 2026-08-17**: beyond the
  live processing-time meter (in progress now), logging/plotting other
  interesting live data (e.g. a scrolling per-stage timing graph, hot-swap
  crossfade progress, detected-pitch history) for developer show-and-tell
  purposes. Explicitly not a priority right now, per project direction —
  noted so it doesn't get lost, not queued.
- **Visual redesign, done 2026-08-17**: replaced JUCE's default grey
  look and feel with a dark-background/neon-accent theme
  (`Source/PitchzazzLookAndFeel.h`) — custom `LookAndFeel_V4` colour
  scheme plus rounded/neon-outlined combo boxes and an LED-style bypass
  toggle, each performance meter colour-coded from the same shared
  palette. Styling-only, no layout/behavior changes to anything below.

## Presentation walkthrough tooling

**Modern C++ best-practices audit, added 2026-08-17 — do before building
the walkthrough below, not after.** A pass over the whole C++ codebase
(not just the newest PSOLA code) checking for dated patterns a Sr Audio
Core reviewer would notice: raw owning pointers where `unique_ptr`/
`shared_ptr` belong, missing `noexcept`/`[[nodiscard]]` where warranted,
`const`-correctness gaps, places a range-based `for` or `<algorithm>` call
would replace a manual index loop, unnecessary copies where a reference
or `std::move` belongs, and consistency with whatever subset of C++17
this project already commits to elsewhere. Scoped as an audit-and-fix
pass across the existing code, not a rewrite — the point is the
walkthrough holding up to scrutiny on *every* file a reviewer might open,
not just the ones freshly written.

**Done, 2026-08-17.** A file-by-file pass over `cpp-plugin/Source/`,
`cpp-plugin/benchmarks/`, and `cpp-plugin/tests/` against the checklist
above. The codebase held up well — most of the checklist items (raw
owning pointers, const-correctness, manual-loop-vs-range-for/`<algorithm>`,
unnecessary copies) turned up nothing: constructors/setters already take
`const&` where warranted, index loops found were all genuinely
index-dependent (bin/sample-position math, not simple element iteration),
and ownership was already `unique_ptr`-based almost everywhere. Two real,
fixable categories did turn up, both applied:

- **Own heap allocations via `std::unique_ptr`/`make_unique`, never a raw
  `new` handed to `.reset()` or a bare owning pointer with a manual
  `delete`** — clearer ownership and exception-safe by construction (a
  `unique_ptr` cleans up even if code between construction and the old
  manual `delete` throws; the manual version didn't). Two instances fixed:
  `StandaloneApp.cpp`'s `mainWindow.reset (new
  juce::StandaloneFilterWindow (...))` → `std::make_unique`, and
  `tests/helpers/test_helpers.h`'s raw `editor` pointer with a manual
  `delete` at the end of the function → a `unique_ptr` with a custom
  deleter (needed anyway, since JUCE's `editorBeingDeleted()` must run
  immediately before the delete, not just at some later point).
- **Mark functions `noexcept`/`[[nodiscard]]` when their implementation
  provably can't throw or their return value is never meaningful to
  discard, applied consistently within a class, not just on the newest
  code.** `PluginProcessor.h`'s inline getters (`isBypassed`, `getScale`)
  were already `noexcept`, but eight sibling getters implemented
  out-of-line in the `.cpp` (`getActiveEngineName`, `getActiveLatencyMs`,
  `getLastDetectUs`/`getLastQuantizeUs`/`getLastShiftUs`,
  `getLastDetectedHz`, `getLastSemitoneShift`, `getBudgetUs`) — every one
  of them either an atomic load or simple arithmetic, none allocating —
  weren't, an inconsistency within one class rather than a deliberate
  choice. Separately, added `[[nodiscard]]` to pure query/result-producing
  functions whose return value being silently discarded would be a bug,
  not a style issue: `hzToMidi`/`midiToHz`/`nearestInScaleMidi`/
  `Scale::containsPitchClass` (free/query functions), `createEngine`
  (discarding it silently drops the constructed engine), `PitchDetector::
  detect`, and `process()`/`getLatencySamples()` across `Corrector`,
  `PSOLACorrector`, `PitchShifter`, `PSOLAPitchShifter`, the `PitchEngine`
  interface, and all three engine implementations.

No correctness bugs were spotted during the audit (the constraint was:
if one turned up, note it rather than fix it inline — nothing to note
this time). Verified zero behavior change: rebuilt the `Tests` target
headlessly (Xcode-generator `cmake --build`, no GUI) and all 22 test
cases / 243 assertions still pass; `Benchmarks` still builds clean in
Release and a spot-check run (`Corrector::process cost at 44100Hz`) still
executes and reports timings in the same order of magnitude as
`docs/PERFORMANCE_LOG.md`'s existing numbers. See `cpp-plugin/CLAUDE.md`'s
new "C++ code quality standards" section — the two categories above are
now codified as durable rules there, not just a one-time cleanup, so new
code doesn't reintroduce them.

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
7. `docs/FINDINGS.md` (added 2026-08-17, Phase 2 Day 4) — the
   consolidated bug ledger across both engines. Point to this directly
   rather than re-narrating each bug from memory; it's already
   structured as the demo needs it (what broke, what caught it, why, what
   the fix was), including the cross-tool-coverage story (PluginVal
   missing a bug `auval` caught) that's worth calling out explicitly.

The tour should be runnable both live (stepping through in an editor during
the actual presentation) and asynchronously (a reviewer working through it
alone), since it's not yet clear which format the interview will want.

**Done, 2026-08-17.** Built as a single VS Code CodeTour file,
`.tours/pitchzazz-walkthrough.tour`, with the 7 stops above in the exact
order listed, each anchored at real, currently-existing code or docs
(`crates/pitch-cli/src/main.rs` x3, `crates/pitch-core/src/scale.rs`,
`docs/PERFORMANCE_LOG.md`, `cpp-plugin/Source/DSP/PitchEngine.h`,
`docs/FINDINGS.md`) with self-contained description text — written to be
read alone by a reviewer, not just narrated live, so no step says
"explain here" without the actual explanation attached. Satisfies the
"runnable both live and async" requirement above: CodeTour supports
stepping through interactively in VS Code, or just opening the `.tour`
JSON/reading the descriptions directly.

Every step uses both a `line` guess and a `pattern` (a distinctive text
snippet CodeTour uses to relocate the step if the file changes around
it) rather than a bare line number — chosen deliberately, not as
boilerplate: this tour was built while two other sessions were
concurrently editing some of the same anchor files (`scale.rs`,
`main.rs`, `PitchEngine.h`) for unrelated features, so line-number-only
anchors would have been fragile by construction. All 7 anchors were
verified against the actual file contents at build time, not assumed.
