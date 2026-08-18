#pragma once

#include "DSP/CorrectorWorker.h"
#include "DSP/PitchEngineRegistry.h"
#include "DSP/RetuneSmoothing.h"
#include "DSP/Scale.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

//==============================================================================
/**
    Real-time audio plumbing lives here, mirroring pitch-cli/src/main.rs
    (see docs/ARCHITECTURE.md): processBlock downmixes to mono and pushes
    into a lock-free input FIFO, a background CorrectorWorker accumulates
    fixed-size blocks and runs pitch detection/quantization/shifting, and
    processBlock reads whatever's ready from a lock-free output FIFO,
    up-mixing back to the host's channel count. Underruns fill with
    silence rather than blocking — the same real-time-safe failure mode
    pitch-cli's output callback uses.

    Nothing in processBlock may block, allocate, or take a lock — see
    docs/ARCHITECTURE.md's real-time-safety section, which this class is
    the C++ side of. juce::AbstractFifo was audited for this before use
    (atomic-index based, no internal mutex — see docs/ROADMAP.md Phase 2's
    Day 3 entry), the same way pitch-cli's ARCHITECTURE.md audits ringbuf.
*/
class PitchzazzAudioProcessor : public juce::AudioProcessor
{
public:
    PitchzazzAudioProcessor();
    ~PitchzazzAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    // Called from the editor (message thread) only — never from processBlock.
    void setScale (pitchzazz::Scale newScale);
    pitchzazz::Scale getScale() const noexcept { return currentScale; }
    void setBypassed (bool shouldBypass) noexcept { bypassed.store (shouldBypass, std::memory_order_relaxed); }
    bool isBypassed() const noexcept { return bypassed.load (std::memory_order_relaxed); }

    /// The classic Auto-Tune controls (docs/ROADMAP.md Phase 5) — forwarded
    /// to the worker exactly like setScale above. Clamped by the DSP layer
    /// itself (Corrector::setCorrectionAmount etc.), not just here, but
    /// clamping at this boundary too means getCorrectionAmount()/
    /// getRetuneSpeedMs() always reflect a valid value even before
    /// prepareToPlay has constructed a worker to clamp on its behalf.
    void setCorrectionAmount (float amount) noexcept;
    float getCorrectionAmount() const noexcept { return currentCorrectionAmount; }
    void setRetuneSpeedMs (float speedMs) noexcept;
    float getRetuneSpeedMs() const noexcept { return currentRetuneSpeedMs; }

    /// Whether the active engine implements the controls above — see
    /// CorrectorWorker::getActiveSupportsRetuneControls's doc. False (not
    /// true) before prepareToPlay has run, same "no worker yet" default
    /// every other active-engine getter here uses.
    bool activeEngineSupportsRetuneControls() const noexcept;

    /// Hot-swaps the active pitch-correction engine (docs/ROADMAP.md
    /// Phase 3) — `engineId` must be one of pitchzazz::availableEngines()'
    /// ids. Constructs the new engine here (message thread, not
    /// real-time-constrained) and hands it to the worker via
    /// CorrectorWorker::requestEngineSwap(); see that class's doc comment
    /// for why constructing before handoff, rather than during, is what
    /// keeps this dropout-free.
    void setEngine (const std::string& engineId);
    const std::vector<pitchzazz::EngineDescriptor>& getAvailableEngines() const { return pitchzazz::availableEngines(); }
    juce::String getActiveEngineName() const noexcept;

    /// For the GUI's latency/processing-time display (docs/ROADMAP.md
    /// Phase 4) — all 0 before prepareToPlay has run. getActiveLatencyMs
    /// is the pipeline latency (docs/PERFORMANCE_LOG.md's measured
    /// entry), NOT the same quantity as the processing-time getters below
    /// — see PitchEngine::getLatencySamples's doc for why these don't sum
    /// with anything else to produce it.
    double getActiveLatencyMs() const noexcept;
    double getLastDetectUs() const noexcept;
    double getLastQuantizeUs() const noexcept;
    double getLastShiftUs() const noexcept;

    /// Most recent detected pitch (0 if unvoiced/silent) and the
    /// semitone shift applied to correct it, for the GUI's live pitch
    /// display — same all-0-before-prepareToPlay and eventual-consistency
    /// contract as the getters above.
    float getLastDetectedHz() const noexcept;
    float getLastSemitoneShift() const noexcept;

    /// The real-time processing budget for one block, in microseconds —
    /// for the GUI's segmented processing-time meter, so it can show
    /// stage cost relative to the budget, not just relative to itself.
    double getBudgetUs() const noexcept;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Path helpers - macOS Application Support paths (no permission prompts)
    static juce::File getApplicationSupportPath();
    static juce::File getSamplesPath();
    static juce::File getPresetsPath();
    static juce::File getUserDataPath();
    static juce::File getLogsPath();

private:
    // Matches pitch-cli's BLOCK_SIZE/RING_BUFFER_SECONDS/startup-latency
    // constants exactly (crates/pitch-cli/src/main.rs) — same analysis
    // block size, same ring-buffer sizing rationale.
    static constexpr int correctorBlockSize = 2048;
    static constexpr float ringBufferSeconds = 1.0f;
    static constexpr float startupLatencyMs = 150.0f;
    static constexpr int windowSizeMs = 50;

    std::unique_ptr<juce::AbstractFifo> inputFifo, outputFifo;
    std::vector<float> inputRingBuffer, outputRingBuffer;
    std::unique_ptr<pitchzazz::CorrectorWorker> worker;

    pitchzazz::Scale currentScale;
    std::atomic<bool> bypassed { false };
    std::string currentEngineId;
    float currentCorrectionAmount = pitchzazz::correctionAmountMax;
    float currentRetuneSpeedMs = pitchzazz::retuneSpeedMsMin;

    // juce::Thread asserts (UB) if destroyed while still running. Hosts
    // aren't guaranteed to call releaseResources() before destroying the
    // processor — auval doesn't — so stopWorker() is called from three
    // places: prepareToPlay() (before resizing the FIFOs the worker holds
    // references into), releaseResources(), and the destructor itself.
    // Found via auval, not by inspection: see docs/ROADMAP.md Phase 2's
    // Day 4 entry.
    void stopWorker();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchzazzAudioProcessor)
};
