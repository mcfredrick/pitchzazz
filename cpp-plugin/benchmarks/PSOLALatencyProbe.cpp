// Measures PSOLAPitchShifter's *actual* delay via a silence-then-tone onset
// — deliberately not LatencyProbe.cpp's impulse-peak methodology (a single-
// sample impulse has no meaningful detected period, so it wouldn't exercise
// PSOLA's real pitch-dependent mark-firing behavior), and deliberately not
// plain cross-correlation of a continuous tone either: a sine wave
// correlates strongly with *itself* at every integer multiple of its own
// period, not just at the true causal delay, so a naive "best correlation
// lag" search over a continuous tone returns whichever near-tied periodic
// peak floating-point noise favors — found by actually running that version
// first and getting inconsistent, physically-implausible numbers back
// (e.g. a lower detected pitch measuring a *shorter* delay than a higher
// one), not assumed. An unambiguous onset (silence, then a sustained tone)
// sidesteps the periodicity ambiguity entirely: there's exactly one real
// transition to find.
//
// The onset tone uses cos(), not sin(): an earlier version used sin(), whose
// value is exactly zero at the onset instant by construction — at a 1:1
// (zero semitone) shift, PSOLA's read position equals its write position,
// so the accumulator slot for that exact sample is *correctly* zero
// regardless of latency, not under-reported. That produced a confusing,
// hard-to-diagnose false "overshoot" that took real instrumentation to root-
// cause (see PSOLAPitchShifter.cpp's latencySamples comment for the full
// story). Starting at peak amplitude instead removes the ambiguity: no
// meaningful sample in the test tone is ever exactly zero.

#include <DSP/PSOLAPitchShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <iostream>

using namespace pitchzazz;

namespace
{
    void probeAt (float freq, double sampleRate)
    {
        constexpr int blockSize = 2048;
        constexpr int totalBlocks = 8;
        const int onsetSample = blockSize * 2; // input tone starts partway in, well past startup

        PSOLAPitchShifter shifter (sampleRate);

        std::vector<float> fullOutput;
        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> input ((size_t) blockSize, 0.0f);
            for (int i = 0; i < blockSize; ++i)
            {
                const int globalIndex = block * blockSize + i;
                if (globalIndex >= onsetSample)
                {
                    const float t = (float) (globalIndex - onsetSample) / (float) sampleRate;
                    input[(size_t) i] = std::cos (2.0f * juce::MathConstants<float>::pi * freq * t);
                }
            }

            std::vector<float> output (blockSize, 0.0f);
            // detectedHz is fed directly (this is a controlled probe of the
            // shifter alone, not the full detect->shift pipeline) so the
            // period estimate is correct even during the silent prefix,
            // matching how the real Corrector always has a detected pitch
            // available by the time it calls shiftPitch().
            shifter.shiftPitch (freq, 0.0f, input, output); // 0 shift: output should track input, just delayed
            fullOutput.insert (fullOutput.end(), output.begin(), output.end());
        }

        // First sample where the output is meaningfully non-zero.
        constexpr float threshold = 1.0e-3f;
        int onsetIndex = -1;
        for (size_t i = 0; i < fullOutput.size(); ++i)
        {
            if (std::abs (fullOutput[i]) > threshold)
            {
                onsetIndex = (int) i;
                break;
            }
        }

        REQUIRE (onsetIndex >= 0);

        const int claimedLatency = shifter.getLatencySamples();
        const int measuredLatency = onsetIndex - onsetSample;
        const double measuredMs = (double) measuredLatency / sampleRate * 1000.0;
        const double claimedMs = (double) claimedLatency / sampleRate * 1000.0;

        std::cout << freq << "Hz @ " << (int) sampleRate << "Hz: measured " << measuredLatency
                   << " samples (" << measuredMs << "ms) vs. claimed worst-case " << claimedLatency
                   << " samples (" << claimedMs << "ms)" << std::endl;

        // The measured empirical delay should land at or under the
        // reported (worst-case) latency -- reporting more than reality is
        // the safe direction (see getLatencySamples()'s doc), reporting
        // less would be a real correctness bug (an under-informed host
        // would under-compensate).
        CHECK (measuredLatency <= claimedLatency);
        CHECK (measuredLatency > 0);
    }
}

TEST_CASE ("PSOLA pipeline latency probe", "[latency][psola]")
{
    // 220Hz: this project's standard test tone (matches
    // PitchDetectorTests.cpp, PitchShifterTests.cpp, etc.) -- the
    // realistic-case number. 60Hz: this shifter's own worst-case floor
    // (minHz in PSOLAPitchShifter.h) -- should measure close to the
    // reported claimedLatency itself, confirming the worst case is real,
    // not just a formula.
    for (double sampleRate : { 44100.0, 48000.0 })
    {
        probeAt (220.0f, sampleRate);
        probeAt (60.0f, sampleRate);
    }
}
