#include <DSP/PSOLAPitchShifter.h>
#include <DSP/PitchDetector.h>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
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

    // A tremolo (slow amplitude modulation) on top of the carrier —
    // deliberately non-stationary, unlike sineBlock(). A pure stationary
    // sine repeats *exactly* every period, so placeGrainAt()'s analysis
    // buckets (each one period apart) always contain identical content
    // regardless of whether they're cross-faded or snapped to one —
    // meaning a plain sine test tone cannot expose a bucket-boundary
    // discontinuity bug at all, cross-fade or not. Real voice is never
    // perfectly stationary; this is the minimal signal that is.
    std::vector<float> tremoloSineBlock (float carrierFreq, float modFreq, float modDepth,
                                          double sampleRate, int blockSize, int blockIndex)
    {
        std::vector<float> block ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (blockIndex * blockSize + i) / (float) sampleRate;
            const float envelope = 1.0f + modDepth * std::sin (2.0f * juce::MathConstants<float>::pi * modFreq * t);
            block[(size_t) i] = envelope * std::sin (2.0f * juce::MathConstants<float>::pi * carrierFreq * t);
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
    // latencySamples (2 * sampleRate/minHz ~= 1104 at 44.1kHz) is well
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
    constexpr float minHz = 80.0f;
    const int expectedLatency = 2 * (int) std::ceil (sampleRate / minHz);
    CHECK (shifter.getLatencySamples() == expectedLatency);

    // The actual headline claim: at 44.1kHz the phase vocoder's window is
    // ~2048-2206 samples (docs/PERFORMANCE_LOG.md's measured-latency
    // entry). This is a *fixed* worst-case number, not an adaptive one —
    // it does not get smaller for higher detected pitches at runtime (see
    // getLatencySamples()'s doc for why: a host needs one constant value,
    // not one that varies block to block).
    CHECK (shifter.getLatencySamples() < 2048);
}

// Deliberately NOT a test asserting the crackle/beat artifact from
// docs/FINDINGS.md is fixed — it isn't, currently. A cross-fade between
// the two nearest analysis buckets was implemented specifically to fix
// it, and three different automated approaches were tried to verify that
// fix (external reference at the target frequency: meaningless, since
// the reference's arbitrary phase has no relationship to the shifter's
// own output phase; a stationary sine input: cannot expose the bug at
// all, since a perfectly periodic signal's analysis buckets are
// identical whether cross-faded or not; a statistical outlier ratio
// against a mildly-modulated tremolo signal: measured *lower* for the
// single-bucket version than the cross-faded one on one real run, the
// opposite of discriminating) — none of them reliably told the two
// versions apart. The cross-fade shipped anyway on the strength of the
// DSP reasoning, then was reverted after real listening found it didn't
// fix the artifact (and plausibly made it worse — likely comb filtering
// from blending two not-quite-phase-aligned grains, a known PSOLA
// pitfall without correlation-based alignment first; see
// docs/FINDINGS.md #19/#20 for the full account). Matches this project's
// own earlier precedent (finding #14): an automated metric passing and
// genuine perceptual quality are not always the same bar. What this
// test actually checks is the sanity floor every other shifter test in
// this file already uses: bounded, non-exploding output — worth keeping
// as a coarse regression guard even without a crackle-specific metric.
TEST_CASE ("shifting a non-stationary tone stays bounded (a wider net than a single stationary tone)", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float sourceFreq = 220.0f;
    constexpr float semitoneShift = 12.0f; // an octave up -- frequent bucket transitions, the case that produced the original crackle
    constexpr float modFreq = 6.0f;        // a plausible vibrato/tremolo rate -- slow relative to the carrier
    constexpr float modDepth = 0.15f;

    PSOLAPitchShifter shifter (sampleRate);

    std::vector<float> lastOutput;
    for (int block = 0; block < 6; ++block)
    {
        const auto input = tremoloSineBlock (sourceFreq, modFreq, modDepth, sampleRate, blockSize, block);
        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (sourceFreq, semitoneShift, input, output);
        lastOutput = output;
    }

    std::vector<float> deltas (lastOutput.size() - 1);
    for (size_t i = 1; i < lastOutput.size(); ++i)
        deltas[i - 1] = std::abs (lastOutput[i] - lastOutput[i - 1]);
    std::sort (deltas.begin(), deltas.end());

    const float medianDelta = deltas[deltas.size() / 2];
    const float maxDelta = deltas.back();

    // A loose bound (same spirit as the energy-preservation test above):
    // catches an output that's silent, exploding, or NaN-producing.
    // Measured ~4x on this exact signal with the cross-fade fix in
    // place, and ~4x with it temporarily reverted too — genuinely does
    // NOT discriminate the crackle regression (see this test's own
    // header comment for the full story), so 30x here is only a coarse
    // net, not a claim this ratio means "no crackle."
    CHECK (maxDelta < medianDelta * 30.0f);
}
