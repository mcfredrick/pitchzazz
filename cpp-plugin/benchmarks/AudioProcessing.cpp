// NOT a DSP-cost benchmark, despite the name — processBlock only pushes
// into / pops from lock-free FIFOs (see PluginProcessor.h's class doc);
// the actual pitch-detection/shift work happens asynchronously on
// CorrectorWorker's background thread, off this call stack entirely. What
// this *does* legitimately measure: that the real-time-critical path
// itself stays cheap regardless of DSP cost, which is the actual point of
// the FIFO/worker-thread architecture (docs/ARCHITECTURE.md). For the
// real per-block DSP cost — the comparable number for the Rust-vs-C++
// writeup — see CorrectorPerformance.cpp instead.

#include <PluginProcessor.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

TEST_CASE ("Audio buffer processing benchmark", "[benchmark]")
{
    PitchzazzAudioProcessor plugin;

    // Set up a realistic audio configuration
    constexpr int numChannels = 2;
    constexpr int blockSize = 512;
    constexpr double sampleRate = 48000.0;

    plugin.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (numChannels, blockSize);
    juce::MidiBuffer midi;

    BENCHMARK ("processBlock (stereo, 512 samples)")
    {
        // Fill with test signal
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < blockSize; ++i)
                buffer.setSample (ch, i, std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * i / static_cast<float> (sampleRate)));

        plugin.processBlock (buffer, midi);
        return buffer.getSample (0, 0); // prevent optimization
    };

    BENCHMARK ("processBlock (stereo, 1024 samples)")
    {
        juce::AudioBuffer<float> largeBuffer (numChannels, 1024);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < 1024; ++i)
                largeBuffer.setSample (ch, i, std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * i / static_cast<float> (sampleRate)));

        plugin.processBlock (largeBuffer, midi);
        return largeBuffer.getSample (0, 0);
    };

    plugin.releaseResources();
}
