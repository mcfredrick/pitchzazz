#pragma once

#include "DSP/Corrector.h"
#include "DSP/Scale.h"
#include "DSP/ScopeCapture.h"
#include "DSP/ScopeFrameRecorder.h"
#include "PitchzazzLookAndFeel.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>
#include <functional>
#include <utility>
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

/// Live "before/after, one period" scope (docs/ROADMAP.md's waveform-
/// visualization item): overlays the most recent captured input (dim)
/// against the corresponding output (accent-coloured) from
/// ScopeCapture, whichever engine is currently active. Runs its own
/// independent Timer rather than piggybacking on the editor's shared
/// 10Hz one -- that timer's EMA smoothing constant is tuned specifically
/// for its own rate (PitchzazzAudioProcessorEditor's class doc), and a
/// waveform reads better at a faster, entirely unsmoothed refresh than a
/// numeric meter needs.
class ScopeComponent : public juce::Component,
                        private juce::Timer
{
public:
    /// `captureSourceIn` is a callback rather than a stored reference so
    /// this component never has to assume the processor/worker are alive
    /// yet at construction -- matches PluginProcessor::getScopeCapture()'s
    /// own nullable-pointer contract (see that method's doc for why).
    explicit ScopeComponent (std::function<const ScopeCapture*()> captureSourceIn)
        : captureSource (std::move (captureSourceIn))
    {
        startTimerHz (30);
    }

    ~ScopeComponent() override { stopTimer(); }

    void setAccentColour (juce::Colour newColour) noexcept
    {
        accentColour = newColour;
        repaint();
    }

    /// Pauses picking up new data from ScopeCapture (the displayed traces
    /// hold at whatever they last showed) so a fast-scrolling artifact can
    /// actually be looked at -- does NOT pause frame recording (see
    /// timerCallback()'s doc for why holding the picture during a freeze is
    /// the *correct* thing for a recording of this component to show, not
    /// a gap to work around).
    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float cornerSize = juce::jmin (bounds.getHeight() * 0.08f, 6.0f);

        g.setColour (juce::Colour (0xff1b1e27));
        g.fillRoundedRectangle (bounds, cornerSize);
        g.setColour (juce::Colour (0xff2c3140));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.0f);

        if (sampleCount < 2)
        {
            g.setColour (juce::Colour (0xff8a8d9c).withAlpha (0.6f));
            g.setFont (11.0f);
            g.drawFittedText ("no signal", getLocalBounds(), juce::Justification::centred, 1);
            return;
        }

        // Stacked, not overlaid: BEFORE (input) on top, AFTER (output)
        // below, each with its own label and its own zero line -- overlaid
        // traces on a shared axis made it hard to tell which trace was
        // which at a glance and, worse, hid amplitude differences between
        // the two (a change in loudness reads as one trace shrinking
        // relative to the other, easy to miss when they're superimposed).
        // Stacking trades that ambiguity for a coarser per-trace amplitude
        // scale, which is the right trade for this scope's actual job: "did
        // the waveform's shape change," not "are the two exactly the same
        // amplitude."
        auto inner = bounds.reduced (6.0f);
        constexpr float rowGap = 8.0f;
        const float rowHeight = (inner.getHeight() - rowGap) * 0.5f;
        auto inputRow = inner.removeFromTop (rowHeight);
        inner.removeFromTop (rowGap);
        auto outputRow = inner;

        g.setColour (juce::Colour (0xff2c3140));
        g.drawLine (inputRow.getX(), inputRow.getBottom(), inputRow.getRight(), inputRow.getBottom(), 1.0f);

        const auto drawRow = [&] (juce::Rectangle<float> row, const juce::String& label,
                                   const std::array<float, ScopeCapture::captureLength>& data,
                                   juce::Colour colour, float traceAlpha)
        {
            g.setColour (colour.withAlpha (0.85f));
            g.setFont (juce::Font (9.5f, juce::Font::bold));
            g.drawText (label, row.removeFromTop (12.0f).toNearestInt(), juce::Justification::topLeft);

            const float midY = row.getCentreY();
            const float ampScale = row.getHeight() * 0.5f - 1.0f;

            g.setColour (juce::Colour (0xff2c3140));
            g.drawLine (row.getX(), midY, row.getRight(), midY, 1.0f);

            juce::Path path;
            for (int i = 0; i < sampleCount; ++i)
            {
                const float x = row.getX() + (float) i / (float) (sampleCount - 1) * row.getWidth();
                const float y = midY - data[(size_t) i] * ampScale;
                if (i == 0)
                    path.startNewSubPath (x, y);
                else
                    path.lineTo (x, y);
            }
            g.setColour (colour.withAlpha (traceAlpha));
            g.strokePath (path, juce::PathStrokeType (1.4f));
        };

        drawRow (inputRow, "BEFORE", inputSnapshot, juce::Colour (0xff8a8d9c), 0.9f);
        drawRow (outputRow, "AFTER", outputSnapshot, accentColour, 0.95f);
    }

    /// Starts writing this component's rendered frames (30Hz, matching
    /// the Timer below) into `sessionDir` as a PNG sequence + manifest --
    /// the video half of a scope recording, muxed with
    /// AudioSessionRecorder's WAV output afterwards by
    /// scripts/stitch_recording.sh. See ScopeFrameRecorder's doc for why
    /// that's an offline step rather than live video encoding.
    bool startRecordingFrames (const juce::File& sessionDir) { return frameRecorder.start (sessionDir); }
    void stopRecordingFrames() { frameRecorder.stop(); }
    [[nodiscard]] bool isRecordingFrames() const noexcept { return frameRecorder.isRecording(); }

