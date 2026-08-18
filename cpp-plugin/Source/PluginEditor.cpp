#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
    // Index 0 = C, matching pitchzazz::Scale::tonicPitchClass's convention.
    const char* const pitchClassNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                               "F#", "G", "G#", "A", "A#", "B" };

    // Single source of truth for modeBox's population, initial selection,
    // and updateProcessorScale()'s ID-to-mode mapping — a scattered
    // switch/ternary in three places would be an easy spot for the display
    // order and the actual enum mapping to drift apart. ComboBox item IDs
    // are 1-based, so item ID = array index + 1. Major/minor listed first
    // (matches their original two hardcoded IDs 1/2, before the other five
    // modes existed) rather than reordered into "modal brightness" order,
    // so muscle memory from before this feature still selects the same
    // two modes at the same IDs.
    struct ModeEntry { pitchzazz::ScaleMode mode; const char* displayName; };
    const ModeEntry modeEntries[7] = {
        { pitchzazz::ScaleMode::major,      "Major" },
        { pitchzazz::ScaleMode::minor,      "Minor" },
        { pitchzazz::ScaleMode::dorian,     "Dorian" },
        { pitchzazz::ScaleMode::phrygian,   "Phrygian" },
        { pitchzazz::ScaleMode::lydian,     "Lydian" },
        { pitchzazz::ScaleMode::mixolydian, "Mixolydian" },
        { pitchzazz::ScaleMode::locrian,    "Locrian" }
    };

    // "A3  220.5Hz", or "--" for hz <= 0 (unvoiced/silent/not yet
    // available) — shared formatting for both LCD readouts so "detected"
    // and "corrected" always read the same way.
    juce::String noteNameAndHz (float hz)
    {
        if (hz <= 0.0f)
            return "--";

        const int roundedMidi = (int) std::lround (pitchzazz::hzToMidi (hz));
        const int pitchClass = ((roundedMidi % 12) + 12) % 12; // MIDI can't go negative here, but robust regardless
        const int octave = roundedMidi / 12 - 1;                // MIDI 60 = C4, the standard convention

        return juce::String (pitchClassNames[pitchClass]) + juce::String (octave)
             + "  " + juce::String (hz, 1) + "Hz";
    }
}

