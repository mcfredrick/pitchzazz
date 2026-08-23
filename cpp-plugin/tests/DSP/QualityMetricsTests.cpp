// Thresholds below are set from real measured numbers
// (benchmarks/QualityMetricsProbe.cpp's output, see docs/TESTING.md's
// "Objective quality metrics" section for the full table), with real
// headroom above the measured value — loose bounds that catch genuine
// regressions, not precision assertions, matching every other shifter
// test in this codebase.
//
// Deliberately stationary test tones only, same caveat
// PSOLAPitchShifterTests.cpp's own non-stationary test already states
// explicitly: a perfectly periodic signal's spectrum is unambiguous, so
// this is the right signal for comparing spectral cleanliness across
// engines and shift amounts. It is NOT an attempt to measure the PSOLA
// crackle/beat artifact from docs/FINDINGS.md #19/#20 — that artifact
// only exists on non-stationary content, and a tremolo-modulated tone's
// own genuine amplitude-modulation sidebands aren't distinguishable from
// "artifact energy" by a fundamental-harmonic-comb metric like this one
// without separately modeling the modulation itself. Three different
// automated approaches already failed to discriminate that specific
// artifact (see FINDINGS #19/#20); this metric answers a different,
// narrower question — spectral purity of a clean, stationary shift — and
// is not a fourth attempt at the crackle question.

#include <DSP/PitchShifter.h>
#include <DSP/PSOLAPitchShifter.h>
#include <DSP/QualityMetrics.h>
#include <DSP/VarispeedShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <functional>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int analysisFftSize = 16384;
    constexpr int warmupBlocks = 20; // see QualityMetricsProbe.cpp's identical constant for why
    constexpr float testFreq = 220.0f;

    std::vector<float> sineBlock (float freq, int blockIndex)
    {
        std::vector<float> block ((size_t) blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float t = (float) (blockIndex * blockSize + i) / (float) sampleRate;
            block[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freq * t);
        }
        return block;
    }

    using RenderFn = std::function<void (const std::vector<float>&, std::vector<float>&)>;

    QualityMetrics::Result measure (const RenderFn& render, float semitoneShift)
    {
        std::vector<float> allOutput;
        allOutput.reserve ((size_t) blockSize * warmupBlocks);

        for (int block = 0; block < warmupBlocks; ++block)
        {
            const auto input = sineBlock (testFreq, block);
            std::vector<float> output (blockSize, 0.0f);
            render (input, output);
            allOutput.insert (allOutput.end(), output.begin(), output.end());
        }

        QualityMetrics metrics (analysisFftSize);
        const float targetFreq = testFreq * std::pow (2.0f, semitoneShift / 12.0f);
        return metrics.analyze (allOutput, sampleRate, targetFreq);
    }

    QualityMetrics::Result measurePhaseVocoder (float shift)
    {
        PitchShifter shifter (50, sampleRate);
        return measure ([&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (16, shift, in, out); }, shift);
    }

    QualityMetrics::Result measurePsola (float shift)
    {
        PSOLAPitchShifter shifter (sampleRate);
        return measure ([&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (testFreq, shift, in, out); },
                         shift);
    }

    QualityMetrics::Result measureVarispeed (float shift)
    {
        VarispeedShifter shifter (sampleRate);
        return measure ([&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (shift, in, out); }, shift);
    }

    // Same as measurePsola(), but applies chooseGrainWidthMultiplierForShift()
    // before rendering -- the "after" half of the before/after comparison.
    QualityMetrics::Result measurePsolaWithChosenGrainWidth (float shift)
    {
        PSOLAPitchShifter shifter (sampleRate);
        shifter.setGrainWidthMultiplier (chooseGrainWidthMultiplierForShift (shift));
        return measure ([&] (const std::vector<float>& in, std::vector<float>& out) { shifter.shiftPitch (testFreq, shift, in, out); },
                         shift);
    }
}

