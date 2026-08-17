#pragma once

#include "Corrector.h"
#include "Scale.h"
#include <memory>
#include <vector>

namespace pitchzazz
{

/// Construction parameters common to every engine. Deliberately a plain
/// bundle rather than a fixed constructor signature on PitchEngine
/// itself — C++ has no virtual constructors, so each engine's factory
/// function does its own construction; this struct is just the common
/// subset every engine factory can expect to receive. An engine that
/// doesn't need one of these fields (e.g. a future engine with no
/// analysis-window concept) simply ignores it — the struct doesn't grow
/// per-engine, engine-specific tuning belongs in that engine's own
/// factory/constructor, not here.
struct EngineConfig
{
    int blockSize = 2048;
    double sampleRate = 44100.0;
    int windowSizeMs = 50;
    Scale initialScale;
};

/// The interface every pitch-correction algorithm implements to be
/// hot-swappable in this plugin — this project is deliberately set up as
/// a playground for comparing pitch-manipulation approaches (Rust vs.
/// C++ today, a future TD-PSOLA implementation or other algorithm
/// variant later — see docs/ROADMAP.md Phase 5), not a fixed pipeline
/// with one hardcoded algorithm. Adding a new one means: implement this
/// interface, write one factory function, add one line to
/// PitchEngineRegistry.cpp — nothing else in the plugin needs to change,
/// including the UI (see PitchEngineRegistry.h).
///
/// Not real-time-safe to call directly from processBlock — same
/// constraint as Corrector itself (construction and process() may
/// allocate). Lives on CorrectorWorker's background thread, same as
/// today. See docs/ARCHITECTURE.md.
class PitchEngine
{
public:
    virtual ~PitchEngine() = default;

    /// Short, stable, human-readable name for UI display (e.g. "Native
    /// C++", "Rust (FFI)") — not used for lookup/identity, just display;
    /// see PitchEngineRegistry for the identity/lookup mechanism.
    virtual const char* getName() const noexcept = 0;

    virtual void setScale (Scale newScale) noexcept = 0;

    /// `samples.size()` must equal the `blockSize` passed to whichever
    /// factory function constructed this engine.
    virtual CorrectionResult process (const std::vector<float>& samples, double sampleRate) = 0;

    /// This engine's algorithmic pipeline latency, in samples — for
    /// display and for AudioProcessor::setLatencySamples(), so the host's
    /// own latency compensation reflects reality rather than the
    /// block-accumulation-only lower bound the plugin reported before
    /// this existed (docs/ROADMAP.md Phase 3). May legitimately differ
    /// between engines (it does, between the two built in — see
    /// docs/COMPARISON.md's FFT-size-constraints section), which is
    /// exactly why this is per-engine rather than a plugin-wide constant.
    virtual int getLatencySamples() const noexcept = 0;
};

} // namespace pitchzazz
