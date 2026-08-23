#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace pitchzazz
{

/// Lock-free snapshot of the most recent input/output samples, for the
/// GUI's live before/after scope (docs/ROADMAP.md's "one-period waveform
/// visualization" item). Written once per block by CorrectorWorker::run()
/// — no real-time deadline there, but no reason to introduce a lock even
/// so — and read at its own pace by a UI-side Timer on the message
/// thread. `count` is only ever meaningfully read *outside* this class
/// when `read()` returns true, which only happens once the generation
/// check below has confirmed the payload wasn't touched mid-copy.
///
/// Seqlock, not a mutex or a ring buffer: the writer bumps an odd
/// "in-progress" generation, writes both arrays, then bumps to the next
/// even "stable" generation. The reader takes a generation snapshot,
/// copies the payload, then re-checks the generation; if it changed (a
/// write landed mid-copy) or was odd to begin with, the reader discards
/// what it just copied and tries again next tick — cheap and lossless in
/// practice at a fixed ~20-30Hz UI refresh against a ~21.5Hz block rate
/// (44.1kHz / 2048), where a genuine collision is rare and a dropped
/// frame is inaudible and invisible (the next tick just shows the same
/// picture one block later). Never blocks either side, unlike a mutex —
/// this project's whole real-time-safety stance (docs/ARCHITECTURE.md)
/// treats "never block the producer" as non-negotiable even here, where
/// the producer is a worker thread with no hard deadline, not the audio
/// callback itself.
class ScopeCapture
{
public:
    // > one full period down to ~55Hz @ 44.1kHz (period ~801 samples) --
    // comfortably below any real vocal fundamental this plugin targets
    // (PSOLAPitchShifter.h's own minHz floor is 80Hz).
    static constexpr int captureLength = 800;

    /// Called from CorrectorWorker::run() once per block with that
    /// block's full input (the analysis buffer) and final output (post
    /// crossfade-blend, if a hot-swap is in progress) -- only the last
    /// `captureLength` samples of each are kept, the same "just show what
    /// just happened" framing the other live meters already use.
    void write (const float* input, const float* output, int blockSize) noexcept
    {
        const int n = blockSize < captureLength ? blockSize : captureLength;
        const int offset = blockSize - n;

        generation.fetch_add (1, std::memory_order_acq_rel); // now odd: in progress
        for (int i = 0; i < n; ++i)
        {
            inputBuf[(size_t) i] = input[offset + i];
            outputBuf[(size_t) i] = output[offset + i];
        }
        count = n;
        generation.fetch_add (1, std::memory_order_acq_rel); // now even: stable
    }

    /// Returns false (out parameters left untouched) if a write was in
    /// progress or landed mid-copy -- the caller should just try again on
    /// its next tick rather than treat this as an error.
    [[nodiscard]] bool read (std::array<float, captureLength>& outInput,
                              std::array<float, captureLength>& outOutput,
                              int& outCount) const noexcept
    {
        const uint32_t g1 = generation.load (std::memory_order_acquire);
        if ((g1 & 1u) != 0u)
            return false;

        std::array<float, captureLength> tmpIn {}, tmpOut {};
        const int n = count;
        for (int i = 0; i < n; ++i)
        {
            tmpIn[(size_t) i] = inputBuf[(size_t) i];
            tmpOut[(size_t) i] = outputBuf[(size_t) i];
        }

        const uint32_t g2 = generation.load (std::memory_order_acquire);
        if (g1 != g2)
            return false;

        outInput = tmpIn;
        outOutput = tmpOut;
        outCount = n;
        return true;
    }

private:
    std::atomic<uint32_t> generation { 0 };
    std::array<float, captureLength> inputBuf {};
    std::array<float, captureLength> outputBuf {};
    int count = 0;
};

} // namespace pitchzazz
