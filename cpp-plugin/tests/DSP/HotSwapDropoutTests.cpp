// Implements the dropout-detection methodology planned in
// docs/TESTING.md's "Planned: hot-swap dropout detection" section — see
// that doc for the full rationale. Once these tests exist and pass, that
// section should be moved from "planned" to "already built".

#include <DSP/CorrectorWorker.h>
#include <DSP/PitchEngineRegistry.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int numBlocks = 20;
    constexpr int swapAfterBlock = 10;

    std::vector<float> continuousTone (int totalSamples)
    {
        std::vector<float> signal ((size_t) totalSamples);
        for (int i = 0; i < totalSamples; ++i)
            signal[(size_t) i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sampleRate);
        return signal;
    }

    // Feeds `numBlocks` blocks of a continuous tone through a real
    // CorrectorWorker, one block at a time, waiting for each block's
    // output before pushing the next — deterministic control over
    // exactly which block boundary a requested swap lands on, unlike
    // pushing the whole signal at once (LatencyProbe.cpp's approach,
    // fine for a one-shot latency measurement but not for pinning a swap
    // to a specific point). If `swapEngineId` is empty, no swap happens
    // — used to establish the pipeline's own natural baseline.
    std::vector<float> runPipeline (const std::string& initialEngineId, const std::string& swapEngineId)
    {
        const int totalSamples = blockSize * numBlocks;
        const auto input = continuousTone (totalSamples);

        const int ringCapacity = totalSamples + blockSize;
        std::vector<float> inputBuffer ((size_t) ringCapacity, 0.0f);
        std::vector<float> outputBuffer ((size_t) ringCapacity, 0.0f);
        juce::AbstractFifo inputFifo (ringCapacity);
        juce::AbstractFifo outputFifo (ringCapacity);

        const EngineConfig config { blockSize, sampleRate, 50, Scale { 0, ScaleMode::major } };
        auto initialEngine = createEngine (initialEngineId, config);
        REQUIRE (initialEngine != nullptr);

        CorrectorWorker worker (blockSize, sampleRate, std::move (initialEngine),
                                 inputFifo, inputBuffer, outputFifo, outputBuffer);
        worker.startThread();

        int outputExpected = 0;
        for (int block = 0; block < numBlocks; ++block)
        {
            int start1, size1, start2, size2;
            inputFifo.prepareToWrite (blockSize, start1, size1, start2, size2);
            REQUIRE (size1 + size2 == blockSize); // ring is sized generously enough that this can't partially fail

            const int offset = block * blockSize;
            if (size1 > 0)
                std::copy (input.begin() + offset, input.begin() + offset + size1, inputBuffer.begin() + start1);
            if (size2 > 0)
                std::copy (input.begin() + offset + size1, input.begin() + offset + size1 + size2, inputBuffer.begin() + start2);
            inputFifo.finishedWrite (size1 + size2);

            if (! swapEngineId.empty() && block + 1 == swapAfterBlock)
            {
                auto newEngine = createEngine (swapEngineId, config);
                REQUIRE (newEngine != nullptr);
                worker.requestEngineSwap (std::move (newEngine));
            }

            outputExpected += blockSize;
            for (int i = 0; i < 5000 && outputFifo.getNumReady() < outputExpected; ++i)
                juce::Thread::sleep (1);
            REQUIRE (outputFifo.getNumReady() >= outputExpected);
        }

        worker.stopThread (2000);

        std::vector<float> output;
        output.reserve ((size_t) totalSamples);
        int start1, size1, start2, size2;
        outputFifo.prepareToRead (outputFifo.getNumReady(), start1, size1, start2, size2);
        if (size1 > 0)
            output.insert (output.end(), outputBuffer.begin() + start1, outputBuffer.begin() + start1 + size1);
        if (size2 > 0)
            output.insert (output.end(), outputBuffer.begin() + start2, outputBuffer.begin() + start2 + size2);
        outputFifo.finishedRead (size1 + size2);

        return output;
    }

    float maxAdjacentDelta (const std::vector<float>& signal)
    {
        float maxDelta = 0.0f;
        for (size_t i = 1; i < signal.size(); ++i)
            maxDelta = std::max (maxDelta, std::abs (signal[i] - signal[i - 1]));
        return maxDelta;
    }
}

TEST_CASE ("Hot-swap: sample accounting is exact across an engine swap", "[hotswap]")
{
    const auto output = runPipeline ("native-cpp", "rust-ffi");
    // Nothing lost or duplicated in the handoff — the worker produces
    // exactly one output sample per input sample, swap or no swap.
    CHECK (output.size() == (size_t) (blockSize * numBlocks));
}

