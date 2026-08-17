#include "NativeCorrectorEngine.h"

namespace pitchzazz
{

NativeCorrectorEngine::NativeCorrectorEngine (const EngineConfig& config)
    : corrector (config.blockSize, config.sampleRate, config.windowSizeMs, config.initialScale)
{
}

void NativeCorrectorEngine::setScale (Scale newScale) noexcept
{
    corrector.setScale (newScale);
}

CorrectionResult NativeCorrectorEngine::process (const std::vector<float>& samples, double sampleRate)
{
    return corrector.process (samples, sampleRate);
}

} // namespace pitchzazz
