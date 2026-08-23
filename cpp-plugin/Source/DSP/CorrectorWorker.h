#pragma once

#include "AudioSessionRecorder.h"
#include "PitchEngine.h"
#include "RetuneSmoothing.h"
#include "ScopeCapture.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>

namespace pitchzazz
{

/// Owns a swappable PitchEngine and runs it on a plain background thread
/// with no real-time deadline — direct port of pitch-cli's worker thread
/// (see docs/ARCHITECTURE.md): accumulate `blockSize` samples from the
/// input FIFO, run one correction pass, push the result to the output
/// FIFO, poll on a short wait rather than blocking on a condvar signaled
/// from the audio thread (same rationale as the Rust version — even a
/// condvar notify() from processBlock is a debatable real-time-safety
/// call, and a short poll on a thread with no deadline sidesteps the
/// question entirely).
///
/// `processBlock` (the real-time audio thread) never touches this class
/// directly — only the AbstractFifo/backing-buffer pairs passed in here,
/// which are safe for concurrent lock-free single-producer/single-
/// consumer access by construction.
///
/// Engine hot-swap (docs/ROADMAP.md Phase 3): `requestEngineSwap()` hands
/// off a new, already-constructed engine via a single atomic pointer
/// exchange — a lock-free single-producer/single-consumer handoff, same
/// shape as the FIFOs above but for one owned object instead of a sample
/// stream. The worker picks it up between blocks (never mid-`process()`
/// call), so processBlock is entirely unaffected by a swap in progress —
/// the only way a swap could cause an audible gap is if constructing the
/// *new* engine were slow enough to make the worker fall behind, which is
/// why construction happens on the caller's side, before the handoff, not
/// inside this class during the swap itself.
///
/// That handles the concurrency side of "no dropout" but not the DSP
/// side: each engine keeps independent internal phase-vocoder state
/// (windowing FIFO, phase accumulator, overlap-add buffer), so an instant
/// switch discards the old engine's warmed-up state and starts the new
/// one cold — measured as a real, audible-scale discontinuity (0.34 vs. a
/// 0.014 natural baseline) by tests/DSP/HotSwapDropoutTests.cpp before
/// this fix existed; see docs/FINDINGS.md. Fixed with a multi-block
/// equal-power crossfade: for a window after a swap, *both* the old and
/// new engine process the same input and their outputs are blended
/// (cos/sin gains, not linear — linear amplitude blending dips below full
/// power mid-transition, audible as a loudness dip even once the
/// discontinuity test itself passes; equal-power keeps gainOld^2 +
/// gainNew^2 == 1 throughout), then the worker fully switches to the new
/// engine — smooths the transition instead of eliminating the state
/// mismatch (which isn't possible in general; the two engines' internal
/// representations aren't compatible with each other), same principle an
/// audio crossfade always uses for switching sources.
class CorrectorWorker : public juce::Thread
{
public:
    /// `blockSize` must match whatever `initialEngine` was constructed
    /// with — this class doesn't second-guess that, it's the caller's
    /// responsibility (see PitchEngineRegistry::createEngine's
    /// EngineConfig, which is the single source of truth for both).
    CorrectorWorker (int blockSize, double sampleRate, std::unique_ptr<PitchEngine> initialEngine,
                      juce::AbstractFifo& inputFifo, std::vector<float>& inputBuffer,
                      juce::AbstractFifo& outputFifo, std::vector<float>& outputBuffer);
    ~CorrectorWorker() override;

    void run() override;

    /// Safe to call from any thread (the message thread, via the editor's
    /// combo-box callbacks) — plain relaxed atomics, picked up on the
    /// worker's next block rather than applied immediately. No lock
    /// needed: eventual consistency is fine for a UI-driven parameter
    /// like this, unlike the audio-data FIFOs above.
    void setScale (Scale newScale) noexcept;

    /// The classic Auto-Tune controls (docs/ROADMAP.md Phase 5) — same
    /// relaxed-atomic, eventually-consistent forwarding as setScale above,
    /// applied to the active engine (and, during a crossfade, the
    /// incoming one too) every block in run(). Silently a no-op on an
    /// engine that doesn't support them (PitchEngine's default virtuals);
    /// see getActiveSupportsRetuneControls() for how the UI can tell.
    void setCorrectionAmount (float amount) noexcept;
    void setRetuneSpeedMs (float speedMs) noexcept;

