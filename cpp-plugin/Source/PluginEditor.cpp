#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Index 0 = C, matching pitchzazz::Scale::tonicPitchClass's convention.
    const char* const pitchClassNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                               "F#", "G", "G#", "A", "A#", "B" };
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

    modeBox.addItem ("Major", 1);
    modeBox.addItem ("Minor", 2);
    modeBox.setSelectedId (processorRef.getScale().mode == pitchzazz::ScaleMode::major ? 1 : 2,
                           juce::dontSendNotification);
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
        }
    };
    addAndMakeVisible (engineLabel);
    addAndMakeVisible (engineBox);

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

    setSize (320, 500);

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
}

void PitchzazzAudioProcessorEditor::updateProcessorScale()
{
    const int tonicPitchClass = tonicBox.getSelectedId() - 1;
    const auto mode = modeBox.getSelectedId() == 2 ? pitchzazz::ScaleMode::minor : pitchzazz::ScaleMode::major;
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

    area.removeFromTop (16);
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
