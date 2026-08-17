#include "RustCorrectorEngine.h"

namespace pitchzazz
{

namespace
{
    std::uint8_t toTonicPitchClass (const Scale& scale) noexcept { return (std::uint8_t) scale.tonicPitchClass; }
    bool toIsMinor (const Scale& scale) noexcept { return scale.mode == ScaleMode::minor; }
}

RustCorrectorEngine::RustCorrectorEngine (const EngineConfig& config)
    : corrector (pitchzazz_rust::new_corrector (
          (std::size_t) config.blockSize, (std::size_t) config.sampleRate, (std::size_t) config.windowSizeMs,
          toTonicPitchClass (config.initialScale), toIsMinor (config.initialScale))),
      blockSize (config.blockSize)
{
}

void RustCorrectorEngine::setScale (Scale newScale) noexcept
{
    pitchzazz_rust::set_scale (*corrector, toTonicPitchClass (newScale), toIsMinor (newScale));
}

CorrectionResult RustCorrectorEngine::process (const std::vector<float>& samples, double sampleRate)
{
    CorrectionResult result;
    result.samples.assign ((size_t) blockSize, 0.0f);

    const rust::Slice<const float> inSlice (samples.data(), samples.size());
    const rust::Slice<float> outSlice (result.samples.data(), result.samples.size());

    const auto timings = pitchzazz_rust::process (*corrector, inSlice, (std::size_t) sampleRate, outSlice);

    result.detectedHz = timings.detected_hz;
    result.detectedClarity = timings.detected_clarity;
    result.semitoneShift = timings.semitone_shift;
    result.timings.detectUs = timings.detect_us;
    result.timings.quantizeUs = timings.quantize_us;
    result.timings.shiftUs = timings.shift_us;

    return result;
}

} // namespace pitchzazz
