# Architecture

## Signal chain

```
                    real-time audio thread (cpal)
                    ─────────────────────────────
mic ──> input callback ──push_slice──> [lock-free ring buffer] ──pop_slice──┐
        (downmix to mono,                                                   │
         fixed-size stack buffer,                                           │
         no allocation, no locks)                                           │
                                                                              ▼
                                                              worker thread (no deadline)
                                                              ─────────────────────────
                                                              accumulate BLOCK_SIZE samples
                                                              -> PitchCorrector::process()
                                                                 - McLeod pitch detection
                                                                 - scale quantization
                                                                 - pitch shift
                                                              -> push_slice into output ring
                                                                              │
                    ─────────────────────────                                │
                    real-time audio thread (cpal)                            │
speakers <── output callback <──pop_slice── [lock-free ring buffer] <────────┘
             (up-mix mono to N channels,
              underrun -> silence, never blocks)
```

`pitch-core` (the DSP: detection, scale quantization, shifting) has no I/O
and spawns no threads. `pitch-cli` owns all the real-time plumbing above and
is the only crate that touches an audio callback.

## Real-time audio safety

The two closures passed to `cpal::Device::build_input_stream` /
`build_output_stream` in `pitch-cli/src/main.rs` run on the audio driver's
real-time thread, under a hard per-block deadline. Nothing in this codebase
may block, allocate, or take a lock from inside those closures — there is no
"try again next block," a missed deadline is an audible glitch.

### What the original prototype got wrong

This project is a clean rebuild of an earlier prototype (`rusty-tuner`,
2023) whose core pitch-detection and pitch-shifting DSP worked, but whose
real-time plumbing violated the no-block/no-allocate/no-lock rule three
different ways in about ten lines of code:

1. **Lock in the callback.** `Arc<Mutex<AudioProcessor>>.lock()` was called
   directly inside both the input and output audio callbacks. If the
   worker thread was holding that lock when a callback fired, the callback
   blocked — worst case, priority inversion: the (lower-priority) worker
   thread gets preempted while holding the lock, and the (higher-priority,
   deadline-bound) audio thread has no guarantee of getting it back in time.
2. **A blocking channel in the callback.** The input side used
   `std::sync::mpsc::Sender::send()` from inside the callback. `std`'s mpsc
   is itself backed by a mutex/condvar internally — same problem, different
   API.
3. **Allocation in the callback.** `data.clone()` ran on every single
   callback invocation — a heap allocation on the audio thread, on the
   steady-state path, not an edge case.

The double irony on (1): the buffer being locked was a `ringbuf::HeapRb`,
whose entire reason to exist is to give you a lock-free structure so you
*don't* need a mutex. Wrapping it in `Arc<Mutex<_>>` instead of calling
`.split()` (see below) pays for a lock-free structure and then puts a lock
in front of it anyway — strictly worse than a plain `Vec` + mutex would have
been, since it's doing more work to get the same blocking behavior.

### The fix used throughout this codebase

- Both directions (mic → worker, worker → speakers) go through a
  `ringbuf::HeapRb` **split** into a `Producer`/`Consumer` pair
  (`HeapRb::new(capacity).split()`). Splitting is what makes it lock-free —
  the two halves communicate via atomic head/tail indices
  (`CachePadded<AtomicUsize>`, `Ordering::Acquire`/`Release`), not a mutex.
  Verified by reading the crate source directly
  (`~/.cargo/registry/.../ringbuf-0.3.3/src/`) rather than trusting the
  docs: `push_slice`/`pop_slice` copy into/out of the buffer via
  `as_uninit_slices()`, bounded by slice length, no allocation. The only
  `Mutex` usages anywhere in that crate are in its own test suite,
  demonstrating the *wrong* way to use it for a different concurrency
  model — exactly the mistake the original prototype made.
- `push_slice` / `pop_slice` never block: if the ring buffer is full or
  empty they copy fewer samples than requested and return the count, so a
  callback can always finish in bounded time. An output underrun is filled
  with silence, not a wait — silence is the correct real-time-safe failure
  mode, a stall is not.
- All DSP work (`PitchCorrector::process` — pitch detection, scale
  quantization, pitch shifting) happens on a plain worker thread with no
  deadline, free to allocate and log. It polls the input ring buffer on a
  1ms sleep rather than blocking on a condvar signaled from the callback —
  deliberately: even a condvar `notify()` from the callback is a debatable
  real-time-safety call (potential syscall), and a short poll on a thread
  that has no deadline sidesteps the question entirely.
