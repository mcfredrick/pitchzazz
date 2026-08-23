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

TEST_CASE ("reported latency is pitch-independent and matches the worst-case formula exactly", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;

    PSOLAPitchShifter shifter (sampleRate);

    // Matches the derivation in PSOLAPitchShifter.h/.cpp exactly. Asserting
    // the formula, not just "some small number," so a future change to
    // minHz/grainWidthMultiplierMax/psolaAlignSearchRadius/the derivation
    // itself has to consciously update this test rather than silently
    // pass. This is a *fixed* worst-case number, not an adaptive one — it
    // does not get smaller for higher detected pitches (or a narrower
    // grain-width setting) at runtime (see getLatencySamples()'s doc for
    // why: a host needs one constant value, not one that varies block to
    // block).
    //
    // CHANGED (2026-08-23, feature/psola-crackle-latency): was
    // `2 * maxHalfWidthSamples`. placeGrainAt() now blends a second
    // ("bucket B") analysis bucket for the correlation-aligned crackle
    // fix, which needs one more period of reach plus the alignment search
    // margin — see getLatencySamples()'s doc for the full derivation and
    // docs/PERFORMANCE_LOG.md's 2026-08-23 entry for the measured
    // before/after latency numbers this costs.
    constexpr float minHz = 80.0f;
    const int maxPeriodSamples = (int) std::ceil (sampleRate / minHz);
    const int maxHalfWidthSamples = (int) std::ceil ((double) maxPeriodSamples * (double) grainWidthMultiplierMax);
    const int maxForwardReachSamples = maxPeriodSamples + 2 * maxHalfWidthSamples + psolaAlignSearchRadius;
    const int expectedLatency = maxForwardReachSamples + maxHalfWidthSamples;
    CHECK (shifter.getLatencySamples() == expectedLatency);

    // Deliberately NOT also asserting this stays below the phase
    // vocoder's window (an earlier version of this test did, hardcoded
    // to "< 2048"): that couples this shifter's own unit test to an
    // unrelated constant in a different file (PluginProcessor.h's
    // windowSizeMs), and it kept numerically passing even after
    // windowSizeMs=30 made the claim false (docs/PERFORMANCE_LOG.md's
    // 2026-08-19 entry) — false confidence from a test whose *premise*
    // had silently gone stale, not its assertion. The actual current
    // cross-engine comparison is tracked as measured data in
    // docs/ALGORITHMS.md and docs/PERFORMANCE_LOG.md, not as a test
    // invariant here, since it depends on independent tuning choices in
    // both engines rather than being a property of this shifter alone.
}

TEST_CASE ("grain-width multiplier is clamped to its documented range", "[psola-shifter]")
{
    constexpr double sampleRate = 44100.0;
    PSOLAPitchShifter shifter (sampleRate);

    // No public getter for the clamped value — exercised indirectly via
    // getLatencySamples() staying constant (setGrainWidthMultiplier()
    // never changes it, by design; see that method's doc) regardless of
    // in-range or out-of-range input. This test is really about the clamp
    // not throwing/asserting/crashing on an out-of-range value, which a
    // caller (the UI's slider range technically already prevents, but
    // PluginProcessor::setGrainWidthMultiplier's own clamp is the actual
    // last line of defense) could still pass.
    const int latencyBefore = shifter.getLatencySamples();
    shifter.setGrainWidthMultiplier (grainWidthMultiplierMin - 10.0f); // deliberately out of range
    CHECK (shifter.getLatencySamples() == latencyBefore);
    shifter.setGrainWidthMultiplier (grainWidthMultiplierMax + 10.0f); // deliberately out of range
    CHECK (shifter.getLatencySamples() == latencyBefore);
}

