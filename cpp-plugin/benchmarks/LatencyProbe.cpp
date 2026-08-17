// Measures this engine's actual algorithmic pipeline latency — mirrors
// crates/pitch-cli/examples/latency_probe.rs exactly (same method, same
// block size, same sample rates) for a direct Rust-vs-C++ comparison.
// See that file's doc comment for the full methodology rationale
// (impulse-response-peak as a group-delay proxy, why startup-latency
// pre-fill and real audio-hardware I/O latency are deliberately excluded).
//
// Unlike the Rust probe, this drives the *real* CorrectorWorker thread
// rather than a synchronous stand-in — the sample-domain result (which
// sample index the impulse's peak lands on) is fully determined by the
// DSP math and FIFO ordering, not by wall-clock thread scheduling, so
// using the actual production worker thread doesn't add noise to the
// measurement and additionally exercises the real threaded path.

#include <DSP/CorrectorWorker.h>
#include <DSP/NativeCorrectorEngine.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>

using namespace pitchzazz;

namespace
{
    double measureLatencyMs (double sampleRate, int& outLatencySamples)
    {
        constexpr int blockSize = 2048;

        const int impulseIndex = blockSize;
        const int totalLen = blockSize * 12;
        std::vector<float> input ((size_t) totalLen, 0.0f);
        input[(size_t) impulseIndex] = 1.0f;

        const int ringCapacity = totalLen + blockSize;
        std::vector<float> inputBuffer ((size_t) ringCapacity, 0.0f);
        std::vector<float> outputBuffer ((size_t) ringCapacity, 0.0f);
        juce::AbstractFifo inputFifo (ringCapacity);
        juce::AbstractFifo outputFifo (ringCapacity);

        const EngineConfig engineConfig { blockSize, sampleRate, 50, Scale { 0, ScaleMode::major } };
        CorrectorWorker worker (blockSize, sampleRate, std::make_unique<NativeCorrectorEngine> (engineConfig),
                                 inputFifo, inputBuffer, outputFifo, outputBuffer);
        worker.startThread();

        // Push the whole probe signal in up front.
        int start1, size1, start2, size2;
        inputFifo.prepareToWrite (totalLen, start1, size1, start2, size2);
        if (size1 > 0)
            std::copy (input.begin(), input.begin() + size1, inputBuffer.begin() + start1);
        if (size2 > 0)
            std::copy (input.begin() + size1, input.begin() + size1 + size2, inputBuffer.begin() + start2);
        inputFifo.finishedWrite (size1 + size2);

        // Wait for the worker to produce as much output as it's going to
        // for this input (it stops advancing once fewer than blockSize
        // samples remain buffered) — poll rather than a fixed sleep,
        // bounded so a real regression fails fast instead of hanging.
        int stableCount = 0;
        int lastReady = -1;
        for (int i = 0; i < 2000 && stableCount < 20; ++i)
        {
            juce::Thread::sleep (1);
            const int ready = outputFifo.getNumReady();
            stableCount = (ready == lastReady) ? stableCount + 1 : 0;
            lastReady = ready;
        }

        worker.stopThread (1000);

        std::vector<float> output ((size_t) ringCapacity, 0.0f);
        outputFifo.prepareToRead (outputFifo.getNumReady(), start1, size1, start2, size2);
        if (size1 > 0)
            std::copy (outputBuffer.begin() + start1, outputBuffer.begin() + start1 + size1, output.begin());
        if (size2 > 0)
            std::copy (outputBuffer.begin() + start2, outputBuffer.begin() + start2 + size2, output.begin() + size1);
        outputFifo.finishedRead (size1 + size2);
        output.resize ((size_t) (size1 + size2));

        int peakIndex = 0;
        float peakValue = 0.0f;
        for (size_t i = 0; i < output.size(); ++i)
        {
            if (std::abs (output[i]) > peakValue)
            {
                peakValue = std::abs (output[i]);
                peakIndex = (int) i;
            }
        }

        outLatencySamples = juce::jmax (0, peakIndex - impulseIndex);
        return ((double) outLatencySamples / sampleRate) * 1000.0;
    }
}

TEST_CASE ("Pipeline latency probe", "[latency]")
{
    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        int latencySamples = 0;
        const double latencyMs = measureLatencyMs (sampleRate, latencySamples);

        const double blockAccumulationMs = (2048.0 / sampleRate) * 1000.0;
        std::cout << (int) sampleRate << "Hz: " << latencyMs << "ms (" << latencySamples
                   << " samples) -- block accumulation ~" << blockAccumulationMs
                   << "ms + phase vocoder window" << std::endl;

        // Sanity bounds, not a precision assertion -- the actual number
        // is the finding, this just catches a gross regression (e.g. the
        // probe finding nothing, or a runaway/unbounded delay).
        CHECK (latencyMs > 0.0);
        CHECK (latencyMs < 500.0);
    }
}
