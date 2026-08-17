# Testing methodology: from "does it sound right" to "does it pass"

Written for the demo, at the user's request — the point isn't just what
these tests check, it's the underlying reframe: turning a question that
used to require a human ear ("does this glitch, does this sound clean")
into a question a computer can answer deterministically and repeatably.
Manual listening doesn't scale, doesn't catch regressions automatically,
and can't run in CI — the tests below are the same QA questions, rebuilt
as assertions on a known, synthetic signal.

**Status check, so nothing here is overclaimed:** everything below is
real, in the repo, passing right now — including the hot-swap dropout
tests, which were genuinely a plan (not a result) when this doc was first
written and have since been built. Updated in place rather than left
stale once that changed.

## The core technique, used by every test below

1. Generate a **known, synthetic input** — a sine wave at a specific
   frequency, a single-sample impulse, a continuous tone — never real
   recorded audio, because you need to know the *correct answer* ahead of
   time to assert against it.
2. Run it through the **actual production architecture**, not a
   simplified stand-in — the real `CorrectorWorker`/`AbstractFifo` pair
   for the C++ engine, the real ring-buffer/worker shape for Rust — so
   the test exercises the same code path a user's audio does, not a
   simplified approximation of it.
3. Assert something **objective and numeric** about the output: a
   measured frequency, an energy bound, a sample index, a discontinuity
   count. Not "does this sound OK" — a number, with a pass/fail threshold
   decided in advance.

## Already built and passing

### Detection accuracy — turns "does pitch detection work" into a number

`cpp-plugin/tests/DSP/PitchDetectorTests.cpp`: feed a synthetic 220Hz sine
wave into `PitchDetector::detect`, assert the detected frequency is
within 2Hz of 220 and clarity is above 0.9. Real result: measured
220.58Hz, 0.90 clarity. This is what actually validated the McLeod port's
FFT math was correct — not listening to it, computing whether the
reported number matches the known-correct answer.

### Energy/sanity bounds — catches "silent" or "exploding" output without listening

