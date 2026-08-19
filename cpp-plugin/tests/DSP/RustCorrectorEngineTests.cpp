#include <DSP/PitchEngineRegistry.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace pitchzazz;

// Proves the cxx FFI bridge produces *correct* DSP output end to end, not
// just that it links — a real synthetic tone through the actual Rust
// pitch-core crate, via the same createEngine() path the plugin uses.

TEST_CASE ("Rust engine detects a clean sine wave through the FFI bridge", "[rust-ffi]")
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float trueFreq = 220.0f; // A3, same as PitchDetectorTests.cpp

    const EngineConfig config { blockSize, sampleRate, 50, Scale { 0, ScaleMode::major } };
    auto engine = createEngine ("rust-ffi", config);
    REQUIRE (engine != nullptr);
    CHECK (std::string (engine->getName()) == "Rust (FFI)");

    std::vector<float> signal ((size_t) blockSize);
    for (int i = 0; i < blockSize; ++i)
        signal[(size_t) i] = std::sin (2.0f * juce::MathConstants<float>::pi * trueFreq * (float) i / (float) sampleRate);

    std::vector<float> output ((size_t) blockSize);
    const auto result = engine->process (signal, sampleRate, output);

    CHECK (result.detectedHz > 0.0f);
    CHECK (std::abs (result.detectedHz - trueFreq) < 2.0f); // same tolerance as the C++ engine's own test
    CHECK (result.detectedClarity > 0.9f);
}

TEST_CASE ("Rust engine silence produces no detected pitch through the FFI bridge", "[rust-ffi]")
{
    constexpr int blockSize = 2048;
    const EngineConfig config { blockSize, 44100.0, 50, Scale { 0, ScaleMode::major } };
    auto engine = createEngine ("rust-ffi", config);
    REQUIRE (engine != nullptr);

    const std::vector<float> silence ((size_t) blockSize, 0.0f);
    std::vector<float> output ((size_t) blockSize);
    const auto result = engine->process (silence, 44100.0, output);

    CHECK (result.detectedHz == 0.0f);
    CHECK (result.semitoneShift == 0.0f);
}
