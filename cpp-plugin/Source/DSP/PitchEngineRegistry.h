#pragma once

#include "PitchEngine.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pitchzazz
{

struct EngineDescriptor
{
    std::string id;          // stable identifier — used for lookup, not for display
    std::string displayName; // shown in the editor's engine selector
    std::function<std::unique_ptr<PitchEngine> (const EngineConfig&)> create;
};

/// The full list of pitch-correction engines available to hot-swap
/// between, in display order. To add a new algorithm module: implement
/// PitchEngine (see PitchEngine.h), write one factory function, add one
/// entry to the list in PitchEngineRegistry.cpp. Nothing else needs to
/// change — the editor's engine selector and CorrectorWorker both
/// enumerate this list generically rather than hardcoding engine names,
/// specifically so this plugin stays a playground for comparing
/// pitch-manipulation algorithms (docs/ROADMAP.md Phase 5) rather than a
/// fixed two-engine special case.
const std::vector<EngineDescriptor>& availableEngines();

/// Constructs the engine registered under `id`, or nullptr if `id` isn't
/// found (callers should treat that as a programming error — ids come
/// from availableEngines(), not user text input).
[[nodiscard]] std::unique_ptr<PitchEngine> createEngine (const std::string& id, const EngineConfig& config);

} // namespace pitchzazz