private:
    void timerCallback() override
    {
        // Frozen: skip picking up new data entirely, so the displayed
        // traces hold still -- but frame recording (below) still runs
        // unconditionally, capturing exactly what's on screen. That's the
        // right behaviour, not a gap to work around: this records the
        // scope, and a frozen scope showing a held picture is what the
        // scope is doing during that span, same as a video camera holding
        // on a paused subject.
        if (! frozen)
        {
            const ScopeCapture* capture = captureSource ? captureSource() : nullptr;
            if (capture != nullptr)
            {
                std::array<float, ScopeCapture::captureLength> in {}, out {};
                int n = 0;
                // A false return just means "a write landed mid-copy, try
                // again next tick" (ScopeCapture.h's doc) -- not an error,
                // and at 30Hz against a ~21.5Hz block rate the next tick
                // almost always succeeds, so silently keeping last frame's
                // picture is the right behaviour, not a fallback worth
                // flagging.
                if (capture->read (in, out, n))
                {
                    inputSnapshot = in;
                    outputSnapshot = out;
                    sampleCount = n;
                    repaint();
                }
            }
        }

        // Pushed every tick, not just on a successful read above -- a
        // steady 30Hz frame cadence makes for a normal-looking video even
        // across a tick where the scope's own data didn't change (the
        // frame is then a duplicate of the previous one, same as a video
        // camera holding on a static subject); frames_manifest.csv still
        // carries each frame's real elapsed time regardless.
        if (frameRecorder.isRecording())
            frameRecorder.pushFrame (createComponentSnapshot (getLocalBounds()));
    }

    bool frozen = false;
    std::function<const ScopeCapture*()> captureSource;
    std::array<float, ScopeCapture::captureLength> inputSnapshot {};
    std::array<float, ScopeCapture::captureLength> outputSnapshot {};
    int sampleCount = 0;
    juce::Colour accentColour = juce::Colours::lightgreen;
    ScopeFrameRecorder frameRecorder;
};