//==============================================================================
PitchzazzAudioProcessorEditor::PitchzazzAudioProcessorEditor (PitchzazzAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setLookAndFeel (&lookAndFeel);

    for (int i = 0; i < 12; ++i)
        tonicBox.addItem (pitchClassNames[i], i + 1); // ComboBox item IDs are 1-based
    tonicBox.setSelectedId (processorRef.getScale().tonicPitchClass + 1, juce::dontSendNotification);
    tonicBox.onChange = [this] { updateProcessorScale(); };
    addAndMakeVisible (tonicLabel);
    addAndMakeVisible (tonicBox);

    for (int i = 0; i < 7; ++i)
        modeBox.addItem (modeEntries[i].displayName, i + 1);
    {
        const auto currentMode = processorRef.getScale().mode;
        int selectedId = 1; // falls back to Major if somehow not found below
        for (int i = 0; i < 7; ++i)
            if (modeEntries[i].mode == currentMode)
                selectedId = i + 1;
        modeBox.setSelectedId (selectedId, juce::dontSendNotification);
    }
    modeBox.onChange = [this] { updateProcessorScale(); };
    addAndMakeVisible (modeLabel);
    addAndMakeVisible (modeBox);

    // Populated generically from the registry, not hardcoded engine
    // names — a future third algorithm module needs no editor changes
    // (docs/ROADMAP.md Phase 3). Item IDs are 1-based (JUCE convention);
    // index i in availableEngines() maps to item ID i+1.
    const auto& engines = processorRef.getAvailableEngines();
    for (size_t i = 0; i < engines.size(); ++i)
        engineBox.addItem (engines[i].displayName, (int) i + 1);
    const juce::String activeName = processorRef.getActiveEngineName();
    for (size_t i = 0; i < engines.size(); ++i)
        if (engines[i].displayName == activeName.toStdString())
            engineBox.setSelectedId ((int) i + 1, juce::dontSendNotification);
    if (engineBox.getSelectedId() == 0 && ! engines.empty())
        engineBox.setSelectedId (1, juce::dontSendNotification); // prepareToPlay hasn't run yet — show the default
    engineBox.onChange = [this]
    {
        const auto& availableEngines = processorRef.getAvailableEngines();
        const int index = engineBox.getSelectedId() - 1;
        if (index >= 0 && (size_t) index < availableEngines.size())
        {
            processorRef.setEngine (availableEngines[(size_t) index].id);
            resetSmoothingOnNextTick = true;
            updateRetuneControlsEnablement();
        }
    };
    addAndMakeVisible (engineLabel);
    addAndMakeVisible (engineBox);

    // Range/suffix chosen to match how the DSP layer itself interprets
    // these values (RetuneSmoothing.h): amount as a 0-100% blend, speed as
    // a millisecond time constant (not a 0-100 "feel" knob) so the number
    // shown is the number that's actually meaningful to the math, not an
    // arbitrary UI-only scale.
    correctionAmountSlider.setRange (100.0 * (double) pitchzazz::correctionAmountMin,
                                      100.0 * (double) pitchzazz::correctionAmountMax, 1.0);
    correctionAmountSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    correctionAmountSlider.setTextValueSuffix ("%");
    correctionAmountSlider.setValue (100.0 * (double) processorRef.getCorrectionAmount(), juce::dontSendNotification);
    correctionAmountSlider.onValueChange = [this] { processorRef.setCorrectionAmount ((float) (correctionAmountSlider.getValue() / 100.0)); };
    addAndMakeVisible (correctionAmountLabel);
    addAndMakeVisible (correctionAmountSlider);

    retuneSpeedSlider.setRange ((double) pitchzazz::retuneSpeedMsMin, (double) pitchzazz::retuneSpeedMsMax, 1.0);
    retuneSpeedSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    retuneSpeedSlider.setTextValueSuffix ("ms");
    retuneSpeedSlider.setValue ((double) processorRef.getRetuneSpeedMs(), juce::dontSendNotification);
    retuneSpeedSlider.onValueChange = [this] { processorRef.setRetuneSpeedMs ((float) retuneSpeedSlider.getValue()); };
    addAndMakeVisible (retuneSpeedLabel);
    addAndMakeVisible (retuneSpeedSlider);

    // Cyan for "detected" (matches the detect-stage meter below) and
    // green for "corrected" (matches the shift-stage meter, and reads as
    // the "final/settled" value) — same shared-palette convention as
    // everything else in this editor.
    detectedPitchDisplay.setCaption ("DETECTED");
    detectedPitchDisplay.setAccentColour (juce::Colour (pitchzazz::colours::accentCyan));
    addAndMakeVisible (detectedPitchDisplay);

    correctedPitchDisplay.setCaption ("CORRECTED");
    correctedPitchDisplay.setAccentColour (juce::Colour (pitchzazz::colours::accentGreen));
    addAndMakeVisible (correctedPitchDisplay);

    // Cyan to match the DETECTED box's own accent (this meter is reading
    // the fine-grained detail behind that box's note name), green for the
    // in-tune band — reusing the same "engaged/good" green the bypass LED
    // and CORRECTED box already use elsewhere in this editor.
    detectedCentsMeter.setAccentColour (juce::Colour (pitchzazz::colours::accentCyan));
    detectedCentsMeter.setInTuneColour (juce::Colour (pitchzazz::colours::accentGreen));
    addAndMakeVisible (detectedCentsMeter);

    bypassButton.setToggleState (processorRef.isBypassed(), juce::dontSendNotification);
    bypassButton.onClick = [this] { processorRef.setBypassed (bypassButton.getToggleState()); };
    addAndMakeVisible (bypassButton);

    performanceHeaderLabel.setFont (juce::Font (11.0f, juce::Font::bold));
    performanceHeaderLabel.setColour (juce::Label::textColourId, juce::Colour (pitchzazz::colours::textSecondary));
    addAndMakeVisible (performanceHeaderLabel);

    latencyValueLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (latencyValueLabel);
    addAndMakeVisible (latencyMeter);
    latencyMeter.setFillColour (juce::Colour (pitchzazz::colours::accentMagenta));

    // Each stage gets its own neon accent from the shared palette
    // (PitchzazzLookAndFeel.h) rather than an arbitrary colour — keeps the
    // meters' colours consistent with the rest of the UI instead of being
    // a one-off choice made only here.
    detectMeter.setFillColour (juce::Colour (pitchzazz::colours::accentCyan));
    quantizeMeter.setFillColour (juce::Colour (pitchzazz::colours::accentAmber));
    shiftMeter.setFillColour (juce::Colour (pitchzazz::colours::accentGreen));

    for (auto* label : { &detectLabel, &quantizeLabel, &shiftLabel })
        label->setFont (juce::Font (13.0f));
    for (auto* label : { &detectValueLabel, &quantizeValueLabel, &shiftValueLabel, &totalValueLabel })
    {
        label->setJustificationType (juce::Justification::centredLeft);
        label->setFont (juce::Font (13.0f));
    }

    addAndMakeVisible (detectLabel);
    addAndMakeVisible (detectMeter);
    addAndMakeVisible (detectValueLabel);
    addAndMakeVisible (quantizeLabel);
    addAndMakeVisible (quantizeMeter);
    addAndMakeVisible (quantizeValueLabel);
    addAndMakeVisible (shiftLabel);
    addAndMakeVisible (shiftMeter);
    addAndMakeVisible (shiftValueLabel);
    addAndMakeVisible (totalValueLabel);

    // Height computed from resized()'s layout sum: 566px of stacked
    // content (tonic/mode/engine rows, the two retune-control sliders,
    // the pitch displays, the cents meter, bypass, the divider, and the
    // three performance meters) plus the 16px top/bottom margins from
    // getLocalBounds().reduced(16) — kept in sync with resized() by hand
    // since JUCE has no layout-measurement API to derive this from.
    setSize (320, 598);

    updateRetuneControlsEnablement();

    // 10Hz: fast enough to read as "live" for a demo, slow enough not to
    // burden the message thread — this is a UI poll, not audio-path code,
    // so there's no real-time-safety constraint on the rate itself.
    startTimerHz (10);
    timerCallback(); // show real numbers immediately rather than "-" until the first tick
}

