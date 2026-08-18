#include <DSP/RetuneSmoothing.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace pitchzazz;

// The pure formulas behind docs/ROADMAP.md Phase 5's "classic Auto-Tune
// controls" — tested directly (exact values, no pitch-detection noise to
// tolerate) since Corrector/PSOLACorrector's own tests can only observe
// these through a full detect->quantize pipeline.

TEST_CASE ("correction amount scales the raw shift linearly", "[retune-smoothing]")
{
    CHECK (applyCorrectionAmount (4.0f, 1.0f) == 4.0f);
    CHECK (applyCorrectionAmount (4.0f, 0.0f) == 0.0f);
    CHECK (std::abs (applyCorrectionAmount (4.0f, 0.5f) - 2.0f) < 1e-5f);
}

TEST_CASE ("zero (or negative) retune time snaps instantly to the target, exactly", "[retune-smoothing]")
{
    CHECK (glideTowards (0.0f, 3.0f, 0.0f, 46.4f) == 3.0f);
    // Negative is defensive, not an expected input (setters clamp to >= 0
    // before this is ever called) -- checked anyway so this function's
    // behaviour at its own boundary is pinned down explicitly.
    CHECK (glideTowards (-2.0f, 5.0f, -10.0f, 46.4f) == 5.0f);
}

TEST_CASE ("positive retune time approaches the target without overshoot or an instant jump", "[retune-smoothing]")
{
    constexpr float previous = 0.0f;
    constexpr float target = 5.0f;
    constexpr float retuneTimeMs = 200.0f;
    constexpr float blockPeriodMs = 46.4f; // ~2048 samples @ 44.1kHz, this project's block size

    // The very first block must NOT jump straight to the target -- that's
    // the entire behavioural point of a nonzero retune time.
    const float firstBlockShift = glideTowards (previous, target, retuneTimeMs, blockPeriodMs);
    CHECK (firstBlockShift > previous);
    CHECK (firstBlockShift < target);

    float shift = previous;
    for (int block = 0; block < 30; ++block)
        shift = glideTowards (shift, target, retuneTimeMs, blockPeriodMs);

    // A one-pole filter chasing a fixed target monotonically approaches it
    // and never overshoots.
    CHECK (shift <= target);
    // 30 blocks * 46.4ms ~= 1.4s is ~7 time constants at 200ms -- should be
    // essentially converged by now (1 - e^-7 > 0.999).
    CHECK (shift > target * 0.999f);
}