/// Live "before/after" magnitude spectrum, the frequency-domain sibling of
/// ScopeComponent -- reuses the exact same ScopeCapture snapshots (a
/// second, independent read each tick; ScopeCapture's seqlock supports any
/// number of concurrent readers, see its class doc). Built specifically to
/// chase the carrier-wave/AM-looking pattern spotted in the time-domain
/// scope: amplitude modulation shows up as symmetric sidebands at
/// carrier +/- modulator Hz, which is unambiguous on a magnitude spectrum
/// and easy to miss by eye in the time domain. A *linear*, not
/// logarithmic, frequency axis is deliberate here even though log is the
/// usual audio convention -- sidebands are evenly spaced in absolute Hz
/// around the carrier, and a linear axis preserves that even spacing,
/// where a log axis would visually compress and distort it.
class SpectrumComponent : public juce::Component,
                           private juce::Timer
{
public:
    explicit SpectrumComponent (std::function<const ScopeCapture*()> captureSourceIn,
                                 std::function<double()> sampleRateSourceIn)
        : captureSource (std::move (captureSourceIn)),
          sampleRateSource (std::move (sampleRateSourceIn)),
          fft (fftOrder),
          window (fftSize, juce::dsp::WindowingFunction<float>::hann)
    {
        startTimerHz (30);
    }

    ~SpectrumComponent() override { stopTimer(); }

    void setAccentColour (juce::Colour newColour) noexcept
    {
        accentColour = newColour;
        repaint();
    }

    /// Same contract as ScopeComponent::setFrozen -- see that doc.
    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float cornerSize = juce::jmin (bounds.getHeight() * 0.08f, 6.0f);

        g.setColour (juce::Colour (0xff1b1e27));
        g.fillRoundedRectangle (bounds, cornerSize);
        g.setColour (juce::Colour (0xff2c3140));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.0f);

        if (! hasData)
        {
            g.setColour (juce::Colour (0xff8a8d9c).withAlpha (0.6f));
            g.setFont (11.0f);
            g.drawFittedText ("no signal", getLocalBounds(), juce::Justification::centred, 1);
            return;
        }

        auto inner = bounds.reduced (6.0f);

        // A frequency-axis tick strip along the bottom, shared by both
        // rows (one x-axis, since both cover the same 0..maxDisplayHz
        // range) -- this plus the filled-area fill below are this
        // component's two biggest visual differences from ScopeComponent's
        // line-only traces: without them the two panels read as near-
        // identical stacked line charts, easy to confuse at a glance
        // despite showing entirely different domains (time vs. frequency).
        auto axisStrip = inner.removeFromBottom (11.0f);
        inner.removeFromBottom (2.0f);

        constexpr float rowGap = 8.0f;
        const float rowHeight = (inner.getHeight() - rowGap) * 0.5f;
        auto inputRow = inner.removeFromTop (rowHeight);
        inner.removeFromTop (rowGap);
        auto outputRow = inner;

        // Bin count actually shown: capped to maxDisplayHz rather than the
        // full Nyquist range, since a vocal fundamental plus several
        // harmonics and any PSOLA-relevant sideband spacing all live well
        // under this -- showing the full range (e.g. up to 48kHz at a
        // 96kHz session) would waste most of the plot on empty spectrum.
        const double sampleRate = sampleRateSource ? sampleRateSource() : 44100.0;
        const double hzPerBin = sampleRate / (double) fftSize;
        const int binsToShow = juce::jlimit (1, fftSize / 2, (int) (maxDisplayHz / hzPerBin));

        const auto drawRow = [&] (juce::Rectangle<float> row, const juce::String& label,
                                   const std::array<float, fftSize / 2>& magnitudesDb, juce::Colour colour)
        {
            g.setColour (colour.withAlpha (0.85f));
            g.setFont (juce::Font (9.5f, juce::Font::bold));
            g.drawText (label, row.removeFromTop (12.0f).toNearestInt(), juce::Justification::topLeft);

            // dBFloor..0dB maps to the row's full height -- a fixed range
            // (not auto-scaled to the current peak) so loudness changes
            // between BEFORE and AFTER, or between ticks, are visible as
            // real vertical movement rather than being normalized away.
            const auto dbToY = [&] (float db)
            {
                const float t = juce::jlimit (0.0f, 1.0f, (db - dbFloor) / -dbFloor);
                return row.getBottom() - t * row.getHeight();
            };

            // Faint vertical gridlines at each 1kHz mark -- reinforces the
            // frequency axis even without repeating tick labels on every
            // row (only the bottom axisStrip below carries the numbers).
            g.setColour (juce::Colour (0xff2c3140));
            for (float hz = 1000.0f; hz < maxDisplayHz; hz += 1000.0f)
            {
                const float x = row.getX() + hz / maxDisplayHz * row.getWidth();
                g.drawLine (x, row.getY(), x, row.getBottom(), 1.0f);
            }

            juce::Path line;
            for (int bin = 0; bin < binsToShow; ++bin)
            {
                const float x = row.getX() + (float) bin / (float) (binsToShow - 1) * row.getWidth();
                const float y = dbToY (magnitudesDb[(size_t) bin]);
                if (bin == 0)
                    line.startNewSubPath (x, y);
                else
                    line.lineTo (x, y);
            }

            // Filled area under the curve -- the standard spectrum-analyzer
            // convention (a scope never fills under its trace, since "area
            // under a waveform" isn't a meaningful quantity the way "energy
            // at this frequency" is), faded top-to-bottom so it reads as
            // magnitude falling away rather than a flat block of colour.
            juce::Path fill (line);
            fill.lineTo (row.getRight(), row.getBottom());
            fill.lineTo (row.getX(), row.getBottom());
            fill.closeSubPath();
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.45f), row.getX(), row.getY(),
                                                       colour.withAlpha (0.03f), row.getX(), row.getBottom(), false));
            g.fillPath (fill);

            g.setColour (colour.withAlpha (0.95f));
            g.strokePath (line, juce::PathStrokeType (1.2f));
        };

        drawRow (inputRow, "BEFORE", inputMagnitudesDb, juce::Colour (0xff8a8d9c));
        drawRow (outputRow, "AFTER", outputMagnitudesDb, accentColour);

        g.setColour (juce::Colour (0xff8a8d9c).withAlpha (0.7f));
        g.setFont (juce::Font (8.5f));
        for (float hz = 0.0f; hz <= maxDisplayHz + 1.0f; hz += 1000.0f)
        {
            const float x = axisStrip.getX() + hz / maxDisplayHz * axisStrip.getWidth();
            const juce::String tickLabel = hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : "0";
            g.drawText (tickLabel, juce::Rectangle<float> (x - 16.0f, axisStrip.getY(), 32.0f, axisStrip.getHeight()).toNearestInt(),
                        juce::Justification::centred);
        }
    }