TEST_CASE ("QualityMetrics: a clean, unshifted sine self-validates as clean", "[quality]")
{
    // No shifter at all — this validates the measurement tool itself
    // against a known-clean ground-truth signal before trusting what it
    // says about anything else. The Hann-windowed-analysis choice in
    // QualityMetrics::analyze() exists specifically so this passes: an
    // unwindowed FFT of a truncated sine leaks real energy into every
    // bin, which would otherwise make even a perfectly clean signal fail
    // this exact test.
    std::vector<float> signal ((size_t) analysisFftSize);
    for (int i = 0; i < analysisFftSize; ++i)
        signal[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * testFreq * (float) i / (float) sampleRate);

    QualityMetrics metrics (analysisFftSize);
    const auto result = metrics.analyze (signal, sampleRate, testFreq);

    CHECK (result.thdPlusNValid);
    CHECK (result.thdPlusNPercent < 2.0f);
    CHECK (result.artifactEnergyPercent < 2.0f);
}

TEST_CASE ("Phase vocoder stays spectrally clean across a wide shift sweep", "[quality]")
{
    // Real measured max across {-12,-3,0,3,12}: 0.82% (both metrics track
    // almost identically for this engine — see the class doc on why).
    // 2% is real headroom, not a guess.
    for (float shift : { -12.0f, -3.0f, 0.0f, 3.0f, 12.0f })
    {
        const auto result = measurePhaseVocoder (shift);
        INFO ("shift = " << shift << ", thdPlusN = " << result.thdPlusNPercent << "%, artifact = " << result.artifactEnergyPercent << "%");
        CHECK (result.thdPlusNPercent < 2.0f);
        CHECK (result.artifactEnergyPercent < 2.0f);
    }
}

TEST_CASE ("PSOLA: clean at unison", "[quality]")
{
    // Real measured: 0.56% for both metrics, matching the phase
    // vocoder's own unison numbers almost exactly.
    const auto result = measurePsola (0.0f);
    CHECK (result.thdPlusNPercent < 2.0f);
    CHECK (result.artifactEnergyPercent < 2.0f);
}

TEST_CASE ("PSOLA: an octave down collapses the true fundamental without producing broadband artifact energy", "[quality]")
{
    // Real, measured edge case (see QualityMetrics.h's doc on
    // thdPlusNValid): shifted a full octave down, TD-PSOLA's output
    // carries almost none of its energy at the true 110Hz fundamental
    // itself — it's nearly all on the upper harmonics instead. THD+N's
    // fundamental-relative ratio is meaningless here (thdPlusNValid
    // correctly reports false); what matters is that the energy is still
    // cleanly concentrated *somewhere* in the harmonic comb, not spread
    // into genuine artifact/noise — artifactEnergyPercent (whose
    // denominator is total energy, not the fragile fundamental alone)
    // confirms that: 0.57% measured, essentially as clean as unison.
    const auto result = measurePsola (-12.0f);
    CHECK_FALSE (result.thdPlusNValid);
    CHECK (result.artifactEnergyPercent < 2.0f);
}

TEST_CASE ("PSOLA: an octave up produces real, severe artifact energy", "[quality]")
{
    // Real measured: 99.18% — essentially all of the output's energy
    // sits outside the target fundamental's harmonic comb at this shift.
    // 50% is a loose bound (half the measured value) that still asserts
    // something real and specific: this is a genuine, large-scale
    // breakdown, not sensor noise or a rounding artifact, consistent
    // with the accepted grain-boundary limitation PSOLAPitchShifter.h's
    // own class doc already documents (docs/FINDINGS.md #19/#20) — this
    // test puts an actual number on a different, wider-scope symptom of
    // the same underlying grain-based reconstruction limits.
    const auto result = measurePsola (12.0f);
    CHECK (result.artifactEnergyPercent > 50.0f);
}