PitchzazzAudioProcessorEditor::~PitchzazzAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr); // must clear before lookAndFeel member is destroyed
}

void PitchzazzAudioProcessorEditor::timerCallback()
{
    updateRetuneControlsEnablement();

    // EMA smoothing — see the class doc for why this over a windowed
    // average. Applied before anything derived from these values (labels
    // and meters alike) so the two stay consistent with each other.
    //
    // Right after an engine switch, blending would mix the outgoing
    // engine's numbers into the incoming one's for the next second or
    // two (emaAlpha's ~0.5s time constant) — a transitional reading that
    // belongs to neither engine and would misrepresent both. Snap
    // straight to the raw reading for exactly one tick instead, then
    // resume normal smoothing from that point on.
    const float rawLatencyMs = (float) processorRef.getActiveLatencyMs();
    const float rawDetectUs = (float) processorRef.getLastDetectUs();
    const float rawQuantizeUs = (float) processorRef.getLastQuantizeUs();
    const float rawShiftUs = (float) processorRef.getLastShiftUs();

    if (resetSmoothingOnNextTick)
    {
        smoothedLatencyMs = rawLatencyMs;
        smoothedDetectUs = rawDetectUs;
        smoothedQuantizeUs = rawQuantizeUs;
        smoothedShiftUs = rawShiftUs;
        resetSmoothingOnNextTick = false;
        centsWasVoiced = false; // force the cents meter to snap below instead of blending across the engine switch
    }
    else
    {
        smoothedLatencyMs += emaAlpha * (rawLatencyMs - smoothedLatencyMs);
        smoothedDetectUs += emaAlpha * (rawDetectUs - smoothedDetectUs);
        smoothedQuantizeUs += emaAlpha * (rawQuantizeUs - smoothedQuantizeUs);
        smoothedShiftUs += emaAlpha * (rawShiftUs - smoothedShiftUs);
    }

    latencyValueLabel.setText ("Latency: " + juce::String (smoothedLatencyMs, 1) + "ms", juce::dontSendNotification);
    // Fixed 100ms scale: current engines measure 43-50ms (docs/PERFORMANCE_LOG.md),
    // so this gives visible headroom either side without needing to be
    // dynamically rescaled.
    latencyMeter.setValue (smoothedLatencyMs / 100.0f);

    // All three meters share the budget as their scale (rather than each
    // auto-scaling to its own range) — see the class doc for why: it's
    // what makes shift's dominance over detect/quantize visible instead
    // of hidden.
    const float budgetUs = (float) processorRef.getBudgetUs();

    detectValueLabel.setText (juce::String (smoothedDetectUs, 0) + "us", juce::dontSendNotification);
    detectMeter.setValue (budgetUs > 0.0f ? smoothedDetectUs / budgetUs : 0.0f);

    quantizeValueLabel.setText (juce::String (smoothedQuantizeUs, 0) + "us", juce::dontSendNotification);
    quantizeMeter.setValue (budgetUs > 0.0f ? smoothedQuantizeUs / budgetUs : 0.0f);

    shiftValueLabel.setText (juce::String (smoothedShiftUs, 0) + "us", juce::dontSendNotification);
    shiftMeter.setValue (budgetUs > 0.0f ? smoothedShiftUs / budgetUs : 0.0f);

    const float smoothedTotalUs = smoothedDetectUs + smoothedQuantizeUs + smoothedShiftUs;
    totalValueLabel.setText ("Total: " + juce::String (smoothedTotalUs, 0) + "us / " + juce::String (budgetUs, 0) + "us budget",
                              juce::dontSendNotification);

    // Deliberately not EMA-smoothed like the numbers above: this is a
    // live musical reading, not a noisy per-block cost measurement, and
    // smoothing it would visually manufacture a fake glide/portamento
    // between notes (and into/out of silence) that never actually
    // happened in the audio. Shown as-is, block to block.
    const float detectedHz = processorRef.getLastDetectedHz();
    detectedPitchDisplay.setValueText (noteNameAndHz (detectedHz));

    const float correctedHz = detectedHz > 0.0f
        ? detectedHz * std::pow (2.0f, processorRef.getLastSemitoneShift() / 12.0f)
        : 0.0f;
    correctedPitchDisplay.setValueText (noteNameAndHz (correctedHz));

    // Cents meter — EMA-smoothed (see the class doc for why this differs
    // from the unsmoothed note-name text above), but only while voiced
    // continuously: on voicing onset (silence -> a new note, or right
    // after an engine switch, per resetSmoothingOnNextTick above) snap
    // straight to the raw reading for one tick rather than blending from
    // a stale previous value that belongs to a different note entirely.
    const bool hasPitch = detectedHz > 0.0f;
    if (hasPitch)
    {
        const float rawCents = pitchzazz::centsOffsetFromNearestNote (detectedHz);
        smoothedCents = centsWasVoiced ? smoothedCents + emaAlpha * (rawCents - smoothedCents) : rawCents;
    }
    centsWasVoiced = hasPitch;
    detectedCentsMeter.setCentsOffset (smoothedCents, hasPitch);
}