private:
    // Declared first in the private section, ahead of computeMagnitudeDb()
    // below -- a member function's *own parameter list* (unlike its body)
    // only sees names already declared earlier in the class, so fftSize
    // has to appear before the function that names it in a parameter type.
    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder; // 1024 -- see computeMagnitudeDb's doc for why this exceeds captureLength
    static constexpr float maxDisplayHz = 5000.0f;
    static constexpr float dbFloor = -80.0f;

    void timerCallback() override
    {
        if (frozen)
            return;

        const ScopeCapture* capture = captureSource ? captureSource() : nullptr;
        if (capture == nullptr)
            return;

        std::array<float, ScopeCapture::captureLength> in {}, out {};
        int n = 0;
        if (capture->read (in, out, n) && n > 1)
        {
            computeMagnitudeDb (in, n, inputMagnitudesDb);
            computeMagnitudeDb (out, n, outputMagnitudesDb);
            hasData = true;
            repaint();
        }
    }

    /// `data` holds `n` real samples (n <= fftSize, ScopeCapture's own
    /// captureLength=800 vs. this class's 1024-point transform) -- zero-
    /// padded up to fftSize before windowing/transforming. That doesn't add
    /// true frequency resolution beyond what 800 samples actually carries
    /// (~sampleRate/800, roughly 55Hz at 44.1kHz) but does interpolate a
    /// smoother-looking curve between the true bins, the same reason any
    /// spectrum analyzer zero-pads a short capture window.
    void computeMagnitudeDb (const std::array<float, ScopeCapture::captureLength>& data, int n,
                              std::array<float, fftSize / 2>& outDb)
    {
        std::array<float, fftSize * 2> fftData {};
        const int m = juce::jmin (n, fftSize);
        for (int i = 0; i < m; ++i)
            fftData[(size_t) i] = data[(size_t) i];

        window.multiplyWithWindowingTable (fftData.data(), fftSize);
        fft.performRealOnlyForwardTransform (fftData.data());

        for (int bin = 0; bin < fftSize / 2; ++bin)
        {
            const float re = fftData[(size_t) (2 * bin)];
            const float im = fftData[(size_t) (2 * bin + 1)];
            const float mag = std::sqrt (re * re + im * im) / (float) fftSize;
            outDb[(size_t) bin] = 20.0f * std::log10 (juce::jmax (mag, 1.0e-7f));
        }
    }

    std::function<const ScopeCapture*()> captureSource;
    std::function<double()> sampleRateSource;
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;

    bool frozen = false;
    bool hasData = false;
    std::array<float, fftSize / 2> inputMagnitudesDb {};
    std::array<float, fftSize / 2> outputMagnitudesDb {};
    juce::Colour accentColour = juce::Colours::lightgreen;
};