    /// Whether the currently active engine implements the controls above
    /// — for the UI to disable them honestly on an engine that doesn't,
    /// rather than accepting input that silently does nothing. Same
    /// eventual-consistency caveat as getActiveEngineName above.
    bool getActiveSupportsRetuneControls() const noexcept;

    /// PSOLA-only "grain width" creative control (docs/ROADMAP.md Phase 5,
    /// item 2) — same relaxed-atomic forwarding and eventual-consistency
    /// contract as setCorrectionAmount/setRetuneSpeedMs above, just gated
    /// on a different capability flag (getActiveSupportsGrainWidthControl)
    /// since it's PSOLA-specific rather than shared with the native
    /// phase-vocoder engine the way retune controls are.
    void setGrainWidthMultiplier (float multiplier) noexcept;
    bool getActiveSupportsGrainWidthControl() const noexcept;

    /// Hands off a new engine for the worker to switch to. `newEngine`
    /// must already be fully constructed (see class doc for why) and
    /// built with the same `blockSize` this worker was constructed with.
    /// Safe to call from the message thread; takes effect on the worker's
    /// next iteration, not synchronously.
    void requestEngineSwap (std::unique_ptr<PitchEngine> newEngine) noexcept;

    /// Name of the engine currently active on the worker thread — safe to
    /// call from the message thread for UI display, but note it may
    /// briefly lag an in-flight requestEngineSwap() (eventual
    /// consistency, same as setScale above).
    const char* getActiveEngineName() const noexcept;

    /// Algorithmic pipeline latency of the currently active engine, in
    /// samples — see PitchEngine::getLatencySamples's doc. Same
    /// eventual-consistency caveat as getActiveEngineName above.
    int getActiveLatencySamples() const noexcept;

    /// Most recent per-stage processing time, in microseconds — for the
    /// GUI's live processing-time meter (docs/ROADMAP.md Phase 4). Purely
    /// informational: reading these has no effect on the audio path,
    /// same as the other UI-facing getters here.
    double getLastDetectUs() const noexcept;
    double getLastQuantizeUs() const noexcept;
    double getLastShiftUs() const noexcept;

    /// Most recent detected pitch in Hz (0 if the block was unvoiced/
    /// silent) and the semitone shift applied to correct it — for the
    /// GUI's live pitch display. Same purely-informational, eventual-
    /// consistency contract as the timing getters above. The *corrected*
    /// Hz isn't stored separately: it's `detectedHz * 2^(semitoneShift/12)`,
    /// cheap enough to derive in the GUI rather than duplicate here.
    float getLastDetectedHz() const noexcept;
    float getLastSemitoneShift() const noexcept;

    /// The GUI scope's data source (docs/ROADMAP.md's "one-period
    /// waveform visualization" item) -- lock-free, see ScopeCapture.h's
    /// own doc for why a seqlock rather than a mutex even on this
    /// no-deadline thread.
    [[nodiscard]] const ScopeCapture& getScopeCapture() const noexcept { return scopeCapture; }

    /// Starts/stops writing the raw audio half of a scope recording
    /// (docs/ROADMAP.md's scope-recording item) to `sessionDir`, which
    /// must already exist. Safe to call from the message thread: same
    /// "hand off a pointer, applied on the worker's next iteration, never
    /// mid-process()" pattern requestEngineSwap() already uses, not
    /// applied synchronously -- see run()'s pickup of pendingRecordingStart
    /// for why that matters here too (AudioSessionRecorder::start/stop
    /// aren't safe to call concurrently with writeBlock()).
    void requestStartRecording (const juce::File& sessionDir)
    {
        auto* old = pendingRecordingStart.exchange (new juce::File (sessionDir), std::memory_order_release);
        delete old; // a second start request superseding an unconsumed one -- same drop-and-replace rule as requestEngineSwap
    }
    void requestStopRecording() noexcept { pendingRecordingStop.store (true, std::memory_order_release); }

