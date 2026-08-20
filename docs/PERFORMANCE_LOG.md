# Performance log

A dated, chronological record of profiling findings and the changes made in
response — kept deliberately separate from `docs/ARCHITECTURE.md` (which
holds the current-state design rationale) because the *investigation
process* — hypothesis, measurement, fix, re-measurement — is itself the
most persuasive material this project has for a technical deep-dive. Don't
retroactively clean this up into just the final state; the value is in
showing the reasoning trail.

**Maintenance rule** (also in `CLAUDE.md`): any time you profile something,
change a performance-relevant parameter, or investigate a latency/CPU-cost
question in this codebase, add a dated entry here — data first, then what
you concluded from it, then what you changed. Entries are append-only;
don't edit old entries to match new findings, add a new entry that
supersedes them and say so.

---

## 2026-08-17 — Initial live smoke test surfaces a real-time budget miss

Context: `docs/ARCHITECTURE.md` covers the concurrency-correctness fixes
made getting here (lock-free ring buffers replacing a mutex-guarded buffer
and a blocking mpsc channel, both formerly called from the real-time audio
callbacks). This entry starts once that architecture was in place and the
CLI was run live against a real mic for the first time.

**Setup:** MacBook Pro built-in mic, cpal default config (96kHz, mono),
`BLOCK_SIZE = 2048` samples, `PitchCorrector` window 50ms, `OVER_SAMPLING =
32` (unchanged from the original prototype — never independently measured
there either).

**Finding:** 2048 samples ÷ 96000 Hz = 21.3ms of audio per block. Measured
`PitchCorrector::process()` time: 28–35ms typically, one spike to ~79ms.
Processing is structurally slower than real-time by roughly 40–60% in
steady state. This is a different failure mode than the concurrency bugs
in `ARCHITECTURE.md` — the lock-free ring buffers correctly prevent the
callback from ever blocking on contention, but if the worker thread can't
produce corrected audio as fast as it arrives, the output ring buffer
still drains empty (starvation), audible as the same kind of dropout.

**Action:** instrumented `PitchCorrector::process` to time each stage
separately (`StageTimings { detect_us, quantize_us, shift_us }` in
`pitch-core/src/corrector.rs`) instead of one combined number, so the next
measurement would show *where* the time goes rather than just *that* it's
too slow.

## 2026-08-17 — Per-stage breakdown: it's entirely the shift stage

**Data** (same setup as above, `--verbose`):

| stage | typical cost |
|---|---|
| detect (McLeod) | ~2000–2900us with a pitch present, ~25–80us on silence |
| quantize (scale snap) | ~5–14us — negligible |
| shift (phase vocoder) | ~28,000–35,000us — **on every block, including silent ones** |

The shift stage alone exceeds the entire 21,333us budget by itself,
regardless of input content — a fixed cost, not a content-dependent one.
That's the tell that this is a parameter problem (something scaling the
shifter's fixed per-call work), not an algorithmic edge case.

**Conclusion:** `OVER_SAMPLING = 32` (passed to `shift_pitch`) was the
prime suspect — carried over from the original prototype, never measured
independently there either. Not sample rate: detection is cheap regardless
of the 96kHz rate, and the cost pattern (shift dominates, constant
regardless of content) doesn't match "too many samples to analyze," it
matches "doing 32x more transform work than needed per call."

**Action:** dropped `OVER_SAMPLING` from 32 to 8 as a first measurement,
not a final answer.

## 2026-08-17 — Oversampling 32 -> 8: ~4x reduction, most blocks now clear budget

**Data:** shift stage dropped to typically 6,500–11,300us (vs. 28,000–
35,000us before — matches the 4x reduction from 32→8 almost exactly,
consistent with the oversampling factor directly multiplying transform
work). Most blocks' total time (6,600–11,900us) now sits comfortably under
the 21,333us budget with real margin.

**Not fully closed out:** two blocks in the same run spiked to ~32ms and
~36ms, with *both* detection (normally ~2ms, spiked to ~13-14ms) and shift
elevated together. Detection's cost is otherwise rock-steady, so a
simultaneous jump in an unrelated stage smells like OS scheduling jitter
on this shared dev machine (something else got CPU time, the worker thread
got descheduled) rather than an algorithmic tail case — but that's a
hypothesis, not yet verified. Real-time systems care about worst-case
(P99/P100) far more than typical-case, so this isn't "done," it's "median
case now has margin, tail latency is an open question."

**Next step, not yet done:** re-run for a longer window and log a
histogram, not just eyeball a few dozen lines, to see whether those spikes
are rare outliers or a recurring pattern with some periodicity (which
would point at something systematic — e.g. GC-like behavior from an
allocation somewhere on the worker thread, or a lower-priority thread
getting starved — rather than one-off jitter).

## 2026-08-17 — SOTA comparison: what does "real-time" mean for this problem, actually

Before chasing lower numbers further: researched what commercial pitch-
correction products and the DSP literature consider acceptable, so effort
here targets a real bar instead of an arbitrary one. Timeline is one week;
not worth re-deriving latency engineering results that already exist in
the industry.

