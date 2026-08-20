#pragma once

#include "PSOLACorrector.h"
#include "PitchEngine.h"

namespace pitchzazz
{

/// Wraps PSOLACorrector (McLeod detection + TD-PSOLA shift) behind the
/// PitchEngine interface — the third registered engine, alongside Native
/// C++ (phase vocoder) and Rust (FFI). See PitchEngine.h for why this
/// indirection exists, and PSOLAPitchShifter.h for why this engine's
/// latency is dramatically lower (pitch-dependent, but typically a few ms
/// vs. the phase vocoder's fixed ~46-50ms window).
class PSOLACorrectorEngine : public PitchEngine
{
public:
    explicit PSOLACorrectorEngine (const EngineConfig& config);

    const char* getName() const noexcept override { return "TD-PSOLA (C++)"; }
    void setScale (Scale newScale) noexcept override;
    bool supportsRetuneControls() const noexcept override { return true; }
    void setCorrectionAmount (float amount) noexcept override { corrector.setCorrectionAmount (amount); }
    void setRetuneSpeedMs (float speedMs) noexcept override { corrector.setRetuneSpeedMs (speedMs); }
    bool supportsGrainWidthControl() const noexcept override { return true; }
    void setGrainWidthMultiplier (float multiplier) noexcept override { corrector.setGrainWidthMultiplier (multiplier); }
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output) override;
    [[nodiscard]] int getLatencySamples() const noexcept override { return corrector.getLatencySamples(); }

private:
    PSOLACorrector corrector;
};

} // namespace pitchzazz
