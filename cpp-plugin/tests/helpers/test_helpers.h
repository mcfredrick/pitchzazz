#pragma once
#include <PluginProcessor.h>
#include <memory>

// Helper to run test code within the context of a plugin editor.
// Creates a processor, opens its editor, runs the test, then cleans up.
//
// Example usage:
//   runWithinPluginEditor ([&] (PitchzazzAudioProcessor& plugin) {
//       auto* editor = plugin.getActiveEditor();
//       REQUIRE (editor != nullptr);
//   });
[[maybe_unused]] static void runWithinPluginEditor (const std::function<void (PitchzazzAudioProcessor& plugin)>& testCode)
{
    PitchzazzAudioProcessor plugin;

    // editorBeingDeleted() must run before the editor itself is destroyed
    // (JUCE's teardown contract), so it's a custom deleter rather than a
    // second manual delete — this also makes cleanup exception-safe: if
    // testCode throws, the editor is still torn down correctly instead of
    // leaking past the manual delete below it used to require.
    const auto deleter = [&plugin] (juce::AudioProcessorEditor* editorToDelete)
    {
        plugin.editorBeingDeleted (editorToDelete);
        delete editorToDelete;
    };
    const std::unique_ptr<juce::AudioProcessorEditor, decltype (deleter)> editor (plugin.createEditorIfNeeded(), deleter);

    testCode (plugin);
}
