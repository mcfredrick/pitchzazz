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
