#include <DSP/PitchDetector.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace pitchzazz;

namespace
{
    std::vector<float> sineWave (float freqHz, double sampleRate, int numSamples)
    {
        std::vector<float> samples ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            samples[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * freqHz * (float) i / (float) sampleRate);
        return samples;
    }
}

// These have no Rust-side equivalent (the Rust engine relies on the
// pitch-detection crate's own upstream test suite) but are essential here:
// unlike the scale/corrector tests, nothing else exercises the FFT/NSDF
// math this port had to get numerically right (see PitchDetector.cpp's
// normalizedSquareDifference comment on the JUCE-vs-rustfft FFT
// normalization mismatch this depends on).

TEST_CASE ("detects a clean sine wave's fundamental", "[pitch-detector]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float trueFreq = 220.0f; // A3

    PitchDetector detector (blockSize);
    const auto signal = sineWave (trueFreq, sampleRate, blockSize);

    const auto pitch = detector.detect (signal, sampleRate);

    CHECK (pitch.frequencyHz > 0.0f);
    CHECK (std::abs (pitch.frequencyHz - trueFreq) < 2.0f); // within 2Hz
    CHECK (pitch.clarity > 0.9f);                            // a clean sine should be very clear
}

TEST_CASE ("silence produces no detected pitch", "[pitch-detector]")
{
    constexpr int blockSize = 2048;
    PitchDetector detector (blockSize);
    const std::vector<float> silence (blockSize, 0.0f);

    const auto pitch = detector.detect (silence, 44100.0);

    CHECK (pitch.frequencyHz == 0.0f);
    CHECK (pitch.clarity == 0.0f);
}
