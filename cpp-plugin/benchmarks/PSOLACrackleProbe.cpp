// Measures the crackle/beat artifact from docs/FINDINGS.md #19/#20 directly,
// via a metric targeted at its actual documented mechanism, rather than a
// generic global energy statistic.
//
// Root cause per finding #20: placeGrainAt() reads from a fixed
// periodSamples-spaced analysis grid, but real voice's true period drifts
// from that fixed grid via natural cycle-to-cycle jitter -- so two
// "one-period-apart" analysis buckets aren't actually identical content, and
// snapping to the nearer one produces a real discontinuity at every bucket
// crossing.
//
// PSOLAPitchShifterTests.cpp's own "non-stationary tone" test already tried
// once to expose this, with tremoloSineBlock() -- and its comment is
// explicit that none of three automated approaches (including that one)
// discriminated the artifact. Worth being precise about why tremolo
// specifically couldn't: amplitude modulation never perturbs the PERIOD at
// all, so every analysis bucket of a tremolo tone is still exactly one true
// period long and interchangeable -- it doesn't exercise the mechanism
// finding #20 actually describes. This probe instead perturbs the
// instantaneous FREQUENCY -- true cycle-to-cycle jitter, phase-integrated so
// it's a real FM signal (sample = sin(integral of 2*pi*f(t)dt), not a
// closed-form sin(2*pi*f*t) with f swapped in, which would not correspond to
// any physically coherent waveform).
//
// Metric: rather than a global max/median delta ratio (tried, doesn't
// discriminate -- see PSOLAPitchShifterTests.cpp's comment), this measures
// local "roughness" (mean |second difference|, a discrete second-derivative
// proxy sensitive to sharp local kinks) in a small window around every
// synthesis mark, then reports the ratio between marks that DID cross an
// analysis-bucket boundary (per placeGrainAt()'s own floor-based bucket
// formula, replicated here from public state) and marks that didn't. A
// ratio near 1 means no detectable bucket-boundary signature; a ratio well
// above 1 means the mechanism is present and measurable at the marks where
// theory says it should be.

