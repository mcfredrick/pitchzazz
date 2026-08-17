# Findings

A consolidated, presentation-ready ledger of every bug found, the
validation/tool that surfaced it, the root cause, and the fix — across
both the Rust engine and the C++/JUCE port. This is the "found a real
problem, understood why, fixed it robustly" evidence for the demo in one
place, rather than scattered across `ARCHITECTURE.md`, `PERFORMANCE_LOG.md`,
and `ROADMAP.md`'s dated checkpoint entries (which still have the full
narrative — this doc is the index/summary, not a replacement).

Append-only, like `PERFORMANCE_LOG.md`: new findings get a new dated
entry, existing entries don't get edited to match later understanding.

## Quick-reference table

| # | Date | Engine | Bug | Found by | Fix |
|---|------|--------|-----|----------|-----|
| 1 | 2023 (original prototype) | Rust | Lock (`Mutex`) taken inside the real-time audio callback | Code review during rebuild | Lock-free `ringbuf` split `Producer`/`Consumer` |
| 2 | 2023 (original prototype) | Rust | Blocking `mpsc::Sender::send()` inside the audio callback | Code review during rebuild | Same lock-free ring buffer replaces the channel |
| 3 | 2023 (original prototype) | Rust | Heap allocation (`data.clone()`) inside the audio callback | Code review during rebuild | Fixed-size stack buffers, no per-callback allocation |
| 4 | 2023 (original prototype) | Rust | Scale-quantization table sized to `notes().len()` silently mismatched `intervals().len()` for some scale/mode combos → wrong "nearest note" | Code review during rebuild | Rewrote to read `Scale::notes()` pitch classes directly, no reconstructed table |
| 5 | 2026-08-17 | Rust | Pitch shifter's oversampling factor (32, unmeasured leftover from the prototype) put most blocks 40-60% over the real-time budget | Live profiling (`docs/PERFORMANCE_LOG.md`) | Dropped to 8 — ~4x reduction in shift-stage cost |
| 6 | 2026-08-17 | Toolchain (Rust) | `rustup`/`libclang` drifted to x86_64-under-Rosetta, breaking `coreaudio-sys`/`bindgen` builds | Build failure + `rustup show` | `rustup default stable-aarch64-apple-darwin` |
| 7 | 2026-08-17 | Toolchain (C++) | Built VST3 was x86_64-only on an arm64 Mac — built fine, failed to load in-process | PluginVal (`Unable to load VST-3 plug-in file`) | Pinned `CMAKE_OSX_ARCHITECTURES=arm64` explicitly in `CMakeLists.txt` — see entry below for root cause |
| 8 | 2026-08-17 | C++ scaffolding | Generated project defaulted the plugin to a MIDI effect (`IS_MIDI_EFFECT TRUE`, `AU_MAIN_TYPE kAudioUnitType_MIDIProcessor`) — wrong regardless of intent: a true JUCE "MIDI effect" type processes MIDI only, no audio I/O at all, which can't host a pitch-*correction* effect either way | Manual review of generated `CMakeLists.txt` | Set `IS_MIDI_EFFECT FALSE`, `AU_MAIN_TYPE kAudioUnitType_Effect` — audio in/out restored. `NEEDS_MIDI_INPUT` left `FALSE` for now; see `docs/ROADMAP.md` Phase 5 for the MIDI-controlled-pitch ("vocoder mode") feature idea this default may have been hinting at |
| 9 | 2026-08-17 | C++ scaffolding | Template hardcoded its own `PLUGIN_CODE`/`PLUGIN_MANUFACTURER_CODE` ("Mrbl"/"Juce") instead of reading ours from `.env` | Manual review of generated `CMakeLists.txt` | Wired `$ENV{PLUGIN_CODE}`/`$ENV{PLUGIN_MANUFACTURER_CODE}` through, matching the `COMPANY_NAME` pattern already in the template |
| 10 | 2026-08-17 | C++ DSP port | JUCE's `dsp::FFT::perform(..., inverse)` auto-normalizes by `1/N`; the Rust engine's FFT libraries (`rustfft`/`realfft`) normalize neither direction | Source audit of both FFT libraries, done proactively before it could ship as a silent bug | Explicit compensating multiply in `PitchDetector::normalizedSquareDifference` and `PitchShifter`'s `accOversamp` constant |
| 11 | 2026-08-17 | C++ real-time wiring | `juce::Thread` destructor asserts (UB) if destroyed while still running — the `AudioProcessor` destructor never stopped the worker thread | Apple's `auval` (`JUCE Assertion failure in juce_Thread.cpp:55`) — not caught by PluginVal | Unified `stopWorker()` helper, called from `prepareToPlay`, `releaseResources`, *and* the destructor, since hosts aren't guaranteed to call `releaseResources()` before destroying the processor. Re-ran `auval -v aufx PIHZ MAFR` after the fix: assertion gone, `AU VALIDATION SUCCEEDED` clean. |
| 12 | 2026-08-17 | Toolchain (Corrosion) | `corrosion_add_cxxbridge`'s `CRATE` argument needs the CMake target name Corrosion actually created, which silently normalizes the Cargo package name's hyphens to underscores (`pitch-core-ffi` → `pitch_core_ffi`) — not documented, and the wrong name fails as a generic `get_target_property() called with non-existent target` error inside `Corrosion.cmake` with no hint it's a naming mismatch | CMake configure error; root-caused by empirically listing `BUILDSYSTEM_TARGETS` after `corrosion_import_crate()` rather than guessing | Used the correct target name (`pitch_core_ffi`); left a comment in `cpp-plugin/CMakeLists.txt` noting this was confirmed empirically, not from documentation |
| 13 | 2026-08-17 | Toolchain (Corrosion + cxx) | The generated cxx bridge header's include path doesn't match plain `cxx-build`'s convention (`<crate-name>/<source-path>.h`) — Corrosion names it after the CMake cxx-bridge target instead (`pitch_core_ffi_cxx/lib.h`), so `#include "pitch-core-ffi/src/lib.rs.h"` (correct for a standalone `cxx-build` project, confirmed by reading the crate's own build output first) fails under Corrosion specifically | Compile error, `fatal error: 'pitch-core-ffi/src/lib.rs.h' file not found`; root-caused by inspecting the actual generated file tree under `build/corrosion_generated/cxxbridge/` rather than assuming cxx's usual convention applied | Used the actual generated path (`pitch_core_ffi_cxx/lib.h`), with a comment explaining the naming difference is Corrosion-specific, not a cxx convention |
| 14 | 2026-08-17 | C++ hot-swap (real-time correctness, not just wiring) | Instant engine swap discarded the outgoing engine's warmed-up internal phase-vocoder state (windowing FIFO, phase accumulator, overlap-add buffer) and started the new engine cold — an audible-scale discontinuity, not a crash or hang | A new automated dropout-detection test (`tests/DSP/HotSwapDropoutTests.cpp`, per `docs/TESTING.md`), self-calibrated against a no-swap baseline rather than an arbitrary threshold: measured 0.340 max sample delta vs. a 0.014 natural baseline (~24x) — no existing test (unit tests, PluginVal, `auval`) would have caught this, since each engine was individually correct in isolation. **Also caught something the automated test didn't**: after the fix below got the metric comfortably passing, a manual listening test in Ableton still heard a quiet click — the automated click-detector threshold and genuine perceptual inaudibility turned out not to be the same bar, confirmed by a human ear, not assumed. | Crossfade in `CorrectorWorker`: both engines process the same input in parallel for a window after a swap, blended, before fully switching over. Iterated three times against real measurements, not by loosening the test: (1) 1-block linear blend → 0.201, still failing; (2) 4-block linear blend → 0.051, passing the automated test, but a listening test still heard a quiet click; (3) 4-block **equal-power** blend (cos/sin gains instead of linear, the standard fix for the loudness-dip a linear amplitude crossfade causes) → user-confirmed "better, but still there." The automated metric actually rose slightly with this change (0.051→0.078, still well under threshold) even as the perceptual result improved — the click-detector metric and perceived smoothness aren't the same thing, worth remembering when trusting either number alone. Root cause of the remaining residual: the two engines' phase accumulators are independently evolving with no shared reference point, which crossfading (blending amplitude) doesn't fully address — a real limitation of swapping between two differently-implemented stateful algorithms, not a bug to hunt down further within the current approach. |

