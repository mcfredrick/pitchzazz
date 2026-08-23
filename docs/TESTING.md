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

## Objective quality metrics — putting a number on "does this sound clean"

Everything above turns timing/energy/dropout questions into numbers.
Spectral *cleanliness* — is a shifted tone's output actually a clean
harmonic reconstruction, or does it leak energy where none should be —
was still assessed only by ear, or documented as a known-but-unmeasured
limitation (`cpp-plugin/Source/DSP/VarispeedResampler.h`'s own class doc:
"an extreme up-shift could alias harshly... not tuned against by ear yet,
flagged here rather than assumed"). `cpp-plugin/Source/DSP/QualityMetrics.{h,cpp}`
closes that gap with two numbers, computed from one windowed FFT of a
steady-state segment of a shifter's output against a *known* target
fundamental:

- **THD+N%** — the standard AES17-style ratio: everything that isn't the
  fundamental, relative to the fundamental.
- **Artifact energy%** — energy that isn't explained by *any* harmonic of
  the target fundamental, relative to total energy. This is the metric
  that actually answers the anti-aliasing question this feature grew out
  of: a cleanly shifted tone has real energy at its harmonics (so THD+N
  alone doesn't distinguish "shifted cleanly" from "shifted with
  artifacts"), but a clean shift shouldn't have energy *outside* that
  harmonic comb — exactly what resampling aliasing or grain-boundary
  discontinuities produce and clean frequency-domain bin-relocation or
  time-domain grain-repositioning don't.

Same technique as every test above: synthetic known tone (a stationary
220Hz sine, chosen so the spectrum is unambiguous — see the caveat below)
through the real shifter classes at a swept range of semitone shifts,
asserting numeric thresholds set from a real measurement run
(`cpp-plugin/benchmarks/QualityMetricsProbe.cpp`), not guessed in
advance. Drives `PitchShifter`/`PSOLAPitchShifter`/`VarispeedShifter`
directly (each takes an explicit, controllable `semitoneShift`, same as
every existing shifter-level test in this codebase) rather than the full
`PitchEngine::process()` pipeline — `RustCorrectorEngine` has no raw-shift
entry point across its FFI boundary (only the full detect+quantize+shift
`process()`), so it's out of this particular sweep; it shares its actual
DSP with the native phase-vocoder engine (ported, not reimplemented —
`docs/COMPARISON.md`), so the native engine's numbers below are the
representative ones for that mechanism.

**Real measured results** (220Hz test tone, 44.1kHz, `QualityMetricsProbe`):

| Engine | Shift | THD+N% | Artifact energy% |
|---|---|---|---|
| Phase vocoder | -12 to +12 | 0.30 – 0.82 | 0.30 – 0.82 |
| PSOLA | 0 | 0.56 | 0.56 |
| PSOLA | -12 | *(undefined — see below)* | 0.57 |
| PSOLA | +12 | 775.86 | **99.18** |
| Varispeed | 0 | 22.96 | 22.34 |
| Varispeed | -12 / +12 | 24.61 / 30.01 | 23.57 / 28.74 |

Three real findings, not just a table:

1. **The phase vocoder stays clean across the entire ±12 semitone range**
   (under ~1% on both metrics). Matches the theory: `PitchShifter.cpp`'s
   bin-relocation loop explicitly drops any bin that would land above
   Nyquist (`if (index < halfFrameSize)`) rather than wrapping/aliasing
   it — this is the anti-aliasing mechanism, built into the algorithm
   itself rather than a separate filter stage.
2. **PSOLA is clean at unison but breaks down sharply and asymmetrically
   at the extremes.** At +12 semitones, 99.18% of the output's energy
   sits outside the target fundamental's harmonic comb — a real, severe
   artifact, consistent with the grain-boundary limitation
   `PSOLAPitchShifter.h`'s own class doc already documents
   (`docs/FINDINGS.md` #19/#20) but never previously quantified. At -12,
   something different and genuinely interesting happens: THD+N's ratio
   becomes undefined (`QualityMetrics::Result::thdPlusNValid == false`)
   because almost none of the output's energy survives at the true
   110Hz fundamental itself — it's nearly all on the upper harmonics
   instead — but artifact energy stays low (0.57%), meaning that energy
   is still cleanly harmonic, just redistributed, not turned to noise.
   A metric that silently reported a huge or NaN THD+N% here instead of
   flagging it would have been actively misleading — worth noting for
   anyone extending this class.
3. **Varispeed carries real, substantial artifact energy even at
   unison** (~22%), confirming with an actual number what its own source
   comment already predicted qualitatively — its cubic-Hermite resampler
   has no anti-aliasing pre-filter by design (see that class's own doc
   for why: physical tape/vinyl varispeed has none either, and fighting
   that is fighting the effect the engine exists to produce). Rises
   further at extreme upward shifts (~29% at +12). **Methodology
   caveat, stated plainly rather than glossed over:** the probe drives
   `VarispeedShifter` in fixed 2048-sample blocks, the same way a real
   caller does — but `VarispeedShifter::shiftPitch`'s own anticipatory
   click-suppression gain ramp (fading toward silence when the pipeline
   temporarily can't fill a whole block) runs on every call, not just at
   startup, so some of this baseline plausibly reflects that ramp rather
   than pure interpolation error alone. The two sources weren't
   separated in this pass — what's measured is the total a real caller
   would actually experience, which is the number that matters for an
   engine-quality comparison even without that separation.

**What this deliberately does *not* attempt:** discriminating the PSOLA
crackle/beat artifact from `docs/FINDINGS.md` #19/#20. Worth being
precise about first: **that artifact isn't aliasing.** PSOLA never
resamples — `placeGrainAt` repositions unmodified, already-sampled
grains rather than recomputing values at a fractional/resampled
position — so there's no decimation event for high-frequency content to
fold back through. The real mechanism is a grain-boundary discontinuity
(the read position snaps to the nearest fixed analysis bucket, so
crossing a boundary jumps to a slightly different, not-quite-identical
grain — real voice drifts cycle-to-cycle) plus a beat between that fixed
grid and the signal's natural jitter. The diagnostic that confirms this
rather than just asserting it: aliasing is a linear, frequency-dependent
phenomenon that doesn't care whether the source is stationary, but this
artifact requires non-stationarity to appear at all — `psola.rs`'s own
test module doc already establishes that a stationary sine cannot expose
it, since every analysis bucket then contains identical content
regardless of the boundary. That non-stationarity dependence is the
signature of a discontinuity/beat mechanism, not spectral fold-back.

That distinction is also *why* this metric can't score it: it only
exists on non-stationary content, and a tremolo-modulated tone's own
genuine amplitude-modulation sidebands aren't distinguishable from
"artifact energy" by a fundamental-harmonic-comb metric like this one
without separately modeling the modulation itself. Three different
automated approaches already failed to discriminate that specific
artifact; running a fourth, structurally-mismatched attempt just to have
tried again would have produced a number without a valid interpretation.
This metric answers a different, narrower, well-posed question — spectral
purity of a *clean, stationary* shift — and the crackle question stays
exactly as documented in Findings #19/#20: real, understood, and not yet
solved.

## From measurement to fix — PSOLA's +12 semitone breakdown

The table above wasn't just descriptive: tracing *why* PSOLA's artifact
energy spikes specifically at +12 (not +3, not any downward shift)
pointed straight at a fixable mechanism, not an inherent limitation.
Grain width is fixed to the original period regardless of shift, but
synthesis spacing shrinks as shift ratio grows — so grain overlap grows
as `2 * grainWidthMultiplier * shiftRatio` (4x the unison baseline at
+12 semitones), producing comb-filtering from summing many time-shifted
copies of the same grain content.

`chooseGrainWidthMultiplierForShift()` (`PSOLAPitchShifter.h`/`.cpp`)
solves that equation for the multiplier that holds overlap at the known-
clean unison baseline: `1/shiftRatio`, clamped to the existing
`grainWidthMultiplierMin` floor. Real before/after measurements
(`tests/DSP/QualityMetricsTests.cpp`, `[grain-width-fix]`):

| Shift | Before (1.0x) | After (formula) |
|---|---|---|
| +3 st | 1.20% | 1.29% (very slightly worse) |
| +6 st | 1.91% | 1.85% (flat) |
| +9 st | 3.58% | 2.11% (real improvement) |
| +12 st | **99.18%** | **2.20%** (~45x) |

Reported honestly rather than smoothed over: the fix is a large,
decisive win exactly where the actual problem is (severe at +9,
catastrophic at +12), and a wash-to-negligible-regression at small
shifts where there was no real problem — narrowing the grain trades away
a little within-grain frequency resolution, and at small shifts nothing
offsets that cost. The data matches the mechanism's own prediction
closely, including where it predicts *no* benefit.

**Deliberately not wired into production** (`PSOLACorrector::process()`
or the GUI) in this pass: `grainWidthMultiplier` is also a user-facing
"Width" slider (`docs/ROADMAP.md` Phase 5), and silently auto-overriding
it every block would change already-shipped behavior — the auto/manual
interaction model that would need is a separate, deliberate decision, not
assumed here. This exists as a tested, measured, callable function.
`docs/FINDINGS.md` #26 has the full account.