TEST_CASE ("Hot-swap: no discontinuity beyond the pipeline's own baseline", "[hotswap]")
{
    // Self-calibrated rather than an arbitrary fixed threshold: run the
    // identical signal through the identical pipeline with and without a
    // swap, and compare. A real dropout (corrupted sample, silence gap,
    // uncontrolled jump) shows up as a delta many times larger than
    // whatever the pipeline already naturally produces — a smooth
    // timbral transition as the new engine's internal window state
    // starts cold does not, since it unfolds over many samples rather
    // than as a single instantaneous jump. This is deliberately a click
    // detector, not a "the audio sounds different after swapping"
    // detector — swapping engines is *expected* to sound momentarily
    // different, that's not a bug.
    const auto baseline = runPipeline ("native-cpp", "");
    const auto swapped = runPipeline ("native-cpp", "rust-ffi");

    const float baselineMaxDelta = maxAdjacentDelta (baseline);
    const float swappedMaxDelta = maxAdjacentDelta (swapped);

    INFO ("baseline max adjacent-sample delta: " << baselineMaxDelta);
    INFO ("swapped max adjacent-sample delta: " << swappedMaxDelta);

    CHECK (swappedMaxDelta < baselineMaxDelta * 4.0f + 0.05f);
}

TEST_CASE ("Hot-swap: no unexplained silence beyond startup latency", "[hotswap]")
{
    const auto output = runPipeline ("native-cpp", "rust-ffi");

    // Skip the startup region where the pipeline's own FIFO latency
    // legitimately produces near-silence (docs/ARCHITECTURE.md's Latency
    // section, docs/PERFORMANCE_LOG.md's measured-latency entry) — this
    // test is about a gap appearing *after* startup, at the swap point.
    const size_t startupSamples = (size_t) blockSize;
    int consecutiveSilent = 0;
    int maxConsecutiveSilent = 0;
    for (size_t i = startupSamples; i < output.size(); ++i)
    {
        if (std::abs (output[i]) < 1e-6f)
            maxConsecutiveSilent = std::max (maxConsecutiveSilent, ++consecutiveSilent);
        else
            consecutiveSilent = 0;
    }

    INFO ("max consecutive near-silent samples after startup: " << maxConsecutiveSilent);
    CHECK (maxConsecutiveSilent < blockSize / 4);
}

// Varispeed is built from two independently-buffered variable-rate stages
// (WSOLA then resample — see VarispeedShifter.h), a more elaborate
// internal FIFO shape than either existing engine's single-stage
// buffering, so its hot-swap behavior isn't assumed to be covered by the
// native-cpp/rust-ffi cases above just because the mechanism looks
// similar on paper — verified explicitly, both directions.

TEST_CASE ("Hot-swap: sample accounting is exact when swapping into Varispeed", "[hotswap]")
{
    const auto output = runPipeline ("native-cpp", "varispeed-cpp");
    CHECK (output.size() == (size_t) (blockSize * numBlocks));
}

TEST_CASE ("Hot-swap: sample accounting is exact when swapping out of Varispeed", "[hotswap]")
{
    const auto output = runPipeline ("varispeed-cpp", "native-cpp");
    CHECK (output.size() == (size_t) (blockSize * numBlocks));
}

TEST_CASE ("Hot-swap: no discontinuity beyond baseline when swapping into Varispeed", "[hotswap]")
{
    const auto baseline = runPipeline ("native-cpp", "");
    const auto swapped = runPipeline ("native-cpp", "varispeed-cpp");

    const float baselineMaxDelta = maxAdjacentDelta (baseline);
    const float swappedMaxDelta = maxAdjacentDelta (swapped);

    INFO ("baseline max adjacent-sample delta: " << baselineMaxDelta);
    INFO ("swapped max adjacent-sample delta: " << swappedMaxDelta);

    CHECK (swappedMaxDelta < baselineMaxDelta * 4.0f + 0.05f);
}

TEST_CASE ("Hot-swap: no unexplained silence beyond startup latency when swapping into Varispeed", "[hotswap]")
{
    const auto output = runPipeline ("native-cpp", "varispeed-cpp");

    // Varispeed's own reported latency (~31ms) is higher than a single
    // block accumulation delay but still comfortably under one block at
    // this test's sample rate/block size (~46.4ms/block) — same startup
    // skip window as the other engines' equivalent test, not widened,
    // specifically so a latency regression that ate into that margin
    // would show up as a failure here rather than being silently
    // accommodated.
    const size_t startupSamples = (size_t) blockSize;
    int consecutiveSilent = 0;
    int maxConsecutiveSilent = 0;
    for (size_t i = startupSamples; i < output.size(); ++i)
    {
        if (std::abs (output[i]) < 1e-6f)
            maxConsecutiveSilent = std::max (maxConsecutiveSilent, ++consecutiveSilent);
        else
            consecutiveSilent = 0;
    }

    INFO ("max consecutive near-silent samples after startup: " << maxConsecutiveSilent);
    CHECK (maxConsecutiveSilent < blockSize / 4);
}
