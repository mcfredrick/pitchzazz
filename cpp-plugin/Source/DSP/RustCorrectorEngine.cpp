#include "RustCorrectorEngine.h"

namespace pitchzazz
{

namespace
{
    std::uint8_t toTonicPitchClass (const Scale& scale) noexcept { return (std::uint8_t) scale.tonicPitchClass; }

    // Index convention shared with the Rust side's mode_from_index()
    // (crates/pitch-core-ffi/src/lib.rs) — 0-6 in Ionian/Dorian/Phrygian/
    // Lydian/Mixolydian/Aeolian/Locrian order. cxx can't share a Rust enum
    // with C++ directly, so both sides agree on this integer convention
    // instead of a second bridge type. Not a switch over ScaleMode's
    // declaration order by coincidence: ScaleMode's own values are
    // major/minor/dorian/phrygian/lydian/mixolydian/locrian (major/minor
    // first, for the two originally-ported modes), which doesn't match
    // this index order, so the mapping is explicit here rather than a
    // static_cast that would silently break if either enum's order ever
    // changes.
    std::uint8_t toModeIndex (const Scale& scale) noexcept
    {
        switch (scale.mode)
        {
            case ScaleMode::major:      return 0; // Ionian
            case ScaleMode::dorian:     return 1;
            case ScaleMode::phrygian:   return 2;
            case ScaleMode::lydian:     return 3;
            case ScaleMode::mixolydian: return 4;
            case ScaleMode::minor:      return 5; // Aeolian
            case ScaleMode::locrian:    return 6;
        }
        return 0; // unreachable for a valid ScaleMode
    }
}

RustCorrectorEngine::RustCorrectorEngine (const EngineConfig& config)
    : corrector (pitchzazz_rust::new_corrector (
          (std::size_t) config.blockSize, (std::size_t) config.sampleRate, (std::size_t) config.windowSizeMs,
          toTonicPitchClass (config.initialScale), toModeIndex (config.initialScale))),
      blockSize (config.blockSize)
{
}

void RustCorrectorEngine::setScale (Scale newScale) noexcept
{
    pitchzazz_rust::set_scale (*corrector, toTonicPitchClass (newScale), toModeIndex (newScale));
}

CorrectionResult RustCorrectorEngine::process (const std::vector<float>& samples, double sampleRate, std::vector<float>& output)
{
    jassert ((int) output.size() == blockSize);

    CorrectionResult result;

    // No pre-zero-fill needed: the Rust side's process() (crates/
    // pitch-core-ffi/src/lib.rs) always fully writes `out` on every call
    // — real samples via copy_from_slice on success, or out.fill(0.0) in
    // its catch_unwind panic-safety fallback — so `output` never carries
    // stale content from a previous block through to the caller.
    const rust::Slice<const float> inSlice (samples.data(), samples.size());
    const rust::Slice<float> outSlice (output.data(), output.size());

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