void PitchzazzAudioProcessorEditor::updateRetuneControlsEnablement()
{
    // A slider's own onValueChange doesn't fire from setEnabled(), so
    // toggling this can't accidentally push a stale value to the
    // processor — it only ever changes whether the *next* user drag does
    // anything.
    const bool enabled = processorRef.activeEngineSupportsRetuneControls();
    correctionAmountSlider.setEnabled (enabled);
    retuneSpeedSlider.setEnabled (enabled);
}

void PitchzazzAudioProcessorEditor::updateProcessorScale()
{
    const int tonicPitchClass = tonicBox.getSelectedId() - 1;
    const int modeIndex = modeBox.getSelectedId() - 1;
    const auto mode = (modeIndex >= 0 && modeIndex < 7) ? modeEntries[modeIndex].mode : pitchzazz::ScaleMode::major;
    processorRef.setScale ({ tonicPitchClass, mode });
}

void PitchzazzAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colour (pitchzazz::colours::accentCyan));
    g.setFont (juce::Font (22.0f, juce::Font::bold));
    g.drawFittedText ("Pitchzazz", getLocalBounds().removeFromTop (40), juce::Justification::centred, 1);

    // Thin accent bar under the title ties the brand mark to the same
    // neon palette the meters use, rather than leaving it a plain white
    // heading disconnected from the rest of the UI.
    g.setColour (juce::Colour (pitchzazz::colours::accentMagenta));
    g.fillRect (getLocalBounds().withTop (40).withHeight (2).reduced (72, 0));

    if (dividerY > 0)
    {
        g.setColour (juce::Colour (pitchzazz::colours::outline));
        g.fillRect (getLocalBounds().withTop (dividerY).withHeight (1).reduced (16, 0));
    }
}

void PitchzazzAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (44); // title

    auto tonicRow = area.removeFromTop (28);
    tonicLabel.setBounds (tonicRow.removeFromLeft (60));
    tonicBox.setBounds (tonicRow);

    area.removeFromTop (8);

    auto modeRow = area.removeFromTop (28);
    modeLabel.setBounds (modeRow.removeFromLeft (60));
    modeBox.setBounds (modeRow);

    area.removeFromTop (8);

    auto engineRow = area.removeFromTop (28);
    engineLabel.setBounds (engineRow.removeFromLeft (60));
    engineBox.setBounds (engineRow);

    area.removeFromTop (8);

    auto correctionAmountRow = area.removeFromTop (28);
    correctionAmountLabel.setBounds (correctionAmountRow.removeFromLeft (60));
    correctionAmountSlider.setBounds (correctionAmountRow);

    area.removeFromTop (8);

    auto retuneSpeedRow = area.removeFromTop (28);
    retuneSpeedLabel.setBounds (retuneSpeedRow.removeFromLeft (60));
    retuneSpeedSlider.setBounds (retuneSpeedRow);

    area.removeFromTop (16);
    auto pitchDisplayRow = area.removeFromTop (54);
    detectedPitchDisplay.setBounds (pitchDisplayRow.removeFromLeft (pitchDisplayRow.getWidth() / 2 - 4));
    pitchDisplayRow.removeFromLeft (8);
    correctedPitchDisplay.setBounds (pitchDisplayRow);

    area.removeFromTop (4);
    auto centsMeterRow = area.removeFromTop (16);
    // Full row width, spanning both DETECTED and CORRECTED above: this is
    // a read on the same "how is the pitch behaving right now" question
    // those two boxes answer together, not just a footnote on DETECTED
    // alone, so it reads as belonging to the pair rather than one half.
    detectedCentsMeter.setBounds (centsMeterRow);

    area.removeFromTop (12);
    bypassButton.setBounds (area.removeFromTop (28));

    area.removeFromTop (20);
    dividerY = area.getY();
    area.removeFromTop (12);

    performanceHeaderLabel.setBounds (area.removeFromTop (16));
    area.removeFromTop (8);
    latencyValueLabel.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);
    latencyMeter.setBounds (area.removeFromTop (14));

    area.removeFromTop (16);
    auto layoutStageRow = [&area] (juce::Label& stageLabel, pitchzazz::LevelMeter& meter, juce::Label& valueLabel)
    {
        auto row = area.removeFromTop (20);
        stageLabel.setBounds (row.removeFromLeft (64));
        valueLabel.setBounds (row.removeFromRight (70));
        meter.setBounds (row.reduced (4, 0));
        area.removeFromTop (6);
    };
    layoutStageRow (detectLabel, detectMeter, detectValueLabel);
    layoutStageRow (quantizeLabel, quantizeMeter, quantizeValueLabel);
    layoutStageRow (shiftLabel, shiftMeter, shiftValueLabel);

    area.removeFromTop (8);
    totalValueLabel.setBounds (area.removeFromTop (20));
}
