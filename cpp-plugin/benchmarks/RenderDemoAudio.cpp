// One-off offline render tool: feeds a WAV file through each of the three
// C++ correctors (phase vocoder, TD-PSOLA, Varispeed+WSOLA) exactly the
// way CorrectorWorker's background thread would -- same Corrector/
// PSOLACorrector/VarispeedCorrector classes, same block size, same
// detect -> quantize -> shift orchestration -- so the output is genuine
// engine output, not a simulation of it. Built for generating the GH
// Pages audio-demo comparison (docs/ROADMAP.md); not part of the regular
// benchmark suite, so it's tagged [!hide] to stay out of default runs.
//
// Rust is deliberately not included here: pitch-core has no retune-speed
// smoothing control at all (see RetuneSmoothing.h), so a "420ms smoothing"
// render doesn't apply to it -- see docs/ROADMAP.md for that scoping call.
//
// Input path comes from the PITCHZAZZ_DEMO_INPUT env var rather than a
// hardcoded path, since the source file lives outside this repo (a
// personal Ableton project export), not something to commit or bake into
// source. Output WAVs land in PITCHZAZZ_DEMO_OUTDIR (default /tmp).
//
// Run with: PITCHZAZZ_DEMO_INPUT=/path/to/input.wav
//           ./build/Release/Benchmarks "[render]"
//
// Tagged [.] (Catch2's hidden-test marker, not "!hide" -- that isn't a
// real Catch2 tag) so it's excluded from default Benchmarks/ctest runs.

#include <DSP/Corrector.h>
#include <DSP/PSOLACorrector.h>
#include <DSP/Scale.h>
#include <DSP/VarispeedCorrector.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace pitchzazz;

namespace
{
    // TEMPORARY for the finding-#31-followup block-rate-artifact causal test:
    // overridable via PITCHZAZZ_DEMO_BLOCKSIZE so the same render can be
    // re-run at a different block size without a rebuild-per-value edit
    // cycle -- to be reverted back to a plain constexpr once that test is
    // done, not left as permanent surface area.
    const int blockSize = std::getenv ("PITCHZAZZ_DEMO_BLOCKSIZE") != nullptr
                               ? std::atoi (std::getenv ("PITCHZAZZ_DEMO_BLOCKSIZE"))
                               : 2048;
    constexpr int windowSizeMs = 30; // matches PluginProcessor.h's real default
    constexpr float retuneSpeedMs = 420.0f;

    // G major: tonic pitch class 7 (0 = C), Ionian mode.
    const Scale gMajor { 7, ScaleMode::major };

    // Downmixes to mono by averaging channels -- matches the plugin's own
    // internal representation (see AudioSessionRecorder.h), so the
    // correctors see the same signal shape they'd see live.
    std::vector<float> readMonoWav (const juce::File& file, double& sampleRateOut)
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

