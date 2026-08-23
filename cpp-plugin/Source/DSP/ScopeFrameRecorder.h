#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <memory>

namespace pitchzazz
{

/// Background PNG-sequence writer for the scope-recording feature -- fed
/// one frame at a time from ScopeComponent's existing 30Hz UI Timer
/// (message thread, see pushFrame()'s doc), encodes and writes to disk on
/// its own thread so a slow PNG encode/disk write never stalls the
/// message thread (which the rest of this editor's live meters also
/// depend on staying responsive).
///
/// Uses a plain juce::CriticalSection, not a lock-free structure like
/// ScopeCapture.h's seqlock -- this is diagnostic tooling driven entirely
/// by a UI Timer, not the audio path, so the lock-free discipline this
/// project applies everywhere real-time-adjacent doesn't buy anything
/// here, just unnecessary complexity for a rare (30Hz, tiny image),
/// non-deadline-bound handoff.
///
/// Only ever holds the single most-recently-pushed, not-yet-written frame:
/// if the writer thread ever falls behind a 30Hz producer (it shouldn't --
/// a PNG this small should encode in well under the 33ms budget, but a
/// slow disk could stall it) it drops older frames rather than building an
/// unbounded backlog. That's fine for this use: scripts/stitch_recording.sh
/// times the resulting video off each frame's actual write time in
/// frames_manifest.csv, not its sequence position, so a drop just means a
/// (correctly timed) slightly longer hold on the previous frame rather
/// than any audio/video desync.
class ScopeFrameRecorder : private juce::Thread
{
public:
    ScopeFrameRecorder() : juce::Thread ("Pitchzazz Frame Recorder") {}
    ~ScopeFrameRecorder() override { stop(); }

    /// `sessionDir` must already exist. Call from the message thread only
    /// (matches pushFrame()'s/stop()'s contract).
    bool start (const juce::File& sessionDir)
    {
        stop();

        manifestStream = std::make_unique<juce::FileOutputStream> (sessionDir.getChildFile ("frames_manifest.csv"));
        if (! manifestStream->openedOk())
        {
            manifestStream.reset();
            return false;
        }
        manifestStream->writeText ("frame,elapsed_ms,filename\n", false, false, nullptr);

        outputDir = sessionDir;
        frameIndex = 0;
        startedAtMs = juce::Time::getMillisecondCounterHiRes();

        recording.store (true, std::memory_order_relaxed);
        startThread();
        return true;
    }

    /// Safe to call even if start() was never called or already stopped.
    void stop()
    {
        recording.store (false, std::memory_order_relaxed);
        signalThreadShouldExit();
        notify();
        stopThread (2000);
        manifestStream.reset();
    }

    /// Called from the message thread once per UI tick while recording --
    /// cheap (stashes the image and a timestamp under a lock, then wakes
    /// the writer thread) and never itself touches the disk.
    /// createComponentSnapshot() renders via the component's own paint(),
    /// so the pushed image is exactly what's on screen regardless of
    /// when the actual screen repaint happens.
    void pushFrame (const juce::Image& image)
    {
        if (! recording.load (std::memory_order_relaxed))
            return;
        const juce::ScopedLock sl (pendingLock);
        pendingImage = image;
        pendingElapsedMs = juce::Time::getMillisecondCounterHiRes() - startedAtMs;
        hasPending = true;
        notify();
    }

    bool isRecording() const noexcept { return recording.load (std::memory_order_relaxed); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            juce::Image frame;
            double elapsedMs = 0.0;
            {
                const juce::ScopedLock sl (pendingLock);
                if (hasPending)
                {
                    frame = pendingImage;
                    elapsedMs = pendingElapsedMs;
                    hasPending = false;
                }
            }

            if (! frame.isValid())
            {
                wait (15);
                continue;
            }

            const juce::String filename = "frame_" + juce::String (frameIndex++).paddedLeft ('0', 6) + ".png";
            juce::FileOutputStream out (outputDir.getChildFile (filename));
            if (out.openedOk())
            {
                juce::PNGImageFormat png;
                png.writeImageToStream (frame, out);
            }
            manifestStream->writeText (juce::String (frameIndex) + "," + juce::String (elapsedMs, 2) + "," + filename + "\n",
                                        false, false, nullptr);
        }
    }

    juce::File outputDir;
    std::atomic<bool> recording { false };
    int frameIndex = 0;
    double startedAtMs = 0.0;

    juce::CriticalSection pendingLock;
    juce::Image pendingImage;
    double pendingElapsedMs = 0.0;
    bool hasPending = false;

    std::unique_ptr<juce::FileOutputStream> manifestStream;
};

} // namespace pitchzazz