- The multichannel downmix/upmix paths in both callbacks use a **fixed-size
  stack array**, chunked over the callback's buffer — not a `Vec`. An
  earlier draft of the output callback used `vec![0.0f32; frames]` inside
  the closure; caught in review before it shipped. Left as a comment in
  `main.rs` on purpose, because it's a realistic mistake to make even when
  you know the rule.
- `err_fn` (cpal's stream-error callback) is the one deliberate, documented
  exception: it prints via `eprintln!`. This fires on stream-level errors
  (device disconnected, etc.), not on the steady-state per-block path, so
  it's a pragmatic exception rather than an oversight — flagged as such in
  the code.

### Dependency audit

The only third-party code that executes *inside* a real-time callback in
this codebase is `ringbuf`'s `Producer`/`Consumer` (audited above — clean).
Everything else (`pitch-detection`, `pitch_shift`, `rust-music-theory`,
`clap`, `anyhow`) only ever runs on the worker thread or at startup, by
construction — `PitchCorrector` is never called from `main.rs`'s callback
closures. Before adding any new dependency whose code would run inside a
callback, audit it the same way this doc audits `ringbuf`: find the actual
source in `~/.cargo/registry/src/`, not just the docs, and check for
`Mutex`/`RwLock`/allocation on the hot path.

## Block size vs. host callback size

`PitchCorrector` requires a fixed block size, sized once at construction
(the McLeod detector's internal buffers are allocated for a specific size —
the original prototype rebuilt the detector on every single call, which
this rebuild deliberately avoids). Audio hosts do **not** guarantee a fixed
callback buffer size. `pitch-cli`'s worker thread reconciles this by
accumulating samples from the input ring buffer until it has exactly
`BLOCK_SIZE`, decoupling the DSP's analysis block size from whatever the
driver hands the input callback.

## Latency

Pitch detection needs lookahead (the analysis window), so this pipeline has
inherent latency — it is not achievable to have zero-latency real-time
pitch correction with a windowed detector. `--startup-latency-ms` pre-fills
the output ring buffer with silence so the output callback has something to
drain before the worker thread produces its first corrected block, trading
a small fixed startup delay for immunity to an audible dropout while the
pipeline is still filling. The C++/JUCE port (`docs/ROADMAP.md` Phase 2)
uses the equivalent host-facing mechanism, `AudioProcessor::setLatencySamples()`,
so the DAW can compensate — the CLI has no host to report to, so it only
has the startup-silence pre-fill instead.

**Measured, not just estimated** (`docs/PERFORMANCE_LOG.md`'s 2026-08-17
"Measured pipeline latency" entry, both engines): the pipeline's own
software latency is exactly one phase-vocoder analysis window's worth of
samples — 50.0ms for the Rust engine at every sample rate tested (by
construction: its frame size is always `sampleRate * 50ms`), 42.7-46.4ms
for the C++ engine (its frame size is rounded to the nearest power of two
for `juce::dsp::FFT`, so it isn't exactly 50ms worth of samples — see
`docs/COMPARISON.md`). The block-accumulation delay (`BLOCK_SIZE`/sample
rate) does **not** add on top of that — whichever of the two lookahead
requirements (block accumulation vs. the phase vocoder's own window) is
larger wins, and in this design the phase vocoder's window is always the
larger one, so it's the sole determinant of measured latency. This
revises the earlier ~70-100ms estimate below (which assumed the two
stack additively) down to the actual measured ~43-50ms range — real
audio-hardware I/O buffering latency is still separate, additive, and not
included in this measurement.

## Performance and latency

Worker-thread processing time vs. the real-time budget, the oversampling
root-cause and fix, and a researched comparison against commercial
pitch-correction products' real-world latency are all tracked in
`docs/PERFORMANCE_LOG.md`, not here — that doc is the append-only
investigation trail (data, hypothesis, fix, re-measurement) and is kept
current; this section would just go stale duplicating it. Read that doc's
latest entries for current status before changing anything performance- or
latency-related.

One architectural point worth keeping here rather than in the log, since
it's a standing design fact rather than a dated finding: this pipeline has
two latency components that are easy to conflate. *Processing time* is how
long `PitchCorrector::process()` takes on the worker thread — a CPU-cost
question, bounded by the real-time budget (`BLOCK_SIZE`/sample_rate).
*Pipeline latency* is the total mic-to-speaker delay, dominated by the
phase vocoder's analysis window (`window_size_ms`, currently 50ms) plus
the block-accumulation delay — a design-parameter question, not a
CPU-speed question. Optimizing one does not fix the other; see
`docs/PERFORMANCE_LOG.md` for why that distinction mattered in practice.
