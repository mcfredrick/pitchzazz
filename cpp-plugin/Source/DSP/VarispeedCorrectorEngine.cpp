#include "VarispeedCorrectorEngine.h"

namespace pitchzazz
{

VarispeedCorrectorEngine::VarispeedCorrectorEngine (const EngineConfig& config)
    : corrector (config.blockSize, config.sampleRate, config.initialScale)
{
}

void VarispeedCorrectorEngine::setScale (Scale newScale) noexcept
{
    corrector.setScale (newScale);
}

CorrectionResult VarispeedCorrectorEngine::process (const std::vector<float>& samples, double sampleRate)
{
    return corrector.process (samples, sampleRate);
}

} // namespace pitchzazz
