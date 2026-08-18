#pragma once

#include "MeterComponents.h"
#include "PitchzazzLookAndFeel.h"
#include "PluginProcessor.h"
#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
/** Tonic + mode (major/minor) selection, an engine selector (Phase 3
    hot-swap — populated generically from pitchzazz::availableEngines()
    rather than hardcoding engine names, so a future third algorithm
    module needs no editor changes), a bypass toggle, and a live
    performance display (Phase 4): pipeline latency (a single number —
    NOT decomposed into "components," since block-accumulation delay and
    the phase vocoder's window don't stack additively, see
    docs/PERFORMANCE_LOG.md's measured-latency entry) plus a live
    per-stage processing-time breakdown (detect/quantize/shift — this
    *does* sum meaningfully, a genuinely different quantity from latency,
    see PluginProcessor::getActiveLatencyMs's doc), shown as three
    separate meters (MeterComponents.h) sharing one scale (the real-time
    budget) rather than each auto-scaled independently — auto-scaling
    each meter to its own range would visually equalize stages that are
    wildly different in magnitude (shift has always dominated detect and
    quantize by orders of magnitude in this project's own data,
    docs/PERFORMANCE_LOG.md), which would hide the actual finding instead
    of showing it. All values shown are smoothed with an exponential
    moving average — the raw per-block numbers are noisy enough
    tick-to-tick to be hard to read live otherwise; EMA was chosen over a
    windowed average because it needs no history buffer and is the same
    technique real level meters use.

    Visual style (added 2026-08-17): dark background with neon accent
    colours (`PitchzazzLookAndFeel.h`), replacing JUCE's default grey
    look and feel — a styling pass only, no layout/behavior changes,
    scoped per docs/ROADMAP.md Phase 5's reskin item.

    Live pitch display (added 2026-08-17): two LCD-style readouts
    (`pitchzazz::LCDDisplay`) showing the detected pitch and the
    corrected/quantized pitch it's being snapped to, each as a note name
    plus Hz. The corrected value isn't a separately-tracked quantity —
    it's derived here from `getLastDetectedHz()`/`getLastSemitoneShift()`
    (`detectedHz * 2^(semitoneShift/12)`), the same relationship
    `Corrector::process()` itself uses, rather than duplicating that math
    on the DSP side for a display-only value.

    Creative controls (added 2026-08-17): correction amount (0-100%,
    blends towards "no correction") and retune speed (0-1000ms glide time
    constant, 0 = instant snap) — docs/ROADMAP.md Phase 5's "classic
    Auto-Tune controls." Disabled, not hidden, when the active engine
    doesn't implement them (currently just the Rust engine — see
    PitchEngine.h) so the UI is honest about why they stopped doing
    anything rather than pretending they still apply.

    Grain width control (added 2026-08-17): a 0.5x-1.5x multiplier on
    PSOLA's grain half-width (docs/ROADMAP.md Phase 5, item 2 of the
    creative-parameter-exposure list) — PSOLA-only, unlike the two
    controls above, since it's a property of that specific algorithm
    (grain-based synthesis) with no equivalent in the phase vocoder or the
    Rust engine. Disabled on both of those, same "disabled not hidden"
    honesty pattern, just a narrower condition (only PSOLA enables it,
    where the two controls above are disabled only on Rust). The upper
    bound is capped at 1.5x rather than something more dramatic
    specifically to keep this engine's worst-case reported latency below
    the phase vocoder's — see grainWidthMultiplierMax's doc
    (PSOLAPitchShifter.h) for the numbers; a wider range would silently
    have defeated the latency advantage that's this engine's whole reason
    for existing.

    Tuner-style cents meter (added 2026-08-17, widened to the full row
    2026-08-17): a thin gauge spanning the full width under both the
    DETECTED and CORRECTED displays, showing
    pitchzazz::centsOffsetFromNearestNote — how sharp/flat the raw input
    is relative to the nearest chromatic note,
    i.e. the sub-semitone detail the DETECTED note name's own rounding
    discards. Deliberately measured against the nearest chromatic note,
    not the scale-corrected target semitoneShift represents — see that
    function's doc in Corrector.h for why. Lightly EMA-smoothed (same
    alpha as the performance meters below) — unlike the note-name text,
    which stays unsmoothed on purpose (a name is categorical; smoothing
    it would fabricate a fake glide between notes), a cents offset is
    continuous, so smoothing only damps block-to-block jitter without
    inventing anything, the same reasoning that justifies smoothing on a
    real hardware tuner's needle.
*/
class PitchzazzAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    explicit PitchzazzAudioProcessorEditor (PitchzazzAudioProcessor&);
    ~PitchzazzAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PitchzazzAudioProcessor& processorRef;
    pitchzazz::PitchzazzLookAndFeel lookAndFeel;

    juce::Label tonicLabel { {}, "Tonic" };
    juce::ComboBox tonicBox;
    juce::Label modeLabel { {}, "Mode" };
    juce::ComboBox modeBox;
    juce::ToggleButton bypassButton { "Bypass" };
    juce::Label engineLabel { {}, "Engine" };
    juce::ComboBox engineBox;

    // The classic Auto-Tune controls (docs/ROADMAP.md Phase 5). Disabled
    // (not hidden) on an engine that doesn't implement them — see
    // updateRetuneControlsEnablement()'s doc — so a user switching to the
    // Rust engine sees *why* the controls stopped doing anything rather
    // than the controls silently vanishing or silently no-oping.
    juce::Label correctionAmountLabel { {}, "Amount" };
    juce::Slider correctionAmountSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label retuneSpeedLabel { {}, "Smooth" };
    juce::Slider retuneSpeedSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // PSOLA-only grain-width control — see the class doc for why its
    // enablement condition is narrower than the two above.
    juce::Label grainWidthLabel { {}, "Width" };
    juce::Slider grainWidthSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    pitchzazz::LCDDisplay detectedPitchDisplay;
    pitchzazz::LCDDisplay correctedPitchDisplay;
    pitchzazz::CentsMeter detectedCentsMeter;

    juce::Label performanceHeaderLabel { {}, "PERFORMANCE" };
    juce::Label latencyValueLabel { {}, "-" };
    pitchzazz::LevelMeter latencyMeter;

    juce::Label detectLabel { {}, "Detect" };
    pitchzazz::LevelMeter detectMeter;
    juce::Label detectValueLabel { {}, "-" };
    juce::Label quantizeLabel { {}, "Quantize" };
    pitchzazz::LevelMeter quantizeMeter;
    juce::Label quantizeValueLabel { {}, "-" };
    juce::Label shiftLabel { {}, "Shift" };
    pitchzazz::LevelMeter shiftMeter;
    juce::Label shiftValueLabel { {}, "-" };
    juce::Label totalValueLabel { {}, "-" };

    // Exponential moving average state — see the class doc for why EMA
    // over a windowed average. alpha tuned for the 10Hz timer below:
    // roughly a 0.5s time constant, smooth without feeling laggy for a
    // demo.
    static constexpr float emaAlpha = 0.2f;
    float smoothedLatencyMs = 0.0f;
    float smoothedDetectUs = 0.0f;
    float smoothedQuantizeUs = 0.0f;
    float smoothedShiftUs = 0.0f;

    // EMA state for the cents meter, tracked separately from the timing
    // values above since it resets on a different condition (voicing
    // onset, not an engine switch) — see timerCallback()'s comment.
    float smoothedCents = 0.0f;
    bool centsWasVoiced = false;

    // Set true by the engine selector's onChange, consumed by the next
    // timerCallback() tick: without this, switching engines would EMA-
    // blend the outgoing engine's numbers into the incoming one's for the
    // next second or two, showing a misleading transitional reading that
    // belongs to neither engine — the smoothing state itself carries no
    // memory of *which* engine it was measuring.
    bool resetSmoothingOnNextTick = false;

    // Set by resized(), read by paint() — the divider line between the
    // controls and performance sections has to track the same layout
    // computation resized() already does rather than duplicating it.
    int dividerY = 0;

    void updateProcessorScale();
    void timerCallback() override;

    // Reflects the active engine's supportsRetuneControls() onto the two
    // sliders' enabled state — checked from the engine selector's onChange
    // (immediate) and every timerCallback tick (in case the active engine
    // changes for a reason other than the user picking one, though nothing
    // in this plugin currently does that — cheap enough to be safe here
    // rather than relying on that always being true).
    void updateRetuneControlsEnablement();

    /// Same shape as updateRetuneControlsEnablement() above, kept as its
    /// own function rather than folded together: the two check different
    /// capability flags (a PSOLA-only property vs. a phase-vocoder+PSOLA
    /// one), and keeping them separate means a future engine with its own
    /// unique creative controls follows the same one-flag/one-function
    /// pattern rather than growing one function's branching logic further.
    void updateGrainWidthControlEnablement();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchzazzAudioProcessorEditor)
};