**Findings** (see sources; treat vendor-quoted numbers as directional, not
lab-verified — they're marketing copy, not published test methodology):

- Commercial products built specifically for **live, low-latency** use
  (Antares Auto-Tune Live/Artist/Realtime X, similar hardware-oriented
  tools) target **sub-10ms**, with cited figures like 2.5ms in an explicit
  "low latency mode." The same vendor's general-purpose plugin modes
  (not optimized for live use) are cited around **58ms** — a ~23x spread
  between "optimized for live" and "optimized for quality," from the same
  company, which is itself the key data point: this is a real, acknowledged
  engineering tradeoff in the industry, not something this project is
  failing to solve correctly.
- The literature is unambiguous about *why*: phase-vocoder / FFT-based
  pitch shifting (what this project's `pitch_shift` crate implements —
  confirmed by reading its source, not assumed: `frame_size = sample_rate *
  window_duration_ms / 1000`, real FFT overlap-add) gives better quality,
  especially on complex/polyphonic input, at the cost of latency roughly
  proportional to the analysis window. Time-domain approaches (TD-PSOLA)
  are simpler and much lower-latency by working directly on identified
  pitch periods instead of transforming to the frequency domain, at some
  quality cost. This is almost certainly the actual algorithmic reason
  Auto-Tune's low-latency mode and general mode differ so much — different
  underlying method, not just different tuning of the same one.
- Human perceptibility: latency becomes noticeable for self-monitoring
  around **10–12ms**, starts affecting a performer's sense of timing around
  **5–10ms**, and singers are unusually sensitive to it specifically
  because bone-conducted self-hearing is near-zero-latency, so any added
  system delay creates audible comb-filtering against that reference.

**What this means for this project's latency, not just its per-block CPU
cost:** the 6,500–11,300us *processing time* measured above is not the same
number as end-to-end *pipeline latency*. The phase vocoder's window alone
(`window_size_ms = 50`) is close to a fixed ~50ms algorithmic delay by
construction — and because it's expressed as a time duration, not a sample
count, **lowering the sample rate reduces CPU cost per block but does not
reduce this latency floor.** Add the current block-accumulation delay
(`BLOCK_SIZE`/sample_rate ≈ 21ms) and the true mic-to-speaker latency is
almost certainly in the neighborhood of 70–100ms+, not the ~7–35ms this
log has been discussing — those numbers are the worker thread's CPU
budget, a real and necessary thing to fix, but a different question from
total system latency.

**Where that puts this project, honestly:** in the same territory as
"general-purpose, not latency-optimized" commercial plugin modes, not
anywhere near the specialized sub-10ms live tier — and closing that gap
would mean swapping the shifting algorithm family (phase vocoder → TD-PSOLA
or similar), not tuning parameters. That's a legitimate, well-scoped Phase
2+ idea (see `docs/ROADMAP.md`) but explicitly **not** an MVP goal — per
project direction, this timeline should benchmark honestly against real
competitors and explain *where* the latency comes from, not chase a number
past what the best available products themselves achieve.

**Not yet done:** an actual measured end-to-end latency number (e.g. an
impulse/loopback test — feed a click in, measure sample-accurate offset to
when it appears corrected at the output) to replace the analytical ~70-
100ms estimate above with a real one. That's the next concrete task before
this can be stated as a number in the presentation rather than an estimate.

### Sources

- [Improve live vocals with Antares' Auto-Tune Artist | MusicTech](https://musictech.com/news/gear/antares-auto-tune-artist-live-vocal-processing-plug-in/)
- [Antares Auto-Tune Realtime X Plug-In Primer – Universal Audio Support](https://help.uaudio.com/hc/en-us/articles/115002843943-Antares-Auto-Tune-Realtime-X-Plug-In-Primer)
- [lowest latency autotune (or autotune like) fx - KVR Audio Forum](https://www.kvraudio.com/forum/viewtopic.php?t=535495)
- [(PDF) Low latency audio pitch shifting in the time domain](https://www.academia.edu/27298856/Low_latency_audio_pitch_shifting_in_the_time_domain)
- [(PDF) New phase-vocoder techniques for pitch-shifting, harmonizing and other exotic effects](https://www.academia.edu/73897779/New_phase_vocoder_techniques_for_pitch_shifting_harmonizing_and_other_exotic_effects)
- [Phase Vocoder Implementation with FLWT and TD-PSOLA — Stanford EE264](https://web.stanford.edu/class/ee264/projects/EE264_w2015_final_project_kong.pdf)
- [A Matter Of Timing: Clarifying Latency And Putting It Into Context - ProSoundWeb](https://www.prosoundweb.com/a-matter-of-timing-clarifying-latency-and-putting-it-into-context/)
- [How digital audio latency can affect musicians and performers - Church Production Magazine](https://www.churchproduction.com/education/latency-and-its-affect-on-performers/)

---

## 2026-08-17 — Rust-vs-C++ per-block cost, controlled for build profile

Context: the C++ port's `Corrector` (`cpp-plugin/Source/DSP/`, ported this
session — see `docs/FINDINGS.md`) needed a real per-block cost number for
the Phase 3 comparison. First attempt (`cpp-plugin/build/Release/Benchmarks`,
Catch2 benchmark, block 2048): ~370us mean across 44.1/48/96kHz. Compared
naively against this log's earlier Rust numbers (6,600-11,900us typical,
post the 32→8 oversampling fix), that looked like a 20-30x C++ advantage —
**wrong comparison, caught before writing it up**: every number in this
log so far was gathered via `cargo run` (the root `CLAUDE.md`-documented
command), which builds the **unoptimized debug profile**. Comparing a
Rust debug build against a C++ Release build measures the compilers'
optimization settings, not the algorithms or the languages.

**Fix:** wrote `crates/pitch-core/examples/bench_corrector.rs`, mirroring
`cpp-plugin/benchmarks/CorrectorPerformance.cpp` exactly — same block size
(2048), same window (50ms), same synthetic 220Hz test tone (not silence —
silence hits the power-threshold early exit and never runs real DSP), same
three sample rates, same C major scale, same 100-iteration mean after a
5-iteration warm-up. Run with `cargo run --release --example
bench_corrector -p pitch-core`, not plain `cargo run`.

**Controlled data** (block 2048, mean of 100 iterations each, both engines
in an optimized build):

| Sample rate | Rust (release) | C++ (Release) | Budget | Rust % of budget | C++ % of budget |
|---|---|---|---|---|---|
| 44100Hz | 461.5us | 371.4us | 46,439.9us | 0.99% | 0.80% |
| 48000Hz | 349.1us | 368.3us | 42,666.7us | 0.82% | 0.86% |
| 96000Hz | 307.0us | 375.6us | 21,333.3us | 1.44% | 1.76% |

**Interpretation:** no consistent winner — C++ is faster at 44100Hz, Rust
is faster at 96000Hz, they're within noise of each other at 48000Hz. Both
engines are trivially under budget (under 2% utilization at every rate
tested) once properly optimized. The honest story for the presentation
isn't "one language wins," it's that a naive, profile-mismatched
comparison would have overstated the gap by ~20-30x, and the real,
controlled difference between the two engines is implementation-detail
noise (most plausibly JUCE's Apple-optimized vDSP FFT backend vs.
`rustfft`'s portable implementation — see `docs/ARCHITECTURE.md`'s Apple
FFT engine notes — not evidence either language is fundamentally faster
for this workload).

**Implication for earlier entries in this log, not yet acted on:** this
also means the 32→8 `OVER_SAMPLING` investigation above was conducted
entirely in a debug build. The debug-profile "most blocks now clear
budget with real margin" conclusion still held at the time (debug numbers
vs. debug budget, an internally consistent comparison), but a release
build finishes the *entire* corrector in less time than the debug build's
shift stage alone took post-fix (6,500-11,300us) — meaning `OVER_SAMPLING
= 8` was chosen against debug-build costs and hasn't been re-evaluated
against release-build costs. Worth revisiting whether a higher oversampling
value (better pitch-shift quality) is affordable in a release build before
calling that parameter settled — **not done in this entry**, flagged as a
follow-up, not a re-measurement, to keep this entry's data honest about
what it does and doesn't cover.

**Not covered by this entry:** the two-outlier tail-latency question from
the oversampling entries above (still open — this entry's 100-iteration
runs are too short in wall-clock time to observe rare scheduling-jitter
spikes) and true end-to-end pipeline latency (still an analytical
estimate, not measured, for both engines).

---

## 2026-08-17 — Measured pipeline latency, both engines: real numbers, not an estimate

Context: Phase 1's original "immediate next task" (`docs/ROADMAP.md`) —
replace the analytical ~70-100ms latency estimate above with an actual
measurement. Deferred by the JD-driven reprioritization to the C++ port,
picked back up once the sprint's core scope was done and there was runway
left.

**Method:** an impulse-response probe, one per engine
(`crates/pitch-cli/examples/latency_probe.rs`,
`cpp-plugin/benchmarks/LatencyProbe.cpp`), mirroring each other exactly —
same block size (2048), same three sample rates, same technique: feed a
single-sample impulse through the *same* ring-buffer/FIFO +
worker-pipeline shape the real system uses (not calling
`process()`/`Corrector::process` directly — this needs to capture
whatever the block-accumulation architecture contributes, not just the
DSP's own cost), then find the peak of the output response. For an
impulse probe, this is the standard "impulse-response-peak as a
group-delay proxy" technique — cross-correlating anything against a
literal delta function reduces to finding the response's peak, so this
isn't a shortcut, it's the textbook method evaluated at its simplest input.

Both probes feed the entire signal into the ring buffer/FIFO at once
rather than pacing it in real time — deliberately, not as an oversight;
see the "what this does and doesn't capture" section below for why that
turned out not to matter here.

**Data** (impulse response peak position, in samples and ms):

| Sample rate | Rust: samples (ms) | C++: samples (ms) |
|---|---|---|
| 44100Hz | 2206 (50.0ms) | 2048 (46.4ms) |
| 48000Hz | 2400 (50.0ms) | 2048 (42.7ms) |
| 96000Hz | 4800 (50.0ms) | 4096 (42.7ms) |

**Finding: the measured latency exactly equals the phase vocoder's own
frame size, in samples, at every sample rate, for both engines —
independent of `BLOCK_SIZE` (2048, fixed).** Confirmed unambiguously at
96kHz, where the two numbers can't be confused: C++'s frame size at 96kHz
is 4096 (`cpp-plugin/Source/DSP/PitchShifter.cpp`'s nearest-power-of-two
rounding), and the measured latency is exactly 4096 samples — not 2048
(`BLOCK_SIZE`), not 2048+4096. Rust's frame size is
`sample_rate * 50ms`, rounded to even, which is why its latency lands at
exactly 50.0ms at every rate (the frame-size/sample-rate ratio is
constant by construction) while C++'s lands at 42.7-46.4ms — a direct,
now-measured consequence of the power-of-two frame-size rounding already
documented as a design divergence in `docs/COMPARISON.md` (chosen
specifically to avoid inflating latency versus rounding up) — that
divergence isn't just a documented implementation detail anymore, it's a
measured ~4-8ms latency difference between the two engines.

**Why `BLOCK_SIZE` doesn't add on top, and why that's not a measurement
artifact:** the ring-buffer/FIFO worker only calls
`process()`/`Corrector::process` once it has a full `BLOCK_SIZE` (2048)
of input — but the phase vocoder's *own* internal FIFO/window state is
continuous across calls, not reset per call, and its own analysis window
(the frame size — 2206-4800 samples for Rust, 2048-4096 for C++,
depending on rate) is, in every case tested, **greater than or equal to**
`BLOCK_SIZE`. Whichever lookahead requirement is larger wins: since the
phase vocoder always needs at least as much lookahead as one `BLOCK_SIZE`
chunk provides, waiting for a `BLOCK_SIZE` chunk to accumulate never adds
delay beyond what the phase vocoder's own window already requires. This
holds for a live, real-time-paced input too, not just this offline
batch-fed probe: a live system fundamentally cannot produce output for a
given input sample faster than its analysis window's worth of *future*
samples have physically arrived, and that lower bound is exactly the
frame size — which is what got measured. If `BLOCK_SIZE` were ever tuned
larger than the phase vocoder's frame size, this would no longer hold and
`BLOCK_SIZE` would start dominating instead; worth re-checking this
reasoning if that ratio ever changes.

**What this measurement does and doesn't include:** this is the
pipeline's own algorithmic/software latency — ring buffer or FIFO,
worker-thread accumulation, and the phase vocoder's analysis window. It
does **not** include audio-hardware I/O latency (ADC input buffering, DAC
output buffering), which is real, additive, and depends on the audio
interface's own buffer-size configuration, not on this codebase — that
remains a genuinely separate, unmeasured contributor for both engines.

**Revising the earlier estimate:** the ~70-100ms analytical estimate in
the SOTA-comparison entry above assumed block-accumulation delay
(`BLOCK_SIZE`/sample rate, ~21-46ms) and the phase vocoder's window
(~50ms) stack additively. They don't — this entry's data shows the larger
of the two wins, not their sum. The real measured software latency
(42.7-50.0ms depending on engine and rate) is close to *half* what the
conservative estimate assumed. That earlier estimate wasn't wrong to be
conservative given it was never measured, but the mechanism it assumed
(additive stacking) was incorrect — worth stating plainly rather than
quietly revising the number without explaining why it moved.

**Not yet done:** pacing the probe's input in true real time (rather than
batch-feeding the whole signal at once) as a direct empirical check on
the "block-accumulation doesn't add delay" reasoning above, rather than
relying on the argument for why it shouldn't. The reasoning is solid, but
this codebase's own standard is measurement over argument where
practical — flagged as a follow-up, not treated as equivalent to having
already done it. Also not done: real audio-hardware loopback measurement
(would need a physical loopback cable or interface, not available in this
environment) to get the true end-to-end number including hardware I/O.

---

## 2026-08-17 — OVER_SAMPLING re-evaluated against release-build costs

Context: flagged as a follow-up in this log's "Rust-vs-C++ per-block cost"
entry above — `OVER_SAMPLING = 8` was chosen against Rust *debug*-build
costs (the 32→8 entry near the top of this log), never re-checked against
release-build costs once those turned out to be far cheaper than assumed.

**What `OVER_SAMPLING` actually does**, for context on why this needed
checking rather than assuming: it sets the phase vocoder's STFT hop size
(`step = frameSize / overSampling`), not the analysis window size
(`frameSize`) itself — a smaller hop means more overlapping analysis
frames, which gives the algorithm's instantaneous-frequency estimation
(the `deltaPhase`/`expected` phase-tracking math in both engines'
`PitchShifter`) more, closer-spaced phase samples to work from, generally
reducing phase-vocoder-typical artifacts ("phasiness," transient
smearing). That's the standard DSP-literature explanation for the
parameter's effect on quality — **not independently verified by ear in
this session**; no audio-quality/listening test tooling exists in this
environment, so "higher oversampling sounds better" is asserted on
textbook grounds, not measured here. What *is* measured: cost and
latency.

**Method:** swept `overSampling` directly against `PitchShifter` (not the
full `Corrector` — only the shift stage is affected by this parameter) at
4/8/16/32/64, measuring both per-call cost and impulse-response latency
in the same run, at 44100Hz, in both engines
(`crates/pitch-core/examples/oversampling_sweep.rs`,
`cpp-plugin/benchmarks/OversamplingSweep.cpp`). Release builds for both —
the C++ side was first measured in Debug by mistake and re-run in Release
before writing this up, the same debug/release discipline established in
the entry above.

**Data** (mean cost per call, % of the 46,439.9us budget at 44100Hz,
block 2048):

| over_sampling | Rust cost | Rust % budget | C++ cost | C++ % budget | Latency (both engines) |
|---|---|---|---|---|---|
| 4 | 225.2us | 0.48% | 135.7us | 0.29% | unchanged |
| 8 (current) | 452.0us | 0.97% | 272.5us | 0.59% | unchanged |
| 16 | 898.7us | 1.94% | 574.3us | 1.24% | unchanged |
| 32 | 1981.6us | 4.27% | 1205.8us | 2.60% | unchanged |
| 64 | 4543.1us | 9.78% | 2512.2us | 5.41% | unchanged |

"Latency unchanged" is exact, not approximate: every single run at every
`overSampling` value landed on precisely the same impulse-response peak
sample index as at `overSampling = 8` (2206 samples / 50.0ms for Rust,
2048 samples / 46.4ms for C++), for the reason in the "What
`OVER_SAMPLING` actually does" paragraph above — the analysis window size
is what determines latency, and this parameter doesn't touch it.

**Also observed, not the point of this entry but worth noting honestly:**
C++ is consistently faster than Rust at every value in this isolated
shift-only sweep (~1.6-1.8x), unlike the full-`Corrector` comparison
above where neither engine consistently won. Plausible explanation:
JUCE's Apple-optimized vDSP FFT backend vs. `rustfft`'s portable one,
consistent with this log's and `docs/COMPARISON.md`'s existing
"implementation detail, not language" framing — not independently
confirmed by further profiling here, flagged as an observation rather
than a new conclusion.

**Decision: raise `OVER_SAMPLING` from 8 to 16.** Reasoning: cost scales
~linearly and stays cheap at every value tested (even 64 is under 10% of
budget on the slower engine), and latency is provably unaffected — so the
real constraint on how high to go isn't headroom, it's diminishing
returns on quality past a certain point, which this session has no way to
measure directly. Doubling to 16 is a meaningful, clearly-justified
improvement (more overlap than before, room to spare — 1.24-1.94% of
budget) without picking an arbitrary "why not 64" number that this data
can't actually justify over 32 or 16 on quality grounds alone. The
correct way to pick a final value is a listening test, not a headroom
calculation — this is a conservative, defensible interim value, not a
claim that 16 is optimal.

**Verification:** both engines rebuilt with the new constant; `cargo
test` (6/6), `cargo clippy` (clean), and the C++ `Tests` target (12/12,
56 assertions) all still pass — this parameter doesn't change either
engine's correctness-test behavior, only shift quality/cost, so a clean
pass here confirms nothing broke, not that quality improved (that still
needs the listening test noted above).

---

## 2026-08-17 — Correction: Auto-Tune's low-latency mode isn't an algorithm swap

The SOTA-comparison entry above asserted "this is almost certainly the
actual algorithmic reason Auto-Tune's low-latency mode and general mode
differ so much — different underlying method, not just different tuning
of the same one." Re-checked while discussing whether this project's
hot-swap feature should generalize beyond Rust-vs-C++ — that claim
doesn't hold up. Auto-Tune Pro's Low Latency mode is a toggle *within the
same plugin*, switchable instantly, and by Antares' own description
shares the same core pitch-correction algorithm as the full-quality mode;
the one genuinely separate product, AutoTune Hybrid, gets to zero latency
via dedicated Avid HDX/Carbon hardware DSP offload, not a different
software algorithm. Sources: [AutoTune 2026 for Live & Studio](https://www.antarestech.com/products/pitch-correction/at2026),
[AutoTune Hybrid](https://www.antarestech.com/products/pitch-correction/hybrid).
Same caveat as the original entry: these are vendor-facing product pages,
not published algorithmic documentation, so treat as directional, not
verified implementation detail.

Not edited into the original entry — this supersedes it, per this log's
own append-only rule. The TD-PSOLA-vs-phase-vocoder algorithm-family
question from that entry is still a real, valid lever in general (it's
literature-supported, not vendor-marketing-supported), just not
confirmed to be *the* mechanism Auto-Tune specifically uses for its live
mode. More likely candidate, given it's a same-algorithm toggle: a
settings/window-size tradeoff similar in kind to this project's own
"content-aware window size" idea (`docs/ROADMAP.md` Phase 5) — not
confirmed, but a more consistent explanation for a same-plugin, no-restart
toggle than swapping the underlying method entirely.

---

## 2026-08-17 — TD-PSOLA engine: measured latency vs. the phase vocoder

Built a third `PitchEngine` (`PSOLACorrectorEngine`, id `psola-cpp`) — the
algorithm-family swap flagged as a lever in the SOTA-comparison entry
above, finally acted on rather than just noted. Time-domain pitch-
synchronous overlap-add: pulls a ~2-period Hann-windowed grain at each of
a series of pitch marks and overlap-adds it back at a *different*
spacing (closer for pitch up, farther for pitch down) — grain spacing
carries perceived pitch, grain width (fixed at the original period)
preserves the spectral envelope. C++ only, not ported to Rust (per
project direction — the Rust/C++ parity story is already proven by the
existing two engines, a third algorithm doesn't need to re-prove it).

**Latency is pitch-dependent by construction, unlike the phase vocoder's
fixed window** — the existing `LatencyProbe.cpp` (impulse-peak) doesn't
work here at all: an impulse has no meaningful detected period, so it
wouldn't exercise this engine's actual mark-firing behavior. Built a
dedicated probe (`benchmarks/PSOLALatencyProbe.cpp`, onset-detection
based) instead. `getLatencySamples()` itself is a **fixed** worst-case
number (2 pitch periods at a 60Hz floor — see `PSOLAPitchShifter.h`'s doc
for why fixed, not adaptive: a host needs one number, not "it varies"),
sized once and used regardless of what's actually playing.

**Measured** (matches the derivation exactly, confirmed after fixing a
real bug along the way — see below and `docs/FINDINGS.md` #18):

| Sample rate | PSOLA (worst-case, measured) | Phase vocoder (measured, `PERFORMANCE_LOG`'s earlier entry) | Reduction |
|---|---|---|---|
| 44.1kHz | 1470 samples / 33.3ms | 2048 samples / 46.4ms | ~28% |
| 48kHz | 1600 samples / 33.3ms | 2048 samples / 42.7ms | ~22% |

Notable: PSOLA's latency is **constant in milliseconds across sample
rates** (33.3ms at both 44.1kHz and 48kHz) — it's a period-*count* bound
(2 periods at a fixed-Hz floor), not a sample-*count* bound, so it scales
with sample rate exactly enough to cancel out. The phase vocoder's
sample-count-based window doesn't have this property (its ms figure
varies by rate, per the nearest-power-of-two rounding entry above). A
real, honest win — meaningful, not dramatic: roughly a quarter less
latency, not the order-of-magnitude gap to genuine sub-10ms hardware-DSP
tiers (still the same different-league comparison the SOTA entry already
covers).

**The bug, briefly** (full story in `docs/FINDINGS.md` #18): the first
version of this measurement showed `48kHz/220Hz` overshooting the
derived latency bound by ~220 samples while three other tested
combinations matched almost exactly — initially misread as expected
overlap-add onset smoothing (a real phenomenon, just not what was
actually happening here). Increasing the safety margin (2x → 3x → 4x
`maxPeriodSamples`) left the ~220-sample gap completely unchanged in
size, which was the actual tell that it wasn't a margin problem. Direct
instrumentation (a temporary mark-firing trace, compiled and run
standalone) found the real cause: `placeGrainAt`'s `std::floor()`-based
read-position calculation, applied to a value that *should* land exactly
on an integer multiple of the period but occasionally sat a few ULPs
below it due to floating-point accumulation from repeated addition —
flooring `17.999999997` gives `17`, not `18`, silently reading an entire
extra period of stale content. A `+1e-6` epsilon before the `floor()`
fixed it; latency then matched the derived bound exactly across every
tested combination, no padding needed. Worth remembering for future
work in this class of algorithm: "increasing a safety margin doesn't
change the size of the discrepancy" is itself diagnostic — it means the
bug isn't in the margin.

---

## 2026-08-17 — TD-PSOLA revisited: tighter floor, then a real quality fix that cost latency back

Two changes to the entry above, in sequence, both user-directed. Superseding it per this log's own append-only rule, not editing it.

**1. Tightened `minHz` (60 -> 80).** The entry above used a 60Hz floor —
comfortably below a typical bass vocal (~80-150Hz), but more margin than
this project's actual use case (vocal pitch correction, not general-
purpose/instrument-agnostic shifting) needs. Since latency here is
`k * ceil(sampleRate / minHz)` — a period-*count* bound, constant in
milliseconds regardless of sample rate — raising the floor to 80Hz (still
below typical bass vocal range, just with less headroom) is a pure,
host-safe win: no runtime adaptivity, no risk to plugin-delay-compensation
correctness (see below for why that risk is real and why this project
isn't attempting it), just a smaller fixed number. At `k=2` (the
derivation before the next change): 33.3ms -> 25.0ms, a 25% reduction, confirmed exact via `benchmarks/PSOLALatencyProbe.cpp` at both 44.1kHz and 48kHz.

**Considered and rejected: fully adaptive (pitch-tracked) latency.**
Raised directly by the user: since the actual data needed for a high
detected pitch is ready well before the worst-case tap fires, why not
slide the read tap forward and use fresher audio when available? The DSP
math supports it, but a DAW host's plugin-delay-compensation (PDC) relies
on `setLatencySamples()` reporting one *constant* value and the plugin's
actual output delay matching it exactly, every block — if the real delay
varied with detected pitch, output would arrive at an inconsistent
effective offset relative to other tracks the host delayed to match the
worst case, producing real phase/timing smear when mixed, even though
each block's own processing is completely correct in isolation. Same
open risk `docs/ROADMAP.md`'s "dynamic/hybrid window sizing" idea already
flagged for the phase vocoder — not attempted here either, would need
dedicated host-compatibility research first.

**2. Cross-fade fix cost a period back (`k=2` -> `k=3`).** Separately,
real listening on live audio (not the synthetic test tones the unit
tests use) found a crackle/low-frequency-beat artifact — full diagnosis
in `docs/FINDINGS.md`. Root cause: `placeGrainAt` read content from a
single, floor-quantized analysis bucket, so source content jumped
discretely every time the synthesis position crossed a bucket boundary.
Fixed by cross-fading between the two nearest buckets instead, which
needs an extra period of lookahead for the farther one — `latencySamples`
became `3 * ceil(sampleRate / minHz)`, not 2.

**Net effect, measured exact via the same probe:**

| | 44.1kHz | 48kHz |
|---|---|---|
| Original (minHz=60, k=2) | 33.3ms | 33.3ms |
| Floor tightened only (minHz=80, k=2) | 25.0ms | 25.0ms |
| + cross-fade fix (minHz=80, k=3) | **37.55ms** | **37.5ms** |
| Phase vocoder (unchanged) | 46.4ms | 42.7ms |
| **Net reduction vs. phase vocoder** | **~19%** | **~12%** |

The quality fix cost back more than the floor-tightening gained — net
result is a smaller win over the phase vocoder than either the original
33.3ms number (~28%/~22%) or the floor-only number (~46%/~41%) implied,
but still a real, verified reduction, and the crackle fix isn't optional:
an engine that's faster but audibly broken isn't a real alternative.
Explicit trade-off, made with the data in front of us, not a regression
nobody noticed.

**Automated verification of the crackle fix itself: attempted, didn't
land.** Three approaches tried (full account in
`tests/DSP/PSOLAPitchShifterTests.cpp`'s own comments): a stationary-sine
test can't expose the bug at all (a perfectly periodic signal's analysis
buckets are identical whether cross-faded or not — the bug only bites
non-stationary content, which real voice always is and a pure test tone
never is); a tremolo-modulated signal compared against an externally-
synthesized reference tone was phase-misaligned and meaningless; the same
tremolo signal's own statistical outlier ratio (max delta vs. median
delta) measured *lower* for the deliberately-broken single-bucket version
than the fixed one on one real run — the opposite of discriminating.
Matches this project's own earlier precedent (`docs/FINDINGS.md` #14): an
automated metric passing and genuine perceptual quality are not always
the same bar. The fix shipped anyway, justified by the DSP reasoning
(a discrete source-content jump at a bucket boundary is a real,
identifiable defect on its own terms) — real-audio listening
confirmation from the user is still the open item, not an automated
test asserting a threshold that was measured not to actually
discriminate the regression.

---

## 2026-08-17 — TD-PSOLA cross-fade reverted: real listening said it didn't work

That "still open item" resolved fast, and not the way the DSP reasoning
predicted. Real listening on the rebuilt cross-fade version found the
crackle/beat artifact **still present, possibly worse** — the fix that
shipped on sound-sounding theory did not survive contact with actual
ears. Reverted rather than keep tuning blind.

**Best available diagnosis, not confirmed by measurement** (no way to
verify this by ear myself): comb filtering. The cross-fade blends two
waveform segments exactly one period apart, which is only a clean blend
if the signal is truly periodic at exactly that spacing. Real voice
isn't — `periodSamples` is a single per-block (~46ms) estimate, but
actual vocal-fold vibration has natural cycle-to-cycle jitter, so the
true period drifts slightly out of sync with that fixed estimate over
the course of a block. Linearly blending two similar-but-phase-shifted
waveforms doesn't sound like a smooth cross-fade — it produces frequency-
dependent cancellation, and as the misalignment changes over time the
cancelled frequencies sweep, which could easily read as a "beat." This
is a documented failure mode of naive PSOLA grain-averaging without
alignment, not a novel guess, but it's a hypothesis, not a verified root
cause — nothing here was measured to confirm it, only reasoned about.

**A high/low-pass filter was raised as a possible fix and rejected on
the same reasoning basis, not tested:** beating shows up as sidebands
around the pitch itself (a 220Hz tone beating at 5Hz puts energy at
215Hz/225Hz, not at a separate 5Hz component), and crackle is a broadband
transient — neither has a spectral signature a simple threshold filter
would cleanly separate from the desired signal. Worth adding a standard
DC-blocking high-pass (~20-30Hz) as general overlap-add hygiene
regardless, but not expected to address this artifact specifically.

**Decision, offered as an explicit choice and taken:** revert to
single-bucket (back to `latencySamples = 2 * maxPeriodSamples`, ~25ms —
see the two entries above for that number's own history), document the
crackle as a known, accepted limitation of this simplified PSOLA design,
and stop iterating on it for now. The real fix — correlation-based grain
alignment before blending, not blending alone — is bigger scope than this
project's timeline has room for. This engine's demonstrated value is the
latency comparison against the phase vocoder (docs/ROADMAP.md's original
framing for building it at all), not production-grade reconstruction
quality; the artifact is disclosed, not hidden, which is the more
important property for what this project is actually for.

**Numbers, back to where they were before the cross-fade attempt:** 25.0ms
at both 44.1kHz and 48kHz (2 periods at `minHz=80`, confirmed exact via
`benchmarks/PSOLALatencyProbe.cpp`), ~46% less than the phase vocoder's
46.4ms at 44.1kHz and ~41% less than its 42.7ms at 48kHz — the `minHz`
tightening's full gain, with none of it given back this time.

---

## 2026-08-18 — PSOLA grain-width control: worst-case latency cost of the range chosen

Context: building the "grain width" creative control (`docs/ROADMAP.md`
Phase 5's "Per-algorithm creative parameter exposure" item 2) — a
multiplier on PSOLA's grain half-width, previously fixed at exactly one
period. This entry exists because the control's upper bound turned out to
be a *latency* decision, not just a musical one, and the first value tried
was wrong in a way this project's own testing discipline caught before it
shipped.

**Mechanism:** `getLatencySamples()` must report one fixed constant a host
can rely on — it can't vary with whatever the multiplier happens to be set
to live. That means the constant has to be sized for the *worst case the
multiplier could ever reach*, not the default. The existing formula
(`latencySamples = 2 * maxPeriodSamples`, derived and verified exact in
this log's TD-PSOLA entries above) generalizes to
`latencySamples = 2 * ceil(maxPeriodSamples * grainWidthMultiplierMax)` —
at the multiplier's default (1.0x) this reduces to exactly the old formula
for the DSP *output*, but the *reported latency* is now always the
worst-case number regardless of what the multiplier is actually set to.

**Data — first attempt, `grainWidthMultiplierMax = 3.0`:**

| Sample rate | Worst-case latency (3.0x ceiling) | Phase vocoder (unchanged) |
|---|---|---|
| 44.1kHz | 3312 samples / 75.10ms | 46.4ms |
| 48kHz | 3600 samples / 75.00ms | 42.7ms |

Caught by `tests/DSP/PSOLAPitchShifterTests.cpp`'s pre-existing
latency-formula test (`CHECK (shifter.getLatencySamples() < 2048)`) before
this shipped — a 3.0x ceiling makes this engine's worst-case latency
*worse* than the phase vocoder's, which would silently defeat the entire
reason TD-PSOLA was built in the first place (the latency comparison,
this file's earlier "TD-PSOLA engine" entry). Not a subtle bug — a naive
"pick a musically wide range" instinct with no thought given to what the
range does to the one number a host actually sees.

**Data — recomputed, `grainWidthMultiplierMax = 1.5`:**

| Sample rate | Worst-case latency (1.5x ceiling) | Phase vocoder (unchanged) | Reduction vs. phase vocoder |
|---|---|---|---|
| 44.1kHz | 1656 samples / 37.55ms | 46.4ms | ~19% |
| 48kHz | 1800 samples / 37.50ms | 42.7ms | ~12% |

**Conclusion:** 1.5x keeps a real, meaningful latency advantage — not
coincidentally, almost exactly the same ~19%/~12% figures this log's
"tighter floor" entry already reported and treated as a legitimate win
after the cross-fade fix cost some latency back. The lower bound (0.5x)
has no equivalent cost: narrowing the grain only ever *reduces* the worst
case, so it was left as an ordinary musically-motivated choice with no
latency derivation behind it. Chosen deliberately narrower than a first
instinct (3.0x) would have picked, specifically to protect the property
this engine exists to demonstrate.

**Not yet done:** `benchmarks/PSOLALatencyProbe.cpp`'s onset probe
predates this control and still asserts against the pre-multiplier
formula's numbers — the 37.55/37.50ms figures above are derived
analytically from the (already-verified-exact-at-1.0x) formula, not
re-confirmed by the probe at the new worst case. Flagged as a follow-up,
not treated as equivalent to having measured it — this codebase's own
"measurement over argument where practical" standard, same caveat this
log has used before when a derivation was solid but not yet independently
re-verified after a formula change.

## 2026-08-19 — Varispeed engine: O(sampleRate²) WSOLA search cost, found via live 96kHz testing

**Setup:** New third-algorithm engine (`VarispeedResampler` + `WSOLATimeStretcher`,
see `docs/ARCHITECTURE.md`-adjacent doc comments on both classes), built
and run live in the Standalone app for the first time — not caught by any
of the unit tests written alongside it, since none of them exercised a
non-default sample rate.

**Finding:** live at 96kHz, block 2048 (21,333.3us budget), the plugin's
own performance panel showed:

| stage | cost |
|---|---|
| detect | 213.7us |
| quantize | 1.0us |
| shift | 26,101.1us |
| **total** | **26,315.8us — 23% over budget** |

Audibly, this presented as constant dropouts ("missing buffers") — the
worker thread structurally could not keep up, so the output ring buffer
underran every block, same failure mode this log's very first entry
documented for the original Rust prototype.

**Root cause:** `WSOLATimeStretcher::searchBestOffset` correlated the
*entire* `synthesisHopSamples`-length overlap region at every one of
`2*searchRadiusSamples+1` candidate offsets. Both `synthesisHopSamples`
and `searchRadiusSamples` are derived from `windowSizeSamples`, itself
`sampleRate`-derived — so total search cost scaled as
`O(searchRadius × correlationLength) = O(sampleRate × sampleRate) =
O(sampleRate²)`, not `O(sampleRate)` the way every other per-block cost in
this codebase does. Invisible at 44.1kHz (where it was written and
tested); at 96kHz the (96/44.1)² ≈ 4.7x multiplier pushed it over budget.
Not caught by design review — flagged in the implementation plan as
"needs a real benchmark before shipping," but shipped before that
benchmark was actually run at a non-44.1kHz rate.

**Fix:** capped the correlation length to a fixed constant
(`maxCorrelationLengthSamples = 256`), independent of sample rate, instead
of the full overlap region — a short reference snippet is sufficient to
find a good splice offset; it doesn't need to cover the whole overlap.
This turns search cost back to `O(searchRadiusSamples)` — linear, matching
every other engine's scaling.

**Data — `benchmarks/VarispeedCorrectorPerformance.cpp`, after the fix:**

| Sample rate | Release | Debug |
|---|---|---|
| 44100Hz | ~802us | not re-measured (Release is the meaningful number) |
| 48000Hz | ~805us | not re-measured |
| 96000Hz | ~791us | 7,255us |

Release cost is now flat (~800us) across all three rates — direct
confirmation the fix removed the sample-rate-dependent blowup, not just
reduced it. Debug-at-96kHz (7.26ms) is what the live-tested Standalone app
actually runs; still ~9x Release's cost (consistent with this project's
standing caveat that Debug/-O0 numbers aren't directly comparable to
Release), but now comfortably under the 21.3ms budget (~34%) instead of
23% over it — the fix alone, before any Debug/Release consideration, is
roughly a 3.6x improvement (26.3ms → 7.3ms) at the exact scenario that was
audibly failing.

**Not yet done:** Release-build live/Standalone re-verification by ear at
96kHz (only the offline benchmark and the Debug Standalone app have been
re-checked so far); a systematic sweep of `maxCorrelationLengthSamples`
against splice-quality (does 256 samples correlate as well as the old
full-overlap-region comparison did, or did this trade quality for speed
in a way that needs its own listening pass) — flagged, not assumed
equivalent just because the budget numbers now pass.

## 2026-08-19 — Varispeed WSOLA search radius reduced (correctness fix, latency side effect)

**Context:** follow-up to this same date's search-cost entry above. A separate
bug (`docs/FINDINGS.md` #23) — a periodic discontinuity artifact from the
correlation search degenerating to a trivial no-op match on stationary
material — was fixed in part by reducing `searchRadiusSamples` from
window/4 to window/16 (the other part of the fix, a nominal-position
bias, doesn't affect timing). Logged here specifically because
`WSOLATimeStretcher::getLatencySamples()` is `windowSizeSamples +
searchRadiusSamples`, so this incidentally *lowers* reported latency —
a parameter change with a measurable timing effect, not just a DSP-quality
one, hence its own entry per this log's standing rule rather than folding
it silently into the FINDINGS.md row.

**Data (44.1kHz):**

| | searchRadiusSamples | Total latency (window + radius) |
|---|---|---|
| Before | 276 (window/4) | 1380 samples ≈ 31.3ms |
| After | 69 (window/16) | 1173 samples ≈ 26.6ms |

**Conclusion:** the correctness fix for finding #23 has a fortunate side
effect — latency drops from 31.3ms to 26.6ms, moving Varispeed from
"between PSOLA (~25ms) and the phase vocoder (~46ms)" to "essentially
tied with PSOLA." Not the reason the radius was reduced (that was purely
about the periodicity artifact), but worth recording since it changes a
number this project reports to the user and compares across engines
elsewhere (`docs/COMPARISON.md`-style framing). Not yet re-verified via
`benchmarks/`'s latency-probe pattern the way PSOLA's own worst-case
latency was (`docs/PERFORMANCE_LOG.md`'s PSOLA entries) — the number
above is derived from the formula, not independently confirmed by an
onset probe; flagged as a follow-up, not treated as equivalent to having
measured it.

---

## 2026-08-19 — Worker-thread per-block allocation removed

Context: a design question ("the audio thread must never block/allocate/
lock — should `CorrectorWorker`'s background thread too, for throughput,
even without a hard deadline?") prompted a source audit of the DSP hot
path rather than a profiling run — see `docs/FINDINGS.md` #25 for the
full finding (renumbered from this entry's original #22 when merged into
main — see that finding's note for why). Worth logging here per
`CLAUDE.md` rule 6 since it's a performance-relevant change, even though
it started from a design question rather than a measured budget miss.

**What the audit found:** no locks anywhere in the hot path (confirmed by
grep across every engine/worker file), but two genuine per-block heap
allocations, on every engine, every block: `Corrector::process`/
`PSOLACorrector::process`/`RustCorrectorEngine::process` each returned an
owned `CorrectionResult` whose `.samples` vector was freshly allocated on
every call (`result.samples.assign(blockSize, 0.0f)` on a
default-constructed, zero-capacity vector), and `PitchDetector`'s
internal peak-picking helper (`detectPeaks`) built a fresh local
`std::vector<std::pair<size_t,float>>` every call — despite
`PitchDetector`'s own class doc already (incorrectly) claiming "no
per-block allocation." Neither was ever audible or caught by the Catch2
suite: both are pure allocator-churn/timing-jitter costs, not correctness
bugs — no test distinguishes an allocating implementation from a
non-allocating one that returns identical values.

**Fix:** `PitchEngine::process()` (and `Corrector`/`PSOLACorrector`/
`RustCorrectorEngine`) now write into a caller-supplied
`std::vector<float>& output` instead of returning an owned buffer — the
same out-param convention `PitchShifter::shiftPitch` already used one
layer down. `CorrectorWorker` owns three persistent, once-sized scratch
buffers (`engineOutput`, `crossfadeOutput`, `blendedOutput`), which also
removes the (larger, 3x) crossfade-path allocation as a side effect.
`PitchDetector::detect()` reuses a persistent `peakScratch` member,
`.clear()`'d rather than reallocated each call. Verified via the full
Catch2 suite (36 test cases, 2323 assertions, all passing) after updating
every call site (3 concrete engines, `CorrectorWorker`, 3 test files, the
`CorrectorPerformance` benchmark — ~25 sites total).

**Data — post-fix baseline** (`benchmarks/CorrectorPerformance.cpp`,
**Debug** build — not comparable to this log's earlier Release-profile
370us figure, see the "controlled for build profile" entry above for why
mixing build profiles produced a misleading number once already):

| Sample rate | `Corrector::process` mean (Debug, block 2048) |
|---|---|
| 44.1kHz | 3.028ms |
| 48kHz | 3.065ms |
| 96kHz | 3.104ms |

**Caveat:** this is a post-fix baseline, not an isolated before/after
delta — getting a true A/B number would mean rebuilding the pre-fix
source in the same Debug configuration, which this session didn't do
(the JUCE/Corrosion/Rust-FFI build cycle in this environment runs
10+ minutes each time). The qualitative case for the fix — two
allocate-then-free cycles removed from every block, on every engine, on
a thread whose throughput has to keep up with the audio thread's
consumption rate on average even without a hard per-block deadline — is
the primary evidence; this table is a number for a future session to
diff against, not a measured improvement claim on its own.

---

## 2026-08-19 — Phase-vocoder window tightened; TD-PSOLA's latency lead inverted

Context: a UI-clarity question about the "Latency" display (why it can't
be decomposed into the per-stage compute-time meters) led to actually
looking at *why* the phase vocoder's latency was what it was —
`PitchShifter.cpp`'s `computeFrameSize` rounds `windowSizeMs` to the
**nearest power of two**, a step function, not a smooth tradeoff. That
raised the obvious question: was `windowSizeMs = 50` sitting well inside
a rounding plateau, or right at a boundary where a small change would
buy a real win?

**Data — deterministic, from the documented formula** (no build needed
for this part):

| `windowSizeMs` | frameSize (nearest pow2) | 44.1kHz | 48kHz | 96kHz |
|---|---|---|---|---|
| 50 (old) | 2048 / 2048 / 4096 | 46.44ms | 42.67ms | 42.67ms |
| 20-35 (plateau) | 1024 / 1024 / 2048 | 23.22ms | 21.33ms | 21.33ms |

Any value from ~20-35ms lands on the identical halved frame size — the
whole range is one plateau below the 50ms one, with the rounding
boundary sitting at ~32ms. 30 was chosen to sit mid-plateau rather than
near either edge.

**Confirmation:** built the Standalone app with `windowSizeMs = 30`
(`PluginProcessor.h`) and listened on real vocal input across both
phase-vocoder engines — user-confirmed "sounds fine." No automated test
in this codebase can validate phase-vocoder reconstruction quality
(`PitchShifterTests.cpp`'s own comment says its check is a loose sanity
bound, not a precision assertion), so this is real-time listening again
being the only tool that actually catches this artifact class, same
lesson `docs/FINDINGS.md` has already logged more than once for PSOLA.
**Shipped**, 50→30.

**Side effect discovered during the same pass:** the Rust FFI engine's
phase vocoder derives its frame size completely differently —
`crates/pitch-core/src/corrector.rs` rounds `sampleRate · windowMs /
1000` up to the nearest *even* number, a near-continuous function, not
C++'s step function. Same nominal `windowSizeMs = 30` therefore produces
different real latency on the two engines:

| Engine | 44.1kHz | 48kHz | 96kHz |
|---|---|---|---|
| Native C++ (nearest pow2) | 23.22ms | 21.33ms | 21.33ms |
| Rust FFI (round to even) | 30.02ms | 30.00ms | 30.00ms |

This divergence already existed at `windowSizeMs = 50` (46.4ms vs.
50.0ms, a ~7% gap — see this log's "Rust-vs-C++ per-block cost" entry
above) but was easy to describe informally as "about the same." At 30ms
the gap is ~29% (30.0ms vs. 23.2ms) precisely because C++'s step function
had a full octave of room to drop (2048→1024) while Rust's rounding just
tracked the target down linearly. Full mechanism explained in
`docs/ALGORITHMS.md`'s phase-vocoder section — both engines' display
names were changed from "Native C++"/"Rust (FFI)" to "Phase Vocoder
(Native C++)"/"Phase Vocoder (Rust FFI)" in the same pass, since which
*algorithm* each engine runs is now directly relevant to reading the
latency display, not just an implementation detail.

**Bigger consequence: this inverted the phase-vocoder-vs-TD-PSOLA latency
ordering.** TD-PSOLA's latency doesn't depend on `windowSizeMs` at all —
it has independent tuning knobs (`minHz`, `grainWidthMultiplierMax`) —
so tightening only the phase vocoder's window was enough to flip which
engine is lower-latency by default:

| Engine | 44.1kHz | 48kHz |
|---|---|---|
| Phase vocoder (was) | 46.44ms | 42.67ms |
| Phase vocoder (now) | 23.22ms | 21.33ms |
| TD-PSOLA (unchanged, `minHz=80`, `grainWidthMultiplierMax=1.5`) | 37.55ms | 37.50ms |

TD-PSOLA was originally built specifically *because* it was the
lower-latency engine (`docs/ROADMAP.md`'s original framing) — that
framing no longer holds against the tightened phase vocoder. Decision
(user-directed, 2026-08-19): document the inversion as a genuine finding
rather than either (a) silently re-tuning PSOLA back into the lead, or
(b) reverting the phase-vocoder change to preserve the old narrative.
The interesting story is that the ordering was never an algorithmic
constant — it was always a function of independent tuning choices on
both sides, and this is direct, measured evidence of that rather than an
assertion of it.

**Follow-up explored in the same pass: can TD-PSOLA go lower too?**
`minHz` (80Hz) is a documented correctness floor — "the bottom of a
typical bass vocal's fundamental" — not just a latency knob, so it
wasn't touched. `grainWidthMultiplierMax` only trades away creative-
control range (no correctness cost), so it was the lever pulled:

| `grainWidthMultiplierMax` | 44.1kHz | 48kHz | vs. old 1.5x |
|---|---|---|---|
| 1.5 (old) | 37.55ms | 37.50ms | — |
| 1.25 (experimental, pending listening) | 31.29ms | 31.25ms | ~17% lower |
| 1.0 (not chosen) | 25.03ms | 25.00ms | ~33% lower, but removes all "widen" headroom above the 1.0x default |

1.25 was picked over 1.0 specifically to leave the creative control some
room to widen the grain above its default, at the cost of a smaller
latency win. Built alongside the phase-vocoder change for the same
listening session; **pending confirmation** — not yet a committed
default, same caveat `windowSizeMs` had before this session's listening
pass confirmed it. Does not fully reclaim TD-PSOLA's latency lead over
the now-tightened phase vocoder (31ms vs. 21-23ms) — see
`docs/ALGORITHMS.md` for why `minHz` was deliberately left alone rather
than pushed further to try to close that remaining gap.
