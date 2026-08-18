#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
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

/// A tuner-style "how close/far is the input from the nearest chromatic
/// note" gauge — the fine-grained sibling of an LCDDisplay note-name
/// readout: that display rounds the continuous detected pitch to the
/// nearest MIDI note for its text, and this shows the sub-semitone
/// deviation that rounding discards (pitchzazz::centsOffsetFromNearestNote,
/// Corrector.h), always within +-50 cents by construction. A thin
/// horizontal strip rather than Auto-Tune-style full circular dial —
/// this project's plugin window is compact, and the same "0 in the
/// centre, ticks either side, a coloured indicator showing the offset"
/// information reads fine linearly at this size.
class CentsMeter : public juce::Component
{
public:
    void setAccentColour (juce::Colour newColour) noexcept
    {
        accentColour = newColour;
        repaint();
    }

    /// Colour shown once the offset is within a "close enough" tolerance
    /// (+-5 cents — the rough band hardware chromatic tuners typically
    /// light an in-tune LED at), distinct from accentColour so the reading
    /// itself communicates "in tune" without requiring a numeric label.
    void setInTuneColour (juce::Colour newColour) noexcept
    {
        inTuneColour = newColour;
        repaint();
    }

    /// `cents` is clamped to [-50, 50] defensively (callers should already
    /// be passing a value in that range — see centsOffsetFromNearestNote's
    /// doc — but a display component shouldn't trust that as an
    /// invariant). `hasPitch` false (silence/unvoiced) dims the whole
    /// meter and centres the indicator rather than drawing it at 0 cents:
    /// 0 at full brightness would read as "singing exactly in tune" when
    /// there is actually no signal to measure at all, a materially
    /// different and more misleading state than being off-pitch.
    void setCentsOffset (float cents, bool hasPitch) noexcept
    {
        const float clamped = juce::jlimit (-50.0f, 50.0f, cents);
        if (clamped != value || hasPitch != voiced)
        {
            value = clamped;
            voiced = hasPitch;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float cornerSize = juce::jmin (bounds.getHeight() * 0.4f, 4.0f);
        const float dim = voiced ? 1.0f : 0.35f;

        g.setColour (juce::Colour (0xff0a0b10).withAlpha (dim));
        g.fillRoundedRectangle (bounds, cornerSize);
        g.setColour (accentColour.withAlpha (0.35f * dim));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.0f);

        const float centreX = bounds.getCentreX();
        const float halfWidth = bounds.getWidth() * 0.5f - 3.0f; // inset so extreme ticks stay inside the border

        g.setColour (accentColour.withAlpha (0.4f * dim));
        for (float tickCents : { -50.0f, -25.0f, 0.0f, 25.0f, 50.0f })
        {
            const float x = centreX + (tickCents / 50.0f) * halfWidth;
            const float tickHalfHeight = (tickCents == 0.0f) ? bounds.getHeight() * 0.5f : bounds.getHeight() * 0.25f;
            g.drawLine (x, bounds.getCentreY() - tickHalfHeight, x, bounds.getCentreY() + tickHalfHeight, 1.0f);
        }

        const float shownValue = voiced ? value : 0.0f;
        const float needleX = centreX + (shownValue / 50.0f) * halfWidth;
        const bool inTune = voiced && std::abs (value) <= 5.0f;
        const juce::Colour needleColour = (inTune ? inTuneColour : accentColour).withAlpha (dim);

        if (voiced)
        {
            // A filled span from centre to the needle, echoing the
            // radial-bars-lit-from-zero look of the hardware/plugin tuner
            // displays this component is modelled on, adapted to a linear
            // gauge instead of a circular one.
            g.setColour (needleColour.withAlpha (0.3f * dim));
            g.fillRect (juce::Rectangle<float> (juce::jmin (centreX, needleX), bounds.getY() + 2.0f,
                                                 std::abs (needleX - centreX), bounds.getHeight() - 4.0f));
        }

        g.setColour (needleColour);
        g.drawLine (needleX, bounds.getY() + 1.0f, needleX, bounds.getBottom() - 1.0f, 2.0f);
    }

private:
    float value = 0.0f;
    bool voiced = false;
    juce::Colour accentColour = juce::Colours::lightblue;
    juce::Colour inTuneColour = juce::Colours::lightgreen;
};

} // namespace pitchzazz
