#include "WSOLATimeStretcher.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

namespace
{
    constexpr float pi = 3.14159265358979323846f;
    constexpr double windowDurationSeconds = 0.025;
}

WSOLATimeStretcher::WSOLATimeStretcher (double sampleRate)
{
    // Rounded to even so synthesisHopSamples is exact and the Hann window
    // is symmetric — see the header's doc for why 25ms and 50% overlap.
    const int rawWindow = (int) std::lround (sampleRate * windowDurationSeconds);
    windowSizeSamples = std::max (16, rawWindow + (rawWindow % 2));
    synthesisHopSamples = windowSizeSamples / 2;
    searchRadiusSamples = windowSizeSamples / 16;
    correlationLengthSamples = std::min (synthesisHopSamples, maxCorrelationLengthSamples);

    // Periodic (DFT-even) Hann definition — divides by windowSizeSamples,
    // not windowSizeSamples - 1 — which is the one that satisfies exact
    // constant-overlap-add at 50% overlap. Kept even though this class
    // also normalizes by an explicit weight accumulator at drain time
    // (belt-and-braces, not a substitute for getting the window right).
    hannWindow.resize ((size_t) windowSizeSamples);
    for (int k = 0; k < windowSizeSamples; ++k)
        hannWindow[(size_t) k] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) k / (float) windowSizeSamples);

    // Generous, fixed headroom (1 full second) rather than tightly sized
    // to the theoretical minimum (windowSizeSamples + searchRadiusSamples)
    // — cheap in memory (a few hundred KB of floats total across all three
    // buffers), same trade-off VarispeedResampler's history buffer makes,
    // and this class has more moving parts (analysis *and* synthesis
    // position bookkeeping) where a tight bound would be easy to get
    // subtly wrong in a first pass.
    const size_t bufferCapacity = std::max<size_t> ((size_t) sampleRate, 16384);
    history.assign (bufferCapacity, 0.0f);
    accumulator.assign (bufferCapacity, 0.0f);
    accumulatorWeight.assign (bufferCapacity, 0.0f);
}

void WSOLATimeStretcher::push (const float* input, int count)
{
    for (int i = 0; i < count; ++i)
    {
        history[(size_t) (totalPushed % (long long) history.size())] = input[i];
        ++totalPushed;
    }
}

int WSOLATimeStretcher::searchBestOffset (long long nominalStart) const noexcept
{
    const long long refStart = lastPlacedAnalysisStart + synthesisHopSamples;

    // Reference (previous window's tail) energy doesn't depend on the
    // candidate offset — computed once outside the offset loop rather
    // than 2*searchRadiusSamples+1 times. Only correlationLengthSamples
    // long (a fixed constant), not the full overlap region — see the
    // header's doc for why: this is the specific fix for a measured
    // O(sampleRate²) search-cost bug.
    float refEnergy = 0.0f;
    for (int k = 0; k < correlationLengthSamples; ++k)
    {
        const float a = historyAt (refStart + k);
        refEnergy += a * a;
    }

    // Normalized cross-correlation between the reference segment and the
    // candidate segment starting at `candStart` — comparable across
    // candidates that may have genuinely different energy (a plain dot
    // product would be biased toward louder segments, regardless of how
    // well-aligned the waveform shape actually is).
    const auto correlationAt = [&] (long long candStart) noexcept -> float
    {
        float dot = 0.0f;
        float candEnergy = 0.0f;
        for (int k = 0; k < correlationLengthSamples; ++k)
        {
            const float a = historyAt (refStart + k);
            const float b = historyAt (candStart + k);
            dot += a * b;
            candEnergy += b * b;
        }
        const float denom = std::sqrt (refEnergy * candEnergy) + 1.0e-9f;
        return dot / denom;
    };

    // The nominal (target) position is the baseline every other candidate
    // has to *substantially* beat, not just edge out — a hard minimum-
    // improvement bar, not a continuous distance-proportional penalty (an
    // earlier version of this function). Found necessary, not a
    // defensive habit: on strongly periodic material, a pure correlation-
    // quality search has no reason to prefer offset 0 over some other
    // offset that happens to correlate just as well (or marginally
    // better, from finite-window/floating-point noise) — and a
    // proportional penalty is negligible right near offset 0, so it
    // couldn't actually stop that marginal preference from kicking off
    // the same drift-then-jump pattern, just at a smaller scale (measured
    // after that first fix: cycle length dropped from 9-13 hops to ~5,
    // reported as sounding "like listening through a fan" — a *faster*,
    // smaller periodic modulation is not an improvement, it crosses into
    // continuous textural roughness rather than occasional discrete
    // clicks). A hard bar closes that gap: nominal wins every tie and
    // every near-tie, and only a clearly-substantial alignment
    // improvement elsewhere is allowed to move the read position away
    // from where the intended stretch ratio actually wants it — which is
    // still WSOLA's real reason to exist over fixed-hop overlap-add, just
    // reserved for when it's actually earned.
    constexpr float minImprovementToDeviate = 0.02f;

    const float nominalScore = correlationAt (nominalStart);
    float bestScore = nominalScore;
    int bestOffset = 0;

    for (int offset = -searchRadiusSamples; offset <= searchRadiusSamples; ++offset)
    {
        if (offset == 0)
            continue; // nominal already evaluated as the baseline above

        const long long candStart = nominalStart + offset;
        if (candStart < 0)
            continue; // no real history before index 0 yet — only possible near stream start

        const float score = correlationAt (candStart);
        if (score > nominalScore + minImprovementToDeviate && score > bestScore)
        {
            bestScore = score;
            bestOffset = offset;
        }
    }
    return bestOffset;
}

