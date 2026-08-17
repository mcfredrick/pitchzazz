#pragma once

#include "PitchDetector.h"
#include "PitchShifter.h"
#include "Scale.h"
#include <vector>

namespace pitchzazz
{

/// Standard equal-temperament Hz -> fractional MIDI note number, A4 = 69 = 440Hz.
float hzToMidi (float freqHz) noexcept;

/// Inverse of hzToMidi.
float midiToHz (float midi) noexcept;

/// Wall-clock cost of each stage of one Corrector::process() call, in
/// microseconds. Mirrors pitch-core's StageTimings exactly — same field
/// names, same shape — so the Phase 3 Rust-vs-C++ benchmark comparison
/// (docs/ROADMAP.md) reports cost the same way on both sides.
struct StageTimings
{
    double detectUs = 0.0;
    double quantizeUs = 0.0;
    double shiftUs = 0.0;

    double totalUs() const noexcept { return detectUs + quantizeUs + shiftUs; }
};

struct CorrectionResult
{
    std::vector<float> samples;
    float detectedHz = 0.0f;
    float detectedClarity = 0.0f;
    float semitoneShift = 0.0f;
    StageTimings timings;
};

/// Detects pitch, snaps it to the nearest note in a Scale, and pitch-
/// shifts the block accordingly. Direct port of pitch-core's
/// PitchCorrector (crates/pitch-core/src/corrector.rs) — same stages,
/// same constants, same over-sampling factor. See `overSampling` below
/// for the current value and docs/PERFORMANCE_LOG.md for the measured
/// cost/latency data behind it — re-measured for this engine
/// specifically, not assumed to carry over from the Rust side's numbers.
///
/// Not real-time-safe to call directly from processBlock: `process()`
/// allocates its output vector. Belongs on a worker thread, fed and
/// drained via lock-free queues — see docs/ARCHITECTURE.md and the
/// Day 3 task in docs/ROADMAP.md Phase 2.
class Corrector
{
public:
    Corrector (int blockSize, double sampleRate, int windowSizeMs, Scale scale);

    void setScale (Scale newScale) noexcept { scale = newScale; }

    /// `samples.size()` must equal the `blockSize` passed to the constructor.
    CorrectionResult process (const std::vector<float>& samples, double sampleRate);

    /// Algorithmic pipeline latency, in samples — delegates to the
    /// shifter, since that's the sole contributor (see
    /// PitchShifter::getLatencySamples's doc).
    int getLatencySamples() const noexcept { return shifter.getLatencySamples(); }

private:
    PitchDetector detector;
    PitchShifter shifter;
    int blockSize;
    Scale scale;

    // Controls the phase vocoder's STFT hop size (step = frameSize /
    // overSampling), not the analysis window itself — affects
    // reconstruction quality and CPU cost, not latency (confirmed
    // experimentally: latency was identical across 4/8/16/32/64 in
    // docs/PERFORMANCE_LOG.md's "OVER_SAMPLING re-evaluated" entry, which
    // has the full cost-vs-value data for both engines). Raised from 8 to
    // 16 there: cost scales ~linearly and stays cheap even at 64 (under
    // 6% of budget for this engine in Release), so affordability isn't
    // the constraint — diminishing quality returns past some point are,
    // which needs a listening test this project doesn't have tooling
    // for. 16 is a conservative, defensible interim value, not a claim
    // that it's optimal.
    static constexpr int overSampling = 16;
};

} // namespace pitchzazz
