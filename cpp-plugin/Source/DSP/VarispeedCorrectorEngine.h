#pragma once

#include "PitchEngine.h"
#include "VarispeedCorrector.h"

namespace pitchzazz
{

/// Wraps VarispeedCorrector (McLeod detection + WSOLA-then-resample
/// shift) behind the PitchEngine interface — a third algorithm family
/// alongside the phase vocoder (Native C++, Rust FFI) and TD-PSOLA. See
/// VarispeedShifter.h for the actual mechanism and why its latency is
/// bounded independent of shift amount despite being built from two
/// variable-rate stages.
class VarispeedCorrectorEngine : public PitchEngine
{
public:
    explicit VarispeedCorrectorEngine (const EngineConfig& config);

    const char* getName() const noexcept override { return "Varispeed + WSOLA"; }
    void setScale (Scale newScale) noexcept override;
    bool supportsRetuneControls() const noexcept override { return true; }
    void setCorrectionAmount (float amount) noexcept override { corrector.setCorrectionAmount (amount); }
    void setRetuneSpeedMs (float speedMs) noexcept override { corrector.setRetuneSpeedMs (speedMs); }
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output) override;
    [[nodiscard]] int getLatencySamples() const noexcept override { return corrector.getLatencySamples(); }

private:
    VarispeedCorrector corrector;
};

} // namespace pitchzazz