    /// Eventually-consistent, same caveat as getActiveEngineName() above --
    /// may briefly lag an in-flight start/stop request.
    bool isRecording() const noexcept { return audioRecorder.isRecording(); }

private:
    // 4 blocks (~185ms at 44.1kHz/2048) rather than 1 — a single-block
    // linear blend measurably reduced the swap-boundary discontinuity
    // (docs/FINDINGS.md) but didn't fully eliminate it: the two engines'
    // phase-vocoder reconstructions can be out of phase with each other
    // even while amplitude-blended, so a longer, gentler ramp reduces the
    // worst-case per-sample slope the blend itself contributes.
    static constexpr int crossfadeBlocks = 4;

    std::unique_ptr<PitchEngine> engine;
    std::unique_ptr<PitchEngine> crossfadeEngine; // non-null only during the crossfade after a swap
    int crossfadeBlockIndex = 0;                  // 0..crossfadeBlocks-1, progress through the crossfade
    int blockSize;
    double sampleRate;

    juce::AbstractFifo& inputFifo;
    std::vector<float>& inputBuffer;
    juce::AbstractFifo& outputFifo;
    std::vector<float>& outputBuffer;

    std::vector<float> analysisBuffer;
    int filled = 0;

    // Output scratch for engine->process() — sized once here (constructor
    // initializer list, same as analysisBuffer above) and reused every
    // block rather than letting CorrectionResult own/return a freshly-
    // allocated buffer each call (see PitchEngine::process's doc and
    // docs/FINDINGS.md). engineOutput holds the active (or, during a
    // crossfade, the outgoing) engine's output; crossfadeOutput holds the
    // incoming engine's; blendedOutput holds the two blended together —
    // three separate buffers because the crossfade branch below needs
    // all three alive simultaneously.
    std::vector<float> engineOutput;
    std::vector<float> crossfadeOutput;
    std::vector<float> blendedOutput;

    std::atomic<int> pendingTonicPitchClass;
    std::atomic<int> pendingMode;
    // Hardcoded to this project's original defaults rather than threaded
    // in from the processor's current setting at construction time — same
    // convention pendingTonicPitchClass/pendingMode above already use (see
    // PluginProcessor::prepareToPlay, which constructs the *engine* itself
    // with the caller's current values via EngineConfig, but always starts
    // a fresh worker's pending-atomic defaults at this class's own
    // baseline). Consistent with existing behavior, not a new gap.
    std::atomic<float> pendingCorrectionAmount { correctionAmountMax };
    std::atomic<float> pendingRetuneSpeedMs { retuneSpeedMsMin };
    // 1.0 = PSOLAPitchShifter's own fixed-at-one-period default — see
    // its doc for why that's the natural default rather than an edge of
    // the range.
    std::atomic<float> pendingGrainWidthMultiplier { 1.0f };

    // Ownership handoff for the pending engine swap: the message thread
    // releases a unique_ptr into this raw pointer (std::atomic<T*> has no
    // owning-smart-pointer equivalent), and the worker thread is the only
    // one ever allowed to turn it back into a unique_ptr and take
    // ownership — see requestEngineSwap()/run()'s implementation.
    std::atomic<PitchEngine*> pendingEngine { nullptr };
    std::atomic<const char*> activeEngineName { nullptr };
    std::atomic<bool> activeSupportsRetuneControls { false };
    std::atomic<bool> activeSupportsGrainWidthControl { false };
    std::atomic<int> activeLatencySamples { 0 };
    std::atomic<double> lastDetectUs { 0.0 };
    std::atomic<double> lastQuantizeUs { 0.0 };
    std::atomic<double> lastShiftUs { 0.0 };
    std::atomic<float> lastDetectedHz { 0.0f };
    std::atomic<float> lastSemitoneShift { 0.0f };

    ScopeCapture scopeCapture;

    // Same ownership-handoff shape as pendingEngine above (a raw pointer
    // the message thread releases into and only the worker thread ever
    // reclaims) -- see requestStartRecording()'s doc.
    std::atomic<juce::File*> pendingRecordingStart { nullptr };
    std::atomic<bool> pendingRecordingStop { false };
    AudioSessionRecorder audioRecorder;
};

} // namespace pitchzazz
