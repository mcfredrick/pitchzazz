#include "PitchEngineRegistry.h"
#include "NativeCorrectorEngine.h"
#include "PSOLACorrectorEngine.h"
#include "RustCorrectorEngine.h"

namespace pitchzazz
{

const std::vector<EngineDescriptor>& availableEngines()
{
    static const std::vector<EngineDescriptor> engines = {
        {
            "native-cpp",
            "Phase Vocoder (Native C++)",
            [] (const EngineConfig& config) -> std::unique_ptr<PitchEngine>
            {
                return std::make_unique<NativeCorrectorEngine> (config);
            },
        },
        {
            "rust-ffi",
            "Phase Vocoder (Rust FFI)",
            [] (const EngineConfig& config) -> std::unique_ptr<PitchEngine>
            {
                return std::make_unique<RustCorrectorEngine> (config);
            },
        },
        {
            "psola-cpp",
            "TD-PSOLA (C++)",
            [] (const EngineConfig& config) -> std::unique_ptr<PitchEngine>
            {
                return std::make_unique<PSOLACorrectorEngine> (config);
            },
        },
    };
    return engines;
}

std::unique_ptr<PitchEngine> createEngine (const std::string& id, const EngineConfig& config)
{
    for (const auto& descriptor : availableEngines())
        if (descriptor.id == id)
            return descriptor.create (config);
    return nullptr;
}

} // namespace pitchzazz