## Validation tools used, and what each one actually catches

Worth being explicit about this for the demo — no single tool caught
everything, and that's the point:

- **`cargo test` / Catch2 (`Tests` target)**: unit-level correctness —
  algorithm logic, edge cases, numeric parity between the two engines.
  Catches logic bugs, not host-integration or lifecycle bugs (finding #11
  passed every unit test; the plugin's DSP logic was never the problem).
- **`cargo clippy`**: Rust-side lint/style — not used for anything in this
  table, but part of the standard gate (`CLAUDE.md` rule 5).
- **PluginVal (strictness 5)**: host-level plugin-lifecycle stress
  testing — many sample rates/block sizes, bus configuration, editor
  lifecycle, state save/restore. Caught #7 (architecture mismatch) but
  *not* #11 (the thread-destruction assertion) — PluginVal's own harness
  apparently doesn't exercise the "destroy without calling
  releaseResources" path the way `auval` does.
- **Apple's `auval`**: the actual validator Logic Pro/GarageBand use
  internally, run independently of PluginVal's harness. Caught #11, which
  PluginVal missed — direct evidence that one validator isn't enough and
  cross-checking against a second, independently-implemented one matters.
- **Manual source audit** (reading the actual crate/framework source in
  `~/.cargo/registry/src/` and `~/.juce_cache/juce-src/`, not trusting
  docs): caught #10 before it ever produced a wrong answer, and confirmed
  #1/#2's `ringbuf` fix and this port's `juce::AbstractFifo` choice were
  actually lock-free rather than assuming it from the class name. Doesn't
  catch integration/lifecycle bugs the way a real host validator does —
  #11 was invisible to source audit since `AudioProcessor`'s own
  destructor code was correct in isolation; the bug was in what *wasn't*
  there (no `stopThread()` in the destructor).

## Full narrative

Each finding's complete story — data, hypothesis, fix, re-verification —
lives in dated entries in the doc closest to what changed:

- Findings #1-4: `docs/ARCHITECTURE.md`'s "What the original prototype
  got wrong" and `crates/pitch-core/src/scale.rs`'s module doc.
- Finding #5: `docs/PERFORMANCE_LOG.md`.
- Finding #6: root `CLAUDE.md`'s Build/test/run section.
- Findings #7-11: `docs/ROADMAP.md` Phase 2's dated Day 1/3/4 checkpoint
  entries.
- Findings #12-14: `docs/ROADMAP.md` Phase 3's hot-swap progress entry;
  finding #14's full before/after numbers also in `docs/TESTING.md`'s
  "Hot-swap dropout detection" section.
