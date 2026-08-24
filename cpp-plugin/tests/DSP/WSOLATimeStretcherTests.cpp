#include <DSP/PitchDetector.h>
#include <DSP/WSOLATimeStretcher.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;

    std::vector<float> sineBlock (float freq, int count, int startSampleIndex)
    {
        std::vector<float> block ((size_t) count);
        for (int i = 0; i < count; ++i)
        {
            const float t = (float) (startSampleIndex + i) / (float) sampleRate;
            block[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }

    float detectFrequency (const std::vector<float>& signal)
    {
        PitchDetector detector ((int) signal.size());
        return detector.detect (signal, sampleRate).frequencyHz;
    }

    // Pushes `totalInput` samples of a sine tone in fixed-size chunks,
    // pulling as much output as is available after each push, and
    // returns everything produced. Mirrors the drain-driven, push/pull
    // streaming pattern VarispeedShifter will actually use.
    std::vector<float> runStretch (float ratio, float freq, int totalInput)
    {
        WSOLATimeStretcher stretcher (sampleRate);
        constexpr int chunk = 512;
        std::vector<float> allOutput;

        int pushedSoFar = 0;
        while (pushedSoFar < totalInput)
        {
            const int thisChunk = std::min (chunk, totalInput - pushedSoFar);
            const auto block = sineBlock (freq, thisChunk, pushedSoFar);
            stretcher.push (block.data(), thisChunk);
            pushedSoFar += thisChunk;

            float out[chunk * 4];
            int produced = 0;
            do
            {
                produced = stretcher.pull (ratio, ratio, out, chunk * 4);
                allOutput.insert (allOutput.end(), out, out + produced);
            } while (produced > 0);
        }

        // Drain whatever's left once all input has been pushed (the
        // lookahead-bound final window(s) become available only once no
        // more input is coming — in real use VarispeedShifter would push
        // silence/hold rather than stopping, but for this test we just
        // drain what's reachable).
        float out[chunk * 4];
        int produced = 0;
        do
        {
            produced = stretcher.pull (ratio, ratio, out, chunk * 4);
            allOutput.insert (allOutput.end(), out, out + produced);
        } while (produced > 0);

        return allOutput;
    }
}

TEST_CASE ("ratio 1.0 preserves both duration and pitch", "[wsola-stretcher]")
{
    const auto output = runStretch (1.0f, 220.0f, 44100); // 1 second in

    // Output length should track input length closely — not exactly,
    // since the very last partial window's worth is never drainable
    // (nothing after it to prove it's finalized), but within a couple of
    // hops.
    INFO ("output size: " << output.size());
    CHECK (output.size() > 44100 - 4000);
    CHECK (output.size() < 44100 + 100);

    const float detected = detectFrequency (std::vector<float> (output.end() - 4096, output.end()));
    INFO ("detected: " << detected);
    CHECK (detected > 200.0f);
    CHECK (detected < 240.0f);
}

TEST_CASE ("ratio 2.0 roughly doubles duration without shifting pitch", "[wsola-stretcher]")
{
    const auto output = runStretch (2.0f, 220.0f, 44100);

    INFO ("output size: " << output.size());
    CHECK (output.size() > 44100 * 2 - 8000);
    CHECK (output.size() < 44100 * 2 + 8000);

    // Pitch must stay at the original frequency — WSOLA changes duration
    // only; any drift toward 440Hz would mean this stage is accidentally
    // doing what the resampler is supposed to do instead.
    const float detected = detectFrequency (std::vector<float> (output.end() - 4096, output.end()));
    INFO ("detected: " << detected);
    CHECK (detected > 200.0f);
    CHECK (detected < 240.0f);
}

TEST_CASE ("ratio 0.5 roughly halves duration without shifting pitch", "[wsola-stretcher]")
{
    const auto output = runStretch (0.5f, 220.0f, 44100);

    INFO ("output size: " << output.size());
    CHECK (output.size() > 44100 / 2 - 4000);
    CHECK (output.size() < 44100 / 2 + 4000);

    const float detected = detectFrequency (std::vector<float> (output.end() - 4096, output.end()));
    INFO ("detected: " << detected);
    CHECK (detected > 200.0f);
    CHECK (detected < 240.0f);
}

TEST_CASE ("pull returns nothing until a full window plus search margin is available", "[wsola-stretcher]")
{
    WSOLATimeStretcher stretcher (sampleRate);
    const auto block = sineBlock (220.0f, 64, 0); // far short of one window
    stretcher.push (block.data(), (int) block.size());

    float out[256];
    const int produced = stretcher.pull (1.0f, 1.0f, out, 256);
    CHECK (produced == 0);
}

TEST_CASE ("getLatencySamples scales with sample rate, not with ratio", "[wsola-stretcher]")
{
    WSOLATimeStretcher at44k (44100.0);
    WSOLATimeStretcher at48k (48000.0);

    // Higher sample rate -> more samples in the same ~25ms window+search,
    // so latency in *samples* should be larger, even though both
    // represent roughly the same real-world delay.
    CHECK (at48k.getLatencySamples() > at44k.getLatencySamples());

    // Latency is a construction-time property only — pull() never
    // changes it, regardless of what ratio gets requested. Exercise a
    // couple of different ratios and confirm the reported value is
    // unchanged (the whole point of stretching the original signal
    // before resampling rather than after).
    const int before = at44k.getLatencySamples();
    std::vector<float> junk (8192, 0.0f);
    at44k.push (junk.data(), (int) junk.size());
    float out[64];
    at44k.pull (2.0f, 2.0f, out, 64);
    at44k.pull (0.5f, 0.5f, out, 64);
    CHECK (at44k.getLatencySamples() == before);
}
