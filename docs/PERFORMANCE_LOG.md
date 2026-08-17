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
