#pragma once

#include <cmath>

namespace pitchzazz
{

// Bounds for the two classic Auto-Tune controls (docs/ROADMAP.md Phase 5's
// "Per-algorithm creative parameter exposure" item 1) — kept here, not
// duplicated per-engine, specifically so both C++ engines' setters clamp
// to the exact same range and the user-facing knob means the same thing
// regardless of which engine is active.
constexpr float correctionAmountMin = 0.0f;
constexpr float correctionAmountMax = 1.0f;
constexpr float retuneSpeedMsMin = 0.0f;
// ~1s: comfortably into "obviously gliding" territory for a sung phrase
// without being so long a glide rarely finishes before the next note
// change in typical vocal material — a ceiling chosen for musical
// usefulness, not a DSP constraint (the math below has no upper bound).
constexpr float retuneSpeedMsMax = 1000.0f;

// Fixed anti-click gain-ramp time constant for MIDI vocoder mode's
// "silence" fallback (docs/ROADMAP.md Phase 5) — deliberately *not* a
// user-facing control like the two constants above: this exists purely to
// avoid a hard mute/unmute discontinuity (the same class of bug
// docs/FINDINGS.md #14 found in the hot-swap crossfade), not to be a
// musical parameter someone would want to tune. 10ms is short enough to
// read as an immediate mute to a listener, long enough that a one-pole
// ramp at this time constant has no audible step at any block boundary.
constexpr float silenceRampMs = 10.0f;

/// Blends `rawShift` toward zero by `amount` (0 = no correction at all,
/// 1 = this project's original full-snap default). A pure scale, applied
/// *before* the glide below, not fused into one formula — keeping "how far
/// to correct" and "how fast to get there" as independent steps is what
/// lets them be independent knobs, matching how the classic Auto-Tune
/// controls are understood musically.
inline float applyCorrectionAmount (float rawShift, float amount) noexcept
{
    return rawShift * amount;
}

/// One-pole (exponential) smoothing of `previousShift` toward `targetShift`
/// — the same technique this codebase already uses for the GUI's
/// processing-time meters (PluginEditor.h's class doc), chosen here for
/// the same reason: no history buffer needed, and it's the standard
/// technique real envelope followers (attack/release) use, which a
/// "retune speed" control musically *is*.
///
/// `retuneTimeMs` is a time constant (~63% of the way to the target after
/// this many ms), not a raw per-block coefficient, so it means the same
/// thing regardless of block size or sample rate — the coefficient is
/// derived from it and `blockPeriodMs` on every call rather than cached,
/// since a stale cached coefficient would silently desync from the
/// control's stated meaning if either ever changed between blocks (host
/// sample-rate changes; a future variable block size).
///
/// `retuneTimeMs <= 0` returns `targetShift` exactly, rather than a
/// coefficient that only approaches (but never exactly reaches) 1 — this
/// is also what makes "speed 0" reproduce this project's original
/// instant-snap behaviour bit-for-bit, not just approximately.
inline float glideTowards (float previousShift, float targetShift, float retuneTimeMs, float blockPeriodMs) noexcept
{
    if (retuneTimeMs <= 0.0f)
        return targetShift;

    const float alpha = 1.0f - std::exp (-blockPeriodMs / retuneTimeMs);
    return previousShift + alpha * (targetShift - previousShift);
}

} // namespace pitchzazz