#include <DSP/PSOLAPitchShifter.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr int totalBlocks = 24; // ~557ms -- enough marks to average over even at the slowest (down-shift) spacing
    constexpr float baseFreq = 220.0f; // A3, this project's standard test tone

    // Two incommensurate slow rates summed, so the resulting frequency
    // wobble isn't a single clean periodicity that could accidentally
    // alias with the analysis-bucket spacing. Depth is realistic jitter
    // magnitude (published vocal jitter is typically under ~1%; 2% here is
    // deliberately on the higher/more-audible end of normal, not a
    // contrived worst case) applied as a *fraction of baseFreq*, phase-
    // integrated rather than substituted into a closed-form sin(2*pi*f*t).
    std::vector<float> jitteredVoice (double totalSeconds)
    {
        constexpr float jitterDepth = 0.02f;
        constexpr float jitterRate1 = 11.3f;
        constexpr float jitterRate2 = 17.7f;

        const size_t n = (size_t) (totalSeconds * sampleRate);
        std::vector<float> out (n);
        double phase = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / sampleRate;
            const double wobble = std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate1 * t) * 0.6
                                 + std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate2 * t) * 0.4;
            const double instFreq = (double) baseFreq * (1.0 + (double) jitterDepth * wobble);
            phase += 2.0 * juce::MathConstants<double>::pi * instFreq / sampleRate;
            out[i] = (float) std::sin (phase);
        }
        return out;
    }

    struct Mark
    {
        long long writeCenter;
        long long bucketIndex;
    };

    // Replicates placeGrainAt()'s mark-generation and bucket-assignment
    // formulas exactly, from public state only (a fresh shifter's
    // nextMarkPos always starts at 0.0 -- PSOLAPitchShifter.h's member
    // initializer -- and periodSamples for a fixed detectedHz within
    // [minHz, maxHz] is just sampleRate/detectedHz, no clamping applied).
    // Kept deliberately separate from and parallel to the production
    // formula rather than calling into it, so this probe can't silently
    // start passing just because it happens to share a bug with the code
    // it's checking.
    std::vector<Mark> predictMarks (float detectedHz, float semitoneShift, int totalOutputSamples)
    {
        const double periodSamples = sampleRate / (double) detectedHz;
        const double shiftRatio = std::pow (2.0, (double) semitoneShift / 12.0);
        const double synthesisSpacing = periodSamples / shiftRatio;

        std::vector<Mark> marks;
        double markPos = 0.0;
        while (markPos < (double) totalOutputSamples)
        {
            const double readMarkPos = std::floor (markPos / periodSamples + 1.0e-6) * periodSamples;
            const long long bucketIndex = (long long) std::llround (readMarkPos / periodSamples);
            marks.push_back ({ (long long) std::llround (markPos), bucketIndex });
            markPos += synthesisSpacing;
        }
        return marks;
    }

    double localRoughness (const std::vector<float>& signal, long long center, int halfWindow)
    {
        double sum = 0.0;
        int count = 0;
        for (long long i = center - halfWindow; i <= center + halfWindow; ++i)
        {
            if (i - 1 < 0 || i + 1 >= (long long) signal.size())
                continue;
            const double secondDiff = (double) signal[(size_t) (i + 1)] - 2.0 * (double) signal[(size_t) i] + (double) signal[(size_t) (i - 1)];
            sum += std::abs (secondDiff);
            ++count;
        }
        return count > 0 ? sum / (double) count : 0.0;
    }

    void probeAt (float semitoneShift)
    {
        PSOLAPitchShifter shifter (sampleRate);
        const int latencySamples = shifter.getLatencySamples();

        const auto input = jitteredVoice ((double) blockSize * totalBlocks / sampleRate);
        std::vector<float> output (input.size(), 0.0f);

        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            std::vector<float> out (blockSize, 0.0f);
            shifter.shiftPitch (baseFreq, semitoneShift, in, out);
            std::copy (out.begin(), out.end(), output.begin() + block * blockSize);
        }

        const auto marks = predictMarks (baseFreq, semitoneShift, (int) output.size() + latencySamples);

        constexpr int halfWindow = 8;
        // Only marks whose whole analysis window lands in the back 60% of
        // the render (past algorithmic + overlap-add warm-up) and whose
        // own window doesn't reach past the end of the buffer.
        const long long analysisStart = (long long) (output.size() * 4 / 10);

        // Classify by *actual bucket delta from the previous mark*, not
        // "changed vs unchanged" -- delta==1 is normal forward playback
        // (every mark reads one period later than the last, which happens
        // even at unison and is not a discontinuity risk in itself).
        // delta==0 is bucket *reuse* (placeGrainAt()'s own doc: "several
        // consecutive synthesis marks land in the same analysis bucket",
        // the pitch-up case). delta>1 is a *skip* ("some analysis buckets
        // never get used at all", the pitch-down case). Those latter two
        // are the actual documented risk mechanisms; delta==1 is the
        // control group.
        std::vector<double> normalRoughness, reuseRoughness, skipRoughness;
        long long previousBucket = std::numeric_limits<long long>::min();
        bool havePrevious = false;

        for (const auto& mark : marks)
        {
            const long long outputPos = mark.writeCenter - latencySamples;
            if (havePrevious && outputPos >= analysisStart && outputPos + halfWindow < (long long) output.size())
            {
                const double roughness = localRoughness (output, outputPos, halfWindow);
                const long long delta = mark.bucketIndex - previousBucket;
                if (delta == 1)
                    normalRoughness.push_back (roughness);
                else if (delta == 0)
                    reuseRoughness.push_back (roughness);
                else if (delta > 1)
                    skipRoughness.push_back (roughness);
                // delta < 0 shouldn't occur (markPos and floor() are both
                // monotonic) -- deliberately uncounted rather than silently
                // folded into another bucket if it ever does.
            }
            previousBucket = mark.bucketIndex;
            havePrevious = true;
        }

        const auto mean = [] (const std::vector<double>& v) -> double
        {
            if (v.empty())
                return 0.0;
            double s = 0.0;
            for (double x : v)
                s += x;
            return s / (double) v.size();
        };

        const double normalMean = mean (normalRoughness);
        const double reuseMean = mean (reuseRoughness);
        const double skipMean = mean (skipRoughness);
        const double reuseRatio = normalMean > 1.0e-12 ? reuseMean / normalMean : 0.0;
        const double skipRatio = normalMean > 1.0e-12 ? skipMean / normalMean : 0.0;

        std::cout << semitoneShift << "st: normal(delta=1) n=" << normalRoughness.size() << " mean=" << normalMean
                   << " | reuse(delta=0) n=" << reuseRoughness.size() << " mean=" << reuseMean << " ratio=" << reuseRatio
                   << " | skip(delta>1) n=" << skipRoughness.size() << " mean=" << skipMean << " ratio=" << skipRatio
                   << std::endl;
    }
}

TEST_CASE ("PSOLA crackle probe: bucket-reuse/skip roughness vs normal-progression roughness, jittered voice", "[psola][crackle]")
{
    std::cout << std::fixed << std::setprecision (6);
    std::cout << "\nPhase-integrated jittered 220Hz tone (2% depth, 11.3Hz+17.7Hz), detectedHz=220 fixed per block:\n";
    // -1/+1 (and the half-steps around them) added specifically: the user
    // reports the artifact is almost always present at real playing
    // shifts, which never exceed +-1 semitone (scale-correction's actual
    // operating range, docs/FINDINGS.md #27) -- the wide +-3..12 sweep
    // above never actually exercised that range at all.
    for (float shift : { -12.0f, -7.0f, -3.0f, -1.5f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.5f, 3.0f, 7.0f, 12.0f })
        probeAt (shift);
    std::cout << std::endl;
}
