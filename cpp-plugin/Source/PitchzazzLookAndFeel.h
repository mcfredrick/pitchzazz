#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace pitchzazz
{

/// The plugin's colour palette (dark background, neon accents) — shared
/// between the LookAndFeel below and PluginEditor.cpp's hand-drawn
/// elements (title, divider, meters) so widget chrome and custom drawing
/// never drift out of sync with each other.
namespace colours
{
    constexpr juce::uint32 background    = 0xff12141a;
    constexpr juce::uint32 panel         = 0xff1b1e27;
    constexpr juce::uint32 outline       = 0xff2c3140;
    constexpr juce::uint32 textPrimary   = 0xfff1f1f6;
    constexpr juce::uint32 textSecondary = 0xff8a8d9c;
    constexpr juce::uint32 accentCyan    = 0xff00e5ff; // detect meter, interactive/focus accent
    constexpr juce::uint32 accentMagenta = 0xffff3fa4; // latency meter, brand/title accent
    constexpr juce::uint32 accentAmber   = 0xffffb020; // quantize meter
    constexpr juce::uint32 accentGreen   = 0xff39ff88; // shift meter, bypass "engaged" LED
}

/// LookAndFeel_V4's built-in ColourScheme (set via setColourScheme, below)
/// already maps 9 palette slots onto most standard-component ColourIds via
/// initialiseColours() — deliberately leaned on rather than setting every
/// ColourId by hand. A handful of extra setColour() calls plus two
/// overridden draw methods (a rounded neon-outlined combo box, and an LED-
/// style tick box for the bypass toggle) cover the rest. This is a styling
/// pass over the stock look and feel, not a bespoke widget-drawing
/// framework — deliberately scoped that way (docs/ROADMAP.md Phase 5's
/// "quick to do whenever it's prioritized" note on the reskin item).
class PitchzazzLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PitchzazzLookAndFeel()
    {
        setColourScheme ({
            colours::background,  // windowBackground
            colours::panel,       // widgetBackground
            colours::panel,       // menuBackground
            colours::outline,     // outline
            colours::textPrimary, // defaultText
            colours::accentCyan,  // defaultFill
            colours::background,  // highlightedText (dark, for contrast against a bright accent fill)
            colours::accentCyan,  // highlightedFill
            colours::textPrimary  // menuText
        });

        setColour (juce::ComboBox::outlineColourId, juce::Colour (colours::outline));
        setColour (juce::ComboBox::arrowColourId, juce::Colour (colours::accentCyan));
        setColour (juce::ToggleButton::tickColourId, juce::Colour (colours::accentGreen));
        setColour (juce::Label::textColourId, juce::Colour (colours::textPrimary));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (colours::accentCyan).withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (colours::textPrimary));
    }

    // Rounded, neon-outlined combo box — the stock LookAndFeel_V4 box is a
    // plain rectangle, which reads as "default JUCE" more than anything
    // else in the UI. This one override does most of the work of making
    // the control row look deliberately designed rather than scaffolded.
    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                        int, int, int, int, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (1.0f);
        const float cornerSize = juce::jmin (bounds.getHeight() * 0.3f, 8.0f);

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (bounds, cornerSize);

        g.setColour (juce::Colour (colours::accentCyan).withAlpha (isButtonDown ? 0.9f : 0.45f));
        g.drawRoundedRectangle (bounds, cornerSize, 1.4f);

        const auto arrowZone = bounds.removeFromRight (bounds.getHeight());
        juce::Path arrow;
        arrow.startNewSubPath (arrowZone.getX() + arrowZone.getWidth() * 0.25f, arrowZone.getCentreY() - 2.0f);
        arrow.lineTo (arrowZone.getCentreX(), arrowZone.getCentreY() + 3.0f);
        arrow.lineTo (arrowZone.getX() + arrowZone.getWidth() * 0.75f, arrowZone.getCentreY() - 2.0f);

        g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.3f));
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // A neon LED dot instead of the stock checkbox tick — bypass is the
    // one control most likely to get toggled live mid-demo, worth reading
    // clearly as on/off at a glance rather than as a small tick mark.
    void drawTickBox (juce::Graphics& g, juce::Component&,
                       float x, float y, float w, float h,
                       bool ticked, bool, bool, bool) override
    {
        const auto bounds = juce::Rectangle<float> (x, y, w, h).reduced (1.0f);

        g.setColour (juce::Colour (colours::outline));
        g.drawEllipse (bounds, 1.2f);

        if (ticked)
        {
            g.setColour (juce::Colour (colours::accentGreen));
            g.fillEllipse (bounds.reduced (2.5f));
            g.setColour (juce::Colour (colours::accentGreen).withAlpha (0.35f));
            g.drawEllipse (bounds.expanded (1.5f), 1.5f);
        }
        else
        {
            g.setColour (juce::Colour (colours::textSecondary).withAlpha (0.5f));
            g.fillEllipse (bounds.reduced (4.5f));
        }
    }
};

} // namespace pitchzazz
