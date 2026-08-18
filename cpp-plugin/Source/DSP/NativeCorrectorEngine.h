#pragma once

#include "PitchEngine.h"

namespace pitchzazz
{

/// Wraps the native C++ Corrector (McLeod detection + phase-vocoder
/// shift, ported directly from pitch-core) behind the PitchEngine
/// interface. See PitchEngine.h for why this indirection exists.
class NativeCorrectorEngine : public PitchEngine
{
public:
    explicit NativeCorrectorEngine (const EngineConfig& config);

    const char* getName() const noexcept override { return "Native C++"; }
    void setScale (Scale newScale) noexcept override;
    bool supportsRetuneControls() const noexcept override { return true; }
    void setCorrectionAmount (float amount) noexcept override { corrector.setCorrectionAmount (amount); }
    void setRetuneSpeedMs (float speedMs) noexcept override { corrector.setRetuneSpeedMs (speedMs); }
    bool supportsMidiTargeting() const noexcept override { return true; }
    void setMidiTargetNote (int noteNumber) noexcept override { corrector.setMidiTargetNote (noteNumber); }
    void setMidiFallbackMode (MidiFallbackMode mode) noexcept override { corrector.setMidiFallbackMode (mode); }
    [[nodiscard]] CorrectionResult process (const std::vector<float>& samples, double sampleRate) override;
    [[nodiscard]] int getLatencySamples() const noexcept override { return corrector.getLatencySamples(); }

private:
    Corrector corrector;
};

} // namespace pitchzazz