`cpp-plugin/tests/DSP/PitchShifterTests.cpp`: run several blocks of a
steady tone through `PitchShifter` at 0 semitones shift, assert the
output RMS energy stays within a generous band (0.3x-3x of the input's).
This test exists *specifically* because it's exactly the kind of bug a
human wouldn't reliably catch by ear in a quick check, but a computer
catches instantly: this project actually shipped a bug where JUCE's FFT
auto-normalization was uncompensated, which would have made the output
either silent or blown out by orders of magnitude (`docs/FINDINGS.md` #10)
— caught before it ever needed a human to notice.

### Impulse-response latency measurement — turns "how much delay" into a measured number

`cpp-plugin/benchmarks/LatencyProbe.cpp` / `crates/pitch-cli/examples/latency_probe.rs`:
feed a single-sample impulse through the real pipeline, find where its
energy peaks in the output. Cross-correlating anything against a literal
impulse reduces mathematically to finding the response's peak — this
isn't an approximation, it's the standard technique evaluated at its
simplest input. Real result: this is how `docs/PERFORMANCE_LOG.md`'s
measured 43-50ms latency numbers were produced, replacing what had been a
pure analytical estimate.

### PluginVal / `auval` — host-level validation neither unit tests nor benchmarks can do

These aren't hand-written tests, they're third-party tools that simulate
a real DAW host: load the plugin, exercise it across many sample
rates/block sizes, drive its lifecycle (create, prepare, process, tear
down) the way a real host would. This is a genuinely different layer from
the tests above — it caught a bug none of the unit tests could have,
because the bug wasn't in the DSP math at all: `juce::Thread` asserting
when destroyed while still running (`docs/FINDINGS.md` #11), a lifecycle
bug that only manifests when something actually drives the plugin's
create/destroy sequence the way a host does. Worth noting for the demo:
PluginVal missed this one and `auval` caught it — two independently-
implemented validators, not redundant with each other.

## Hot-swap dropout detection — and what it found

`cpp-plugin/tests/DSP/HotSwapDropoutTests.cpp`. A dropout is a
*discontinuity* — a click, a pop, a gap — which is exactly the kind of
thing that's easy to hear and hard to describe as a single number. This
suite turns "does this click" into something computable, deterministically:

1. Feed a continuous, known synthetic tone through the real
   `CorrectorWorker`/FIFO pipeline, one block at a time, waiting for each
   block's output before pushing the next — deterministic control over
   exactly which block boundary a swap lands on (unlike pushing a whole
   signal at once, which is fine for a one-shot latency measurement but
   not for pinning a swap to a specific point).
2. Request an engine swap at a known block index, mid-stream, while the
   signal keeps flowing.
3. Capture every output sample continuously through the swap.
4. Check three independent properties: **sample accounting** (output
   count matches input count exactly — catches lost/duplicated samples
   in the handoff, independent of whether the audio *sounds* wrong),
   **discontinuity detection** (max sample-to-sample delta across the
   whole output, self-calibrated against a no-swap baseline run of the
   identical signal through the identical pipeline, rather than an
   arbitrary fixed threshold — a real dropout produces a delta many times
   larger than whatever the pipeline already naturally produces), and
   **no unexplained silence** (a long near-zero run *after* the
   already-understood startup-latency window is itself a finding).

**What it actually found, in one pass, before this doc's numbers were
real:** the first version of the swap mechanism (an instant engine
replacement) failed the discontinuity check — max delta 0.340 against a
0.014 natural baseline, ~24x. Root cause: each engine keeps independent
internal phase-vocoder state (windowing FIFO, phase accumulator,
overlap-add buffer); an instant swap discards the old engine's warmed-up
state and starts the new one cold. Fixed with a crossfade in
`CorrectorWorker` — both engines process the same input in parallel for a
short window immediately after a swap, blended together, before fully
switching over. First attempt (crossfade over 1 block) reduced the delta
to 0.201 — better, still failing. Root cause of the remaining gap: a
plain amplitude blend doesn't account for the two engines' phase-vocoder
reconstructions potentially being out of phase with each other, so even a
smooth amplitude ramp can't fully suppress a local derivative spike where
the two signals fight. Extending the crossfade to 4 blocks (~185ms at
44.1kHz) — spreading the same blend over more samples reduces the
worst-case per-sample slope proportionally — closed the gap: 0.051 against
the same 0.014 baseline, comfortably under threshold. All three checks
pass; the fix is real, not a loosened test (`docs/FINDINGS.md`).

This is arguably the best demo material in the whole testing story: an
automated test caught a real, audible-scale bug that would have been easy
to miss by ear in a quick check, the fix was iterated against real
numbers rather than guessed at, and the same test now guards against a
regression permanently.

**What the automated test alone didn't catch, and what a listening test
found instead:** once the 4-block crossfade passed this suite (0.051
max delta, comfortably under the self-calibrated threshold), a manual
listening test in Ableton still heard a quiet click. This is the honest
punchline for the demo, not an embarrassing gap to gloss over: an
automated click-detector threshold and genuine perceptual inaudibility
turned out not to be the same bar. Switching the blend from linear to
equal-power (cos/sin gains — the standard fix for the loudness dip linear
amplitude-crossfading causes) measurably improved it again
(user-confirmed "better, but still there"), even though the automated
metric itself rose slightly (0.051→0.078, still passing) — the
click-detector number and perceived smoothness aren't the same
quantity, worth remembering when trusting either one alone. Root cause
of the remaining residual, understood rather than chased further: the
two engines' phase accumulators evolve completely independently with no
shared reference point, which amplitude-blending — linear or equal-power
— can improve but can't fully resolve. See `docs/FINDINGS.md` #14 for
the full iteration history and numbers.
