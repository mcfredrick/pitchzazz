// Two diagnostics for the crackle/beat artifact (docs/FINDINGS.md #19/#20),
// requested directly rather than inferred from a summary statistic:
//
// 1. A dropout/underrun scan: pure sine input (deliberately, NOT the
//    jittered voice signal PSOLACrackleProbe.cpp uses -- a pure sine can't
//    expose the bucket-jitter-mismatch mechanism at all, per that file's
//    own comment, which is exactly why it's the right, isolating signal
//    for a *different* question: does the basic overlap-add coverage ever
//    produce a literal zero-output gap, independent of jitter entirely?
//    accumulator slots start at 0.0f and only ever += -- a slot no grain
//    ever wrote to reads back as exact 0.0f, so a run of exact zeros in a
//    sustained-tone's output (past warm-up) is a direct, unambiguous
//    signal of a coverage gap, no internal accessor needed.
// 2. Raw input/output waveform CSV dumps (pure sine at a stress shift, and
//    the jittered voice signal at a shift with frequent bucket
//    transitions) for direct visual inspection -- a statistic can miss a
//    shape a human eye catches immediately, and this makes that possible.

#include <DSP/PSOLAPitchShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float baseFreq = 220.0f;

    std::vector<float> pureSine (int totalSamples)
    {
        std::vector<float> out ((size_t) totalSamples);
        for (int i = 0; i < totalSamples; ++i)
        {
            const float t = (float) i / (float) sampleRate;
            out[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * baseFreq * t);
        }
        return out;
    }

    // Same generator as PSOLACrackleProbe.cpp -- phase-integrated genuine
    // period jitter, not amplitude tremolo.
    std::vector<float> jitteredVoice (int totalSamples)
    {
        constexpr float jitterDepth = 0.02f;
        constexpr float jitterRate1 = 11.3f;
        constexpr float jitterRate2 = 17.7f;

        std::vector<float> out ((size_t) totalSamples);
        double phase = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            const double wobble = std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate1 * t) * 0.6
                                 + std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate2 * t) * 0.4;
            const double instFreq = (double) baseFreq * (1.0 + (double) jitterDepth * wobble);
            phase += 2.0 * juce::MathConstants<double>::pi * instFreq / sampleRate;
            out[(size_t) i] = (float) std::sin (phase);
        }
        return out;
    }

    std::vector<float> render (const std::vector<float>& input, float semitoneShift)
    {
        PSOLAPitchShifter shifter (sampleRate);
        std::vector<float> output (input.size(), 0.0f);
        const int totalBlocks = (int) (input.size() / (size_t) blockSize);
        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            std::vector<float> out (blockSize, 0.0f);
            shifter.shiftPitch (baseFreq, semitoneShift, in, out);
            std::copy (out.begin(), out.end(), output.begin() + block * blockSize);
        }
        return output;
    }
}

TEST_CASE ("PSOLA dropout scan: exact-zero runs in a sustained pure-sine output, wide shift sweep", "[psola][dropout]")
{
    constexpr int totalBlocks = 20;
    const auto input = pureSine (blockSize * totalBlocks);

    std::cout << "\nPure 220Hz sine, dropout scan (exact-zero run length past warm-up):\n";
    for (float shift : { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -15.0f, -18.0f, -24.0f })
    {
        const auto output = render (input, shift);
        PSOLAPitchShifter probe (sampleRate); // just to read getLatencySamples() with matching construction
        const int latency = probe.getLatencySamples();
        const size_t warmupEnd = (size_t) latency + (size_t) blockSize; // past the fill-up region, generous

        int longestZeroRun = 0, currentRun = 0;
        size_t longestRunStart = 0, runStart = 0;
        for (size_t i = warmupEnd; i < output.size(); ++i)
        {
            if (output[i] == 0.0f)
            {
                if (currentRun == 0)
                    runStart = i;
                ++currentRun;
                if (currentRun > longestZeroRun)
                {
                    longestZeroRun = currentRun;
                    longestRunStart = runStart;
                }
            }
            else
            {
                currentRun = 0;
            }
        }

        const double longestRunMs = (double) longestZeroRun / sampleRate * 1000.0;
        std::cout << shift << "st: longest exact-zero run = " << longestZeroRun << " samples (" << longestRunMs
                   << "ms) starting at sample " << longestRunStart << " (of " << output.size() << " analyzed)\n";
    }
    std::cout << std::endl;
}

