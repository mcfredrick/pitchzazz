#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace pitchzazz
{

/// A minimal horizontal level bar, filled proportionally to a 0..1 value.
/// Deliberately plain (flat fill, no gradients/styling) — the visual
/// reskin is separate, later work (docs/ROADMAP.md Phase 5); this exists
/// to make magnitude visible at a glance, which a live-updating number in
/// text alone doesn't do well.
class LevelMeter : public juce::Component
{
public:
    void setValue (float newValue01) noexcept
    {
        const float clamped = juce::jlimit (0.0f, 1.0f, newValue01);
        if (clamped != value)
        {
            value = clamped;
            repaint();
        }
    }

    /// Lets callers colour-code multiple meters sharing one scale (e.g.
    /// the detect/quantize/shift trio) so they stay visually
    /// distinguishable at a glance, not just by their labels.
    void setFillColour (juce::Colour newColour) noexcept
    {
        fillColour = newColour;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float cornerSize = juce::jmin (bounds.getHeight() * 0.5f, 4.0f);

        g.setColour (juce::Colour (0xff1b1e27));
        g.fillRoundedRectangle (bounds, cornerSize);

        if (value > 0.0f)
        {
            g.setColour (fillColour);
            g.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * value), cornerSize);
        }

        // A visible-even-at-zero coloured outline (rather than fill alone)
        // is what makes each meter read as its own neon-accented element
        // instead of a plain grey bar with a coloured fill dropped in.
        g.setColour (fillColour.withAlpha (0.4f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.0f);
    }

private:
    float value = 0.0f;
    juce::Colour fillColour = juce::Colours::lightblue;
};

/// An "LCD"-style readout: a small caption over a large monospace value,
/// on a dark recessed panel with a neon-accent border — reuses the same
/// dark+neon palette PitchzazzLookAndFeel.h already establishes rather
/// than inventing a separate look for just this one component. For the
/// GUI's live detected/corrected pitch display (docs/ROADMAP.md Phase 5).
class LCDDisplay : public juce::Component
{
public:
    void setCaption (const juce::String& newCaption)
    {
        if (caption != newCaption)
        {
            caption = newCaption;
            repaint();
        }
    }

    void setValueText (const juce::String& newValue)
    {
        if (value != newValue)
        {
            value = newValue;
            repaint();
        }
    }

    void setAccentColour (juce::Colour newColour) noexcept
    {
        accentColour = newColour;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float cornerSize = 6.0f;

        // Darker than the surrounding panel background (colours::panel,
        // 0x1b1e27) so this reads as recessed "glass" rather than a flat
        // label sitting on the same surface as everything else.
        g.setColour (juce::Colour (0xff0a0b10));
        g.fillRoundedRectangle (bounds, cornerSize);
        g.setColour (accentColour.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.2f);

        auto area = bounds.reduced (10.0f, 4.0f);

        g.setColour (accentColour.withAlpha (0.75f));
        g.setFont (juce::Font (11.0f, juce::Font::bold));
        g.drawText (caption, area.removeFromTop (16.0f), juce::Justification::centredLeft);

        g.setColour (accentColour);
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 23.0f, juce::Font::bold));
        g.drawText (value, area, juce::Justification::centredLeft);
    }

private:
    juce::String caption;
    juce::String value { "--" };
    juce::Colour accentColour = juce::Colours::lightblue;
};

} // namespace pitchzazz
