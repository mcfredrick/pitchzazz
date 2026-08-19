#include "PSOLACorrectorEngine.h"

namespace pitchzazz
{

PSOLACorrectorEngine::PSOLACorrectorEngine (const EngineConfig& config)
    : corrector (config.blockSize, config.sampleRate, config.initialScale)
{
}

void PSOLACorrectorEngine::setScale (Scale newScale) noexcept
{
    corrector.setScale (newScale);
}

CorrectionResult PSOLACorrectorEngine::process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output)
{
    return corrector.process (samples, sampleRate, output);
}

} // namespace pitchzazz