TEST_CASE ("PSOLA mid-stream shift change: does changing semitoneShift cause a discontinuity?", "[psola][shift-change]")
{
    // Every other render in this file uses a *constant* shift from sample
    // zero -- none test what real use actually looks like: one long-lived
    // shifter object, semitoneShift changing block to block as the
    // detected/quantized target moves, buffers already warm. This
    // directly checks whether the shift value itself changing mid-stream
    // (not a cold construction) produces a discontinuity, and if so, how
    // it compares to the cold-start transient found elsewhere.
    PSOLAPitchShifter shifter (sampleRate);
    const auto input = jitteredVoice (blockSize * 20);
    std::vector<float> output (input.size(), 0.0f);

    const int totalBlocks = (int) (input.size() / (size_t) blockSize);
    const int changeAtBlock = totalBlocks / 2; // shift changes exactly at this block boundary
    for (int block = 0; block < totalBlocks; ++block)
    {
        const float shift = (block < changeAtBlock) ? 0.0f : 1.0f;
        std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
        std::vector<float> out (blockSize, 0.0f);
        shifter.shiftPitch (baseFreq, shift, in, out);
        std::copy (out.begin(), out.end(), output.begin() + block * blockSize);
    }

    const int changeAtSample = changeAtBlock * blockSize;
    std::cout << "\nMid-stream shift change (0st -> +1st at input sample " << changeAtSample << "):\n";

    // Scan the whole render for outlier jumps, same 0.045 threshold used
    // for the fixed-shift renders, and report proximity to the actual
    // change point specifically.
    constexpr float threshold = 0.045f;
    int nearChangeCount = 0, farFromChangeCount = 0;
    float maxJump = 0.0f; int maxJumpPos = -1;
    for (size_t i = 1; i < output.size(); ++i)
    {
        const float d = std::abs (output[i] - output[(size_t) (i - 1)]);
        if (d > maxJump) { maxJump = d; maxJumpPos = (int) i; }
        if (d > threshold)
        {
            const int distFromChange = std::abs ((int) i - changeAtSample);
            if (distFromChange < 100)
                ++nearChangeCount;
            else
                ++farFromChangeCount;
        }
    }
    std::cout << "  jumps >" << threshold << " within 100 samples of the change point: " << nearChangeCount << "\n";
    std::cout << "  jumps >" << threshold << " elsewhere in the render: " << farFromChangeCount << "\n";
    std::cout << "  single largest jump: " << maxJump << " at sample " << maxJumpPos
               << " (" << (maxJumpPos - changeAtSample) << " samples from the change point)\n";

    std::ofstream f ("/tmp/psola_midstream_shift_change.csv");
    f << "sample,input,output\n";
    for (size_t i = 0; i < input.size(); ++i)
        f << i << "," << input[i] << "," << output[i] << "\n";
    std::cout << "  Wrote /tmp/psola_midstream_shift_change.csv (" << input.size() << " rows)" << std::endl;
}

TEST_CASE ("PSOLA waveform dump: input/output CSV for direct visual inspection", "[psola][waveform-dump]")
{
    // Pure sine at -18st: past the ~-12st gap-onset boundary the earlier
    // background investigation derived (shiftRatio < 0.5 at
    // grainWidthMultiplier=1.0), deliberately well past it to make a real
    // gap, if the mechanism is real, unambiguous rather than borderline.
    {
        const auto input = pureSine (blockSize * 20);
        const auto output = render (input, -18.0f);

        std::ofstream f ("/tmp/psola_pure_sine_-18st.csv");
        f << "sample,input,output\n";
        for (size_t i = 0; i < input.size(); ++i)
            f << i << "," << input[i] << "," << output[i] << "\n";
        std::cout << "\nWrote /tmp/psola_pure_sine_-18st.csv (" << input.size() << " rows)\n";
    }

    // Jittered voice at +-1st: this plugin's *actual* operating range
    // (scale-correction never requests more than +-1.5 semitones,
    // docs/FINDINGS.md #27) -- the user reports the artifact is almost
    // always present at real playing shifts, which the +12st render this
    // file originally used never actually exercised. At +1st,
    // shiftRatio=2^(1/12)=1.0595, so consecutive marks' bucket index
    // advances by 1/1.0595=0.9438 per mark on average -- a "reuse" event
    // (bucket delta 0, not the normal 1) roughly once every ~17.8 marks.
    // At a ~200-sample period (220Hz), that's roughly once every 3560
    // samples, ~81ms, ~12Hz -- squarely in low-frequency-*beat* territory
    // (a periodic wobble), not sparse/occasional clicking. That would
    // explain "almost always present" even though PSOLACrackleProbe's
    // per-mark statistic (built around individual-mark roughness, not a
    // repetition *rate*) didn't flag it.
    for (float shift : { -1.0f, 1.0f })
    {
        const auto input = jitteredVoice (blockSize * 20);
        const auto output = render (input, shift);

        const std::string filename = "/tmp/psola_jittered_voice_" + std::string (shift > 0 ? "+" : "")
                                    + std::to_string ((int) shift) + "st.csv";
        std::ofstream f (filename);
        f << "sample,input,output\n";
        for (size_t i = 0; i < input.size(); ++i)
            f << i << "," << input[i] << "," << output[i] << "\n";
        std::cout << "Wrote " << filename << " (" << input.size() << " rows)\n";
    }

    // Pure sine at +-1st: the control case. Adjacent analysis buckets of a
    // stationary sine are bit-identical, so a bucket-reuse/skip transition
    // replays literally the same content -- should be seamless *by
    // construction*, no jitter-mismatch mechanism possible. If this isn't
    // clean, something beyond the jitter-mismatch theory is going on, at
    // the same realistic shift amount the jittered-voice renders use.
    for (float shift : { -1.0f, 1.0f })
    {
        const auto input = pureSine (blockSize * 20);
        const auto output = render (input, shift);

        const std::string filename = "/tmp/psola_pure_sine_" + std::string (shift > 0 ? "+" : "")
                                    + std::to_string ((int) shift) + "st.csv";
        std::ofstream f (filename);
        f << "sample,input,output\n";
        for (size_t i = 0; i < input.size(); ++i)
            f << i << "," << input[i] << "," << output[i] << "\n";
        std::cout << "Wrote " << filename << " (" << input.size() << " rows)\n";
    }
    std::cout << std::endl;
}