bool WSOLATimeStretcher::placeNextWindowIfPossible (int analysisHopSamples)
{
    const long long nominalStart = nextAnalysisPos;

    // Widest possible read this call could need: the search (if any) may
    // reach up to searchRadiusSamples past nominalStart, and whichever
    // candidate wins still needs its full window available.
    const long long requiredPushed = nominalStart + (long long) searchRadiusSamples + windowSizeSamples;
    if (requiredPushed > totalPushed)
        return false;

    const int offset = (lastPlacedAnalysisStart >= 0) ? searchBestOffset (nominalStart) : 0;
    const long long actualStart = std::max<long long> (nominalStart + offset, 0);

    for (int k = 0; k < windowSizeSamples; ++k)
    {
        const size_t accSlot = (size_t) ((nextSynthesisPos + k) % (long long) accumulator.size());
        const float windowed = hannWindow[(size_t) k] * historyAt (actualStart + k);
        accumulator[accSlot] += windowed;
        accumulatorWeight[accSlot] += hannWindow[(size_t) k];
    }

    lastPlacedAnalysisStart = actualStart;

    // Positions before this window's own (fixed-hop) start can never
    // receive another contribution — only this window and the one placed
    // immediately before it can ever touch them, and both are now placed
    // — so this is exactly the new finalized/drainable frontier.
    placedFrontier = nextSynthesisPos;

    nextSynthesisPos += synthesisHopSamples;
    nextAnalysisPos += analysisHopSamples;
    return true;
}

int WSOLATimeStretcher::pull (float ratio, float* output, int maxCount)
{
    // ratio > 1 must mean "output longer than input" (stretch): for a
    // fixed amount of pushed input, that means consuming it *slower*
    // than the fixed synthesis-domain hop, i.e. analysisHop shrinks as
    // ratio grows — division, not multiplication. (Confirmed by an
    // inverted first attempt here: multiplying produced the opposite of
    // the documented contract, caught by
    // WSOLATimeStretcherTests.cpp's duration-ratio tests.)
    const int analysisHopSamples = std::max (1, (int) std::lround ((double) synthesisHopSamples / (double) ratio));

    int produced = 0;
    while (produced < maxCount)
    {
        while (produced < maxCount && drainedSynthesisPos < placedFrontier)
        {
            const size_t slot = (size_t) (drainedSynthesisPos % (long long) accumulator.size());
            const float weight = accumulatorWeight[slot];
            output[produced++] = weight > 1.0e-6f ? accumulator[slot] / weight : accumulator[slot];
            accumulator[slot] = 0.0f;
            accumulatorWeight[slot] = 0.0f;
            ++drainedSynthesisPos;
        }
        if (produced >= maxCount)
            break;
        if (! placeNextWindowIfPossible (analysisHopSamples))
            break;
    }
    return produced;
}

} // namespace pitchzazz