/// Live scale-quantization visual, styled as a one-octave piano keyboard:
/// keys that are members of the active scale glow (bright white keys,
/// coloured black keys); non-members read as "off" (dim/unlit). The
/// plugin-GUI counterpart of the pitch-class-ring diagram in the
/// companion GitHub Pages notebook (docs/ALGORITHMS.md) -- same
/// filled-vs-hollow membership idea, reshaped as a keyboard since that's
/// the more immediately legible convention for a pitch-correction tool
/// specifically. An amber marker floats above the keys at the detected
/// pitch's exact (fractional) position, with a thin line down to
/// whichever key it's being snapped to (ringed in green, matching the
/// CORRECTED display's own accent) -- the same detected-vs-corrected
/// story the LCD displays and cents meter already tell, shown here
/// against the full chromatic context instead of one note at a time.
class ScaleKeyboardComponent : public juce::Component,
                                private juce::Timer
{
public:
    /// Four independent callbacks rather than one combined struct --
    /// matches every other live component in this file (ScopeComponent,
    /// SpectrumComponent), each pulling straight from PluginProcessor's
    /// own getters rather than this class assuming anything about how
    /// they're stored. `scaleNameSource` is supplied rather than computed
    /// here so this class doesn't need its own copy of the
    /// pitch-class-name/mode-name tables PluginEditor.cpp already owns.
    ScaleKeyboardComponent (std::function<Scale()> scaleSourceIn,
                             std::function<float()> detectedHzSourceIn,
                             std::function<float()> semitoneShiftSourceIn,
                             std::function<juce::String()> scaleNameSourceIn)
        : scaleSource (std::move (scaleSourceIn)),
          detectedHzSource (std::move (detectedHzSourceIn)),
          semitoneShiftSource (std::move (semitoneShiftSourceIn)),
          scaleNameSource (std::move (scaleNameSourceIn))
    {
        // Slower than the 30Hz scope/spectrum -- scale membership only
        // changes on a user action, and the detected-pitch marker reads
        // fine well below 30Hz, so there's no reason to repaint this one
        // as often.
        startTimerHz (15);
    }

    ~ScaleKeyboardComponent() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const float cornerSize = juce::jmin (bounds.getHeight() * 0.08f, 6.0f);

        g.setColour (juce::Colour (0xff1b1e27));
        g.fillRoundedRectangle (bounds, cornerSize);
        g.setColour (juce::Colour (0xff2c3140));
        g.drawRoundedRectangle (bounds.reduced (0.5f), cornerSize, 1.0f);

        auto area = bounds.reduced (8.0f, 6.0f);

        auto headerRow = area.removeFromTop (13.0f);
        g.setColour (juce::Colour (0xff8a8d9c).withAlpha (0.85f));
        g.setFont (juce::Font (9.5f, juce::Font::bold));
        g.drawText ("SCALE", headerRow.removeFromLeft (60.0f).toNearestInt(), juce::Justification::topLeft);
        g.setFont (juce::Font (9.5f));
        g.drawText (scaleName, headerRow.toNearestInt(), juce::Justification::topRight);

        area.removeFromTop (2.0f);
        auto markerStrip = area.removeFromTop (12.0f);
        auto keysArea = area;

        const float whiteW = keysArea.getWidth() / 7.0f;
        const float blackW = whiteW * 0.62f;
        const float blackH = keysArea.getHeight() * 0.6f;

        // Pitch class, white-key index (0..6). No black key between E-F
        // (2->3) or B-C (6->0 wraparound) -- the one irregularity in an
        // otherwise-repeating pattern that makes this a piano keyboard
        // rather than 12 evenly-spaced keys.
        static const int whitePitchClasses[7] = { 0, 2, 4, 5, 7, 9, 11 };
        // Pitch class, x-centre in whiteW units -- matches whitePitchClasses'
        // key positions (index + 0.5) and sits at the boundary between the
        // two white keys each black key is nestled between.
        static const std::pair<int, float> blackKeys[5] = { { 1, 1.0f }, { 3, 2.0f }, { 6, 4.0f }, { 8, 5.0f }, { 10, 6.0f } };

        for (int i = 0; i < 7; ++i)
        {
            const int pc = whitePitchClasses[i];
            const bool inScale = scale.containsPitchClass (pc);
            const juce::Rectangle<float> key (keysArea.getX() + (float) i * whiteW, keysArea.getY(), whiteW - 1.5f, keysArea.getHeight());

            g.setColour (inScale ? juce::Colour (0xfff1f1f6) : juce::Colour (0xff30333f));
            g.fillRoundedRectangle (key, 2.0f);
            g.setColour (juce::Colour (0xff12141a));
            g.drawRoundedRectangle (key, 2.0f, 1.0f);

            if (voiced && pc == correctedPitchClass)
            {
                g.setColour (juce::Colour (colours::accentGreen));
                g.drawRoundedRectangle (key.reduced (1.5f), 2.0f, 2.0f);
            }
        }

        // Black keys drawn after (on top of) white keys -- standard piano
        // z-order, since they visually sit above/between the white ones.
        for (const auto& blackKey : blackKeys)
        {
            const int pc = blackKey.first;
            const bool inScale = scale.containsPitchClass (pc);
            const juce::Rectangle<float> key (keysArea.getX() + blackKey.second * whiteW - blackW * 0.5f, keysArea.getY(), blackW, blackH);

            g.setColour (inScale ? juce::Colour (colours::accentCyan) : juce::Colour (0xff0a0b10));
            g.fillRoundedRectangle (key, 1.5f);
            g.setColour (juce::Colour (0xff12141a));
            g.drawRoundedRectangle (key, 1.5f, 1.0f);

            if (voiced && pc == correctedPitchClass)
            {
                g.setColour (juce::Colour (colours::accentGreen));
                g.drawRoundedRectangle (key.reduced (1.0f), 1.5f, 2.0f);
            }
        }

        if (voiced)
        {
            // Key-centre x positions, same units/order as whitePitchClasses/
            // blackKeys above -- used to interpolate the marker's exact
            // (fractional) x position, since a naive pc*width/12 would be
            // visually wrong (black keys aren't evenly spaced, so "pc 7.8"
            // has to interpolate between G's actual centre and G#'s, not a
            // uniform grid).
            static const float centresInWhiteW[12] = { 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.5f, 4.0f, 4.5f, 5.0f, 5.5f, 6.0f, 6.5f };

            const int lower = ((int) std::floor (detectedFrac) % 12 + 12) % 12;
            const int upper = (lower + 1) % 12;
            const float t = detectedFrac - std::floor (detectedFrac);
            // Wraps B (11) -> C (0): the next octave's C sits a further 7
            // whiteW to the right of this octave's, not back at the start.
            const float upperCentre = centresInWhiteW[upper] + (upper == 0 ? 7.0f : 0.0f);
            const float markerCentre = centresInWhiteW[lower] + t * (upperCentre - centresInWhiteW[lower]);

            const float markerX = keysArea.getX() + markerCentre * whiteW;
            const float markerY = markerStrip.getCentreY();
            const float targetX = keysArea.getX() + centresInWhiteW[correctedPitchClass] * whiteW;

            g.setColour (juce::Colour (colours::accentAmber).withAlpha (0.55f));
            g.drawLine (markerX, markerY + 3.0f, targetX, keysArea.getY(), 1.2f);

            g.setColour (juce::Colour (colours::accentAmber));
            constexpr float r = 3.2f;
            juce::Path diamond;
            diamond.addQuadrilateral (markerX, markerY - r, markerX + r, markerY, markerX, markerY + r, markerX - r, markerY);
            g.fillPath (diamond);
        }
    }

private:
    void timerCallback() override
    {
        scale = scaleSource ? scaleSource() : Scale {};
        scaleName = scaleNameSource ? scaleNameSource() : juce::String();

        const float hz = detectedHzSource ? detectedHzSource() : 0.0f;
        voiced = hz > 0.0f;
        if (voiced)
        {
            const float shift = semitoneShiftSource ? semitoneShiftSource() : 0.0f;
            const float detectedMidi = hzToMidi (hz);
            detectedFrac = std::fmod (std::fmod (detectedMidi, 12.0f) + 12.0f, 12.0f);
            const int correctedMidi = (int) std::lround (detectedMidi + shift);
            correctedPitchClass = ((correctedMidi % 12) + 12) % 12;
        }

        repaint();
    }

    std::function<Scale()> scaleSource;
    std::function<float()> detectedHzSource;
    std::function<float()> semitoneShiftSource;
    std::function<juce::String()> scaleNameSource;

    Scale scale;
    juce::String scaleName;
    bool voiced = false;
    float detectedFrac = 0.0f;
    int correctedPitchClass = 0;
};

} // namespace pitchzazz
