#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>

namespace pitchzazz
{

/// Writes the same per-block input/output audio CorrectorWorker already
/// hands to ScopeCapture out to two mono WAV files on disk -- the raw
/// material for the "record the scope as a video" workflow: this class
/// only ever writes audio, a sibling ScopeFrameRecorder (message thread)
/// writes the scope's video frames, and an offline script
/// (scripts/stitch_recording.sh) muxes the two after the fact.
/// Deliberately NOT real-time video encoding inside the app itself -- see
/// that script's header comment for why keeping video encoding out of
/// this worker thread was the right call even though the worker has no
/// hard real-time deadline of its own.
///
/// Owned by CorrectorWorker and only ever started/stopped/written to from
/// its background thread (see CorrectorWorker::run()'s pendingRecording*
/// handoff, the same "message thread requests, worker thread applies
/// between blocks" pattern requestEngineSwap() already uses) -- so unlike
/// ScopeCapture, this class does no cross-thread synchronization of its
/// own; it isn't safe to call from more than one thread concurrently, and
/// doesn't need to be.
class AudioSessionRecorder
{
public:
    /// `sessionDir` must already exist. Mono, 32-bit float WAV at
    /// `sampleRate` -- matches the plugin's own internal representation
    /// exactly, so no format conversion is needed on the write path here.
    bool start (const juce::File& sessionDir, double sampleRate)
    {
        stop();

        inputWriter = makeWriter (sessionDir.getChildFile ("input.wav"), sampleRate);
        outputWriter = makeWriter (sessionDir.getChildFile ("output.wav"), sampleRate);
        if (inputWriter == nullptr || outputWriter == nullptr)
        {
            inputWriter.reset();
            outputWriter.reset();
            return false;
        }

        recording.store (true, std::memory_order_relaxed);
        return true;
    }

    void writeBlock (const float* input, const float* output, int numSamples)
    {
        if (! recording.load (std::memory_order_relaxed))
            return;
        const float* inputChannels[] { input };
        const float* outputChannels[] { output };
        inputWriter->writeFromFloatArrays (inputChannels, 1, numSamples);
        outputWriter->writeFromFloatArrays (outputChannels, 1, numSamples);
    }

    void stop()
    {
        recording.store (false, std::memory_order_relaxed);
        inputWriter.reset();  // flushes and closes the underlying file
        outputWriter.reset();
    }

    /// Relaxed, not acquire/release: only ever read from the same worker
    /// thread that calls start()/stop()/writeBlock() (see class doc), so
    /// there's no cross-thread ordering to establish -- this is exposed
    /// purely so CorrectorWorker can answer getIsRecording() without a
    /// second, separately-tracked bool.
    bool isRecording() const noexcept { return recording.load (std::memory_order_relaxed); }

private:
    static std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::File& file, double sampleRate)
    {
        std::unique_ptr<juce::OutputStream> stream (new juce::FileOutputStream (file));
        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
            return nullptr;

        juce::WavAudioFormat wavFormat;
        // This overload (not the raw-pointer createWriterFor, deprecated)
        // takes ownership of `stream` itself only on success -- it's left
        // untouched on failure, so no manual release()/double-delete
        // bookkeeping is needed either way.
        return wavFormat.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                        .withSampleRate (sampleRate)
                                                        .withNumChannels (1)
                                                        .withBitsPerSample (32));
    }

    std::atomic<bool> recording { false };
    std::unique_ptr<juce::AudioFormatWriter> inputWriter, outputWriter;
};

} // namespace pitchzazz