    void writeMonoWav (const juce::File& file, const std::vector<float>& samples, double sampleRate)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (new juce::FileOutputStream (file));
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wavFormat.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                    .withSampleRate (sampleRate)
                                                    .withNumChannels (1)
                                                    .withBitsPerSample (32)));
        if (writer == nullptr)
        {
            std::cerr << "Failed to open writer for " << file.getFullPathName() << "\n";
            return;
        }
        const float* channels[] { samples.data() };
        writer->writeFromFloatArrays (channels, 1, (int) samples.size());
    }

    // Pads to a whole number of blocks with silence so every engine sees
    // full-sized blocks throughout, same as the real plugin (its ring
    // buffer never hands the worker a partial block either).
    std::vector<float> padToBlocks (const std::vector<float>& input)
    {
        std::vector<float> padded = input;
        const size_t remainder = padded.size() % (size_t) blockSize;
        if (remainder != 0)
            padded.resize (padded.size() + ((size_t) blockSize - remainder), 0.0f);
        return padded;
    }

    template <typename CorrectorT>
    std::vector<float> render (CorrectorT& corrector, const std::vector<float>& input, double sampleRate)
    {
        // Optional per-block CorrectionResult trace, gated on PITCHZAZZ_DEMO_TRACE
        // -- prints the actual detectedHz/detectedClarity/semitoneShift the
        // correction pipeline used for each block, straight from the real
        // process() call, rather than re-detecting pitch on the already-
        // shifted output (which would re-run the same fallible detector on a
        // different signal and confound "did the fix work" with "does
        // re-analysis independently trip the same failure mode" --
        // docs/FINDINGS.md #31's clarity-gate verification).
        const bool trace = std::getenv ("PITCHZAZZ_DEMO_TRACE") != nullptr;

        std::vector<float> output (input.size(), 0.0f);
        const int totalBlocks = (int) (input.size() / (size_t) blockSize);
        std::vector<float> inBlock ((size_t) blockSize), outBlock ((size_t) blockSize);
        if (trace)
            std::cout << "block,timeSec,detectedHz,detectedClarity,semitoneShift\n";
        for (int block = 0; block < totalBlocks; ++block)
        {
            const size_t offset = (size_t) block * (size_t) blockSize;
            std::copy (input.begin() + (long) offset, input.begin() + (long) (offset + (size_t) blockSize), inBlock.begin());
            const auto result = corrector.process (inBlock, sampleRate, outBlock);
            if (trace)
                std::cout << block << "," << (float) offset / (float) sampleRate << ","
                           << result.detectedHz << "," << result.detectedClarity << "," << result.semitoneShift << "\n";
            std::copy (outBlock.begin(), outBlock.end(), output.begin() + (long) offset);
        }
        return output;
    }
}

TEST_CASE ("Render G-major/420ms-smoothing demo audio for the GH Pages site", "[.][render]")
{
    const char* inputPathEnv = std::getenv ("PITCHZAZZ_DEMO_INPUT");
    if (inputPathEnv == nullptr)
    {
        std::cout << "Set PITCHZAZZ_DEMO_INPUT to a WAV path to run this render.\n";
        return;
    }
    const juce::File inputFile (inputPathEnv);
    const char* outDirEnv = std::getenv ("PITCHZAZZ_DEMO_OUTDIR");
    const juce::File outDir (outDirEnv != nullptr ? juce::File (outDirEnv) : juce::File ("/tmp"));

    double sampleRate = 0.0;
    const auto mono = readMonoWav (inputFile, sampleRate);
    REQUIRE (! mono.empty());
    std::cout << "Loaded " << inputFile.getFullPathName() << ": " << mono.size() << " samples @ " << sampleRate << "Hz\n";

    const auto padded = padToBlocks (mono);

    {
        Corrector corrector (blockSize, sampleRate, windowSizeMs, gMajor);
        corrector.setRetuneSpeedMs (retuneSpeedMs);
        const auto output = render (corrector, padded, sampleRate);
        const auto outFile = outDir.getChildFile ("pitchzazz_demo_phase_vocoder.wav");
        writeMonoWav (outFile, output, sampleRate);
        std::cout << "Wrote " << outFile.getFullPathName() << "\n";
    }
    {
        PSOLACorrector corrector (blockSize, sampleRate, gMajor);
        corrector.setRetuneSpeedMs (retuneSpeedMs);
        const auto output = render (corrector, padded, sampleRate);
        const auto outFile = outDir.getChildFile ("pitchzazz_demo_psola.wav");
        writeMonoWav (outFile, output, sampleRate);
        std::cout << "Wrote " << outFile.getFullPathName() << "\n";
    }
    {
        VarispeedCorrector corrector (blockSize, sampleRate, gMajor);
        corrector.setRetuneSpeedMs (retuneSpeedMs);
        const auto output = render (corrector, padded, sampleRate);
        const auto outFile = outDir.getChildFile ("pitchzazz_demo_varispeed.wav");
        writeMonoWav (outFile, output, sampleRate);
        std::cout << "Wrote " << outFile.getFullPathName() << "\n";
    }
}