TEST_CASE ("grain-width multiplier of 1.0x reproduces the original fixed-width behaviour", "[psola-shifter]")
{
    // Regression guard: this is the actual claim made in
    // docs/ROADMAP.md's dated entry for this feature — that adding the
    // control doesn't change anything about the engine's existing,
    // already-tested behaviour when left at its default. Compares two
    // freshly-constructed shifters (one that never touches the new
    // setter, one that explicitly sets 1.0x) against identical input,
    // rather than comparing to a stored "golden" output, so this test
    // fails if 1.0x and "never called" ever stop being equivalent.
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float freq = 220.0f;
    constexpr float semitoneShift = 3.0f;

    PSOLAPitchShifter defaultShifter (sampleRate);
    PSOLAPitchShifter explicitShifter (sampleRate);
    explicitShifter.setGrainWidthMultiplier (1.0f);

    std::vector<float> defaultOutput, explicitOutput;
    for (int block = 0; block < 4; ++block)
    {
        const auto input = sineBlock (freq, sampleRate, blockSize, block);
        std::vector<float> out1 (blockSize, 0.0f), out2 (blockSize, 0.0f);
        defaultShifter.shiftPitch (freq, semitoneShift, input, out1);
        explicitShifter.shiftPitch (freq, semitoneShift, input, out2);
        defaultOutput = out1;
        explicitOutput = out2;
    }

    REQUIRE (defaultOutput.size() == explicitOutput.size());
    for (size_t i = 0; i < defaultOutput.size(); ++i)
        CHECK (defaultOutput[i] == explicitOutput[i]);
}

TEST_CASE ("a narrower grain still preserves signal energy within the existing loose bounds", "[psola-shifter]")
{
    // Same energy-sanity check as the very first test in this file, at
    // the multiplier range's lower bound instead of the default — a
    // narrower grain has less content to overlap-add, so this is worth
    // checking separately rather than assuming the existing 1.0x-only
    // energy test generalizes.
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float freq = 220.0f;

    PSOLAPitchShifter shifter (sampleRate);
    shifter.setGrainWidthMultiplier (grainWidthMultiplierMin);

    std::vector<float> lastOutput;
    for (int block = 0; block < 4; ++block)
    {
        const auto input = sineBlock (freq, sampleRate, blockSize, block);
        std::vector<float> output (blockSize, 0.0f);
        shifter.shiftPitch (freq, 0.0f, input, output);
        lastOutput = output;
    }

    const float inputRms = 1.0f / std::sqrt (2.0f);
    const float outputRms = rms (lastOutput);
    CHECK (outputRms > inputRms * 0.3f);
    CHECK (outputRms < inputRms * 3.0f);
}

TEST_CASE ("chooseGrainWidthMultiplierForShift: theory-derived values, not guessed", "[psola-shifter][grain-width-fix]")
{
    // ratio == 1 (unison): no change from the default, the already-clean
    // baseline this whole formula is anchored to.
    CHECK (chooseGrainWidthMultiplierForShift (0.0f) == 1.0f);

    // ratio < 1 (any downward shift): left at the default. Overlap only
    // grows past baseline in the upward direction (see this function's
    // own doc) -- downward shifts reduce overlap on their own, so there's
    // no mechanism this formula is meant to counteract there.
    CHECK (chooseGrainWidthMultiplierForShift (-12.0f) == 1.0f);
    CHECK (chooseGrainWidthMultiplierForShift (-3.0f) == 1.0f);

    // ratio == 2 (+12 semitones): 1/2 = 0.5, exactly grainWidthMultiplierMin
    // -- solving overlap = 2*multiplier*shiftRatio = 2 (the unison
    // baseline) for multiplier at shiftRatio=2 lands precisely on the
    // existing control's already-tested floor.
    CHECK (chooseGrainWidthMultiplierForShift (12.0f) == grainWidthMultiplierMin);

    // ratio == 4 (+24 semitones, a full two octaves): 1/4 = 0.25, below
    // grainWidthMultiplierMin -- clamped rather than asking the shifter
    // to operate somewhere untested/unsupported by the existing control.
    CHECK (chooseGrainWidthMultiplierForShift (24.0f) == grainWidthMultiplierMin);

    // ratio ~= 1.19 (+3 semitones): a real, non-edge-case value, checked
    // against the exact formula rather than just bounds -- 1/1.1892 ~= 0.8409.
    const float threeSemitoneResult = chooseGrainWidthMultiplierForShift (3.0f);
    CHECK (threeSemitoneResult > 0.83f);
    CHECK (threeSemitoneResult < 0.85f);
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