TEST_CASE ("PSOLA: chooseGrainWidthMultiplierForShift fixes the +12 semitone breakdown", "[quality][grain-width-fix]")
{
    // Real measured: 99.18% (default 1.0x multiplier) -> 2.20% (formula's
    // 0.5x) -- a ~45x reduction, from "essentially all noise" to "as
    // clean as unison." 10x is a loose bound with real margin, not a
    // precision claim.
    const auto before = measurePsola (12.0f);
    const auto after = measurePsolaWithChosenGrainWidth (12.0f);
    CHECK (after.artifactEnergyPercent < before.artifactEnergyPercent / 10.0f);
    CHECK (after.artifactEnergyPercent < 5.0f);
}

TEST_CASE ("PSOLA: chooseGrainWidthMultiplierForShift also helps at +9, a smaller but real win", "[quality][grain-width-fix]")
{
    // Real measured: 3.58% -> 2.11%. Smaller than the +12 case (the
    // overlap problem is milder there to begin with), but a genuine,
    // real-margin improvement, not noise.
    const auto before = measurePsola (9.0f);
    const auto after = measurePsolaWithChosenGrainWidth (9.0f);
    CHECK (after.artifactEnergyPercent < before.artifactEnergyPercent * 0.75f);
}

TEST_CASE ("PSOLA: chooseGrainWidthMultiplierForShift does not regress badly at small shifts", "[quality][grain-width-fix]")
{
    // Real measured: at +3 semitones the formula's narrower grain (0.84x)
    // is actually very slightly *worse* than the 1.0x default (1.29% vs.
    // 1.20%) -- the overlap problem the formula targets barely exists
    // yet at this shift, so narrowing the grain trades away a little
    // within-grain frequency resolution for no offsetting benefit. Not
    // hidden or asserted away: this test only guards against a much
    // larger regression than the ~0.1 percentage point one actually
    // measured, consistent with reporting what the data shows rather
    // than what the hypothesis predicted.
    const auto before = measurePsola (3.0f);
    const auto after = measurePsolaWithChosenGrainWidth (3.0f);
    CHECK (after.artifactEnergyPercent < before.artifactEnergyPercent + 1.0f);
}

TEST_CASE ("Varispeed carries real, measurable baseline artifact energy that the other two engines don't", "[quality]")
{
    // Confirms with a real number what VarispeedResampler.h's own class
    // doc already states as a documented-but-unmeasured limitation ("an
    // extreme up-shift could alias harshly... not tuned against by ear
    // yet, flagged here rather than assumed"): unlike the phase vocoder
    // and PSOLA (both under ~1% even at unison), Varispeed's
    // cubic-Hermite resampler carries substantial artifact energy even
    // at *zero* shift (measured ~22.3%) — its own per-call anticipatory
    // click-suppression gain ramp (VarispeedShifter::shiftPitch's
    // fade-out/fade-in) likely contributes to this baseline, on top of
    // the resampler's own interpolation error; this test doesn't attempt
    // to separate those two sources, only to confirm the total is real
    // and consistently larger than the other engines', which is what
    // actually matters for an engine-selection/quality comparison.
    const auto unison = measureVarispeed (0.0f);
    CHECK (unison.artifactEnergyPercent > 15.0f);

    const auto phaseVocoderUnison = measurePhaseVocoder (0.0f);
    const auto psolaUnison = measurePsola (0.0f);
    CHECK (unison.artifactEnergyPercent > phaseVocoderUnison.artifactEnergyPercent * 5.0f);
    CHECK (unison.artifactEnergyPercent > psolaUnison.artifactEnergyPercent * 5.0f);

    // Real measured: 22.96% at unison -> 30.01% at +12 semitones, a
    // ~30% relative increase — comfortable margin above measurement
    // noise, unlike the -12 direction (23.57% vs. 22.96% unison, too
    // thin a gap to assert reliably), which is why only the upward
    // direction is asserted here.
    const auto upOctave = measureVarispeed (12.0f);
    CHECK (upOctave.artifactEnergyPercent > unison.artifactEnergyPercent * 1.15f);
}
