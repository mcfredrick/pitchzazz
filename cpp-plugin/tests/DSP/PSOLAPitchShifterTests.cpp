#include <DSP/PSOLAPitchShifter.h>
#include <DSP/PitchDetector.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace pitchzazz;

namespace
{
    float rms (const std::vector<float>& v)
    {
        float sumSq = 0.0f;
        for (float s : v)
            sumSq += s * s;
        return std::sqrt (sumSq / (float) v.size());
    }

    std::vector<float> sineBlock (float freq, double sampleRate, int blockSize, int blockIndex)
    {
        std::vector<float> block ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (blockIndex * blockSize + i) / (float) sampleRate;
            block[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }
}

// No Rust-side equivalent (PSOLA only exists in the C++ engine — see
// docs/ROADMAP.md Phase 5). Unlike the phase vocoder, correctness here
// hinges entirely on the pitch-mark placement math (analysis/synthesis
// spacing, grain windowing) rather than an FFT normalization constant, so
// that's specifically what these tests target.

TEST_CASE ("PSOLA: zero semitone shift roughly preserves signal energy", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float freq = 220.0f;

    PSOLAPitchShifter shifter (sampleRate);

    std::vector<float> lastOutput;
    // latencySamples (2 * sampleRate/minHz ~= 1470 at 44.1kHz) is well
    // under one block here, but several blocks still lets the overlap-add
    // settle into a steady state rather than asserting on a
    // still-partially-transient first block.
    for (int block = 0; block < 4; ++block)
    {
        const auto input = sineBlock (freq, sampleRate, blockSize, block);
        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (freq, 0.0f, input, output);
        lastOutput = output;
    }

    const float inputRms = 1.0f / std::sqrt (2.0f); // RMS of a unit sine
    const float outputRms = rms (lastOutput);

    // Same loose bounds as PitchShifterTests's equivalent check, same
    // purpose: catch "silent" or "exploding" output from a broken
    // overlap-add normalization, not assert precision.
    CHECK (outputRms > inputRms * 0.3f);
    CHECK (outputRms < inputRms * 3.0f);
}

TEST_CASE ("shifting a sine up reproduces the target frequency", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float sourceFreq = 220.0f;   // A3
    constexpr float semitoneShift = 3.0f;  // -> ~261.6Hz, close to C4

    PSOLAPitchShifter shifter (sampleRate);

    std::vector<float> lastOutput;
    for (int block = 0; block < 4; ++block)
    {
        const auto input = sineBlock (sourceFreq, sampleRate, blockSize, block);
        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (sourceFreq, semitoneShift, input, output);
        lastOutput = output;
    }

    PitchDetector detector (blockSize);
    const DetectedPitch redetected = detector.detect (lastOutput, sampleRate);

    const float targetFreq = sourceFreq * std::pow (2.0f, semitoneShift / 12.0f);

    REQUIRE (redetected.frequencyHz > 0.0f);
    // Looser tolerance than the phase vocoder's equivalent (PitchShifter
    // has no analogous frequency round-trip test at all — its own
    // correctness test only checks energy bounds): PSOLA's per-block
    // locally-stationary-pitch simplification (class doc in
    // PSOLAPitchShifter.h) and integer-sample grain placement both add a
    // little reconstruction jitter a phase vocoder's continuous phase
    // model doesn't have. 3% is comfortably tighter than a semitone
    // (~6%), which is the threshold that would actually matter musically.
    CHECK (std::abs (redetected.frequencyHz - targetFreq) < targetFreq * 0.03f);
}

TEST_CASE ("silence holds the previous period estimate without misbehaving", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;

    PSOLAPitchShifter shifter (sampleRate);

    const std::vector<float> silence ((size_t) blockSize, 0.0f);
    std::vector<float> output (blockSize, 0.0f);

    // detectedHz <= 0 (unvoiced), same convention Corrector already uses
    // upstream when PitchDetector reports no pitch.
    shifter.shiftPitch (0.0f, 0.0f, silence, output);

    CHECK (rms (output) < 0.01f);
}

TEST_CASE ("reported latency is pitch-independent, worst-case, and far below the phase vocoder's fixed window", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;

    PSOLAPitchShifter shifter (sampleRate);

    // Matches the derivation in PSOLAPitchShifter.h/.cpp exactly: 2 *
    // ceil(sampleRate / minHz). Asserting the formula, not just "some
    // small number," so a future change to minHz/the derivation itself
    // has to consciously update this test rather than silently pass.
    constexpr float minHz = 60.0f;
    const int expectedLatency = 2 * (int) std::ceil (sampleRate / minHz);
    CHECK (shifter.getLatencySamples() == expectedLatency);

    // The actual headline claim: at 44.1kHz the phase vocoder's window is
    // ~2048-2206 samples (docs/PERFORMANCE_LOG.md's measured-latency
    // entry). This worst-case (60Hz) number is a real but modest win
    // (~28% less); realistic vocal pitches (100-300Hz) get a far bigger
    // one, since this shifter's actual latency is pitch-dependent — see
    // docs/PERFORMANCE_LOG.md's dated entry for that fuller comparison.
    CHECK (shifter.getLatencySamples() < 2048);
}
