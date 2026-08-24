// Instrumented trace of the shared McLeod PitchDetector's raw output on
// real vocal content -- built to test a specific hypothesis raised while
// listening to the G-major/420ms-smoothing demo renders: a roughly-
// octave-below artifact, tracking the singer's pitch, with the *same*
// character on both TD-PSOLA and Varispeed+WSOLA despite those being
// structurally unrelated shift algorithms (docs/FINDINGS.md #19/#20 for
// PSOLA's crackle/beat, #23/#24 for Varispeed's zero-lag-lock buzz).
//
// The unifying mechanism this probe checks for: Corrector, PSOLACorrector,
// and VarispeedCorrector each own an *independent* `PitchDetector detector`
// instance (see each class's header) -- not a literal shared object -- but
// RenderDemoAudio.cpp feeds all three the identical input audio at the
// identical blockSize=2048 block boundaries, and PitchDetector::detect()
// is a deterministic function of its input block. So if McLeod's NSDF
// peak-picking occasionally locks onto a subharmonic (a well-known
// autocorrelation-family failure mode -- a periodic signal's NSDF has a
// real secondary peak at 2x the true period whenever even harmonics are
// weak relative to odd ones, which normalphonation regularly produces),
// all three correctors compute that *same* wrong detectedHz for that
// block, and therefore the same wrong semitoneShift (Corrector.cpp:
// `semitoneShift = targetNote - hzToMidi(pitch.frequencyHz)`) feeds
// whichever shifter is active. That would explain the same artifact
// character showing up on two unrelated engines: one shared upstream
// cause via deterministic replay, not two independent internal failures.
//
// This checks that hypothesis directly against detectedHz's own
// trajectory -- no need to instrument either shifter's internals first
// (PSOLA's placeGrainAt has no search/ambiguity of its own to check: it
// deterministically floor-divides by whatever periodSamples
// updatePeriodEstimate() set from detectedHz, so an octave artifact
// there is necessarily downstream of detectedHz being wrong, not a
// separate bucket-selection ambiguity internal to PSOLA itself).
//
// Run with: PITCHZAZZ_DEMO_INPUT=/path/to/demo-dry.mp3
//           ./build/Release/Benchmarks "[octave]"

#include <DSP/PitchDetector.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr int blockSize = 2048; // matches Corrector/PSOLACorrector/VarispeedCorrector's shared blockSize in RenderDemoAudio.cpp

    std::vector<float> readMonoAudio (const juce::File& file, double& sampleRateOut)
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
            return {};

        sampleRateOut = reader->sampleRate;
        const int numChannels = (int) reader->numChannels;
        const int numSamples = (int) reader->lengthInSamples;

        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        reader->read (&buffer, 0, numSamples, 0, true, true);

        std::vector<float> mono ((size_t) numSamples, 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = buffer.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
                mono[(size_t) i] += src[i] / (float) numChannels;
        }
        return mono;
    }
}

TEST_CASE ("Trace raw detectedHz per block on real vocal content, looking for octave-relation instability", "[.][octave]")
{
    const char* inputPathEnv = std::getenv ("PITCHZAZZ_DEMO_INPUT");
    if (inputPathEnv == nullptr)
    {
        std::cout << "Set PITCHZAZZ_DEMO_INPUT to demo-dry.mp3's path to run this probe.\n";
        return;
    }

    double sampleRate = 0.0;
    const auto mono = readMonoAudio (juce::File (inputPathEnv), sampleRate);
    REQUIRE (! mono.empty());
    std::cerr << "Loaded " << mono.size() << " samples @ " << sampleRate << "Hz\n";

    PitchDetector detector (blockSize);
    const int totalBlocks = (int) (mono.size() / (size_t) blockSize);
    std::vector<float> block ((size_t) blockSize);

    std::cout << "block,timeSec,hz,clarity\n";
    for (int b = 0; b < totalBlocks; ++b)
    {
        const size_t offset = (size_t) b * (size_t) blockSize;
        std::copy (mono.begin() + (long) offset, mono.begin() + (long) (offset + (size_t) blockSize), block.begin());
        const DetectedPitch pitch = detector.detect (block, sampleRate);
        const float timeSec = (float) offset / (float) sampleRate;
        std::cout << b << "," << timeSec << "," << pitch.frequencyHz << "," << pitch.clarity << "\n";
    }
}
