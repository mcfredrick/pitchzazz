// Tests the leading untested hypothesis from this session's investigation:
// every prior render (PSOLACrackleProbe.cpp, PSOLAWaveformDump.cpp) fed a
// *fixed* detectedHz into PSOLAPitchShifter::shiftPitch() while only
// jittering the input waveform's actual content -- but PSOLACorrector::
// process() (the real production pipeline) calls detector.detect() fresh
// every block and feeds *that* value straight into shiftPitch(). periodSamples
// -- the single most load-bearing piece of state in placeGrainAt() -- has
// never once been allowed to actually vary block-to-block in this
// investigation, even in the "jittered" renders.
//
// This runs the real PSOLACorrector class directly (not a hand-rolled
// approximation of it) on the jittered voice signal, block by block,
// exactly as production does -- detect, quantize, shift, every block. Logs
// the actual detectedHz sequence (checks PitchDetector's own stability as
// a side effect) and runs the same jump-scan methodology already
// established (0.045 outlier threshold, well above a ~220Hz tone's
// natural max slope) against the real output.

#include <DSP/PSOLACorrector.h>
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace pitchzazz;

namespace
{
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 2048;
    constexpr float baseFreq = 220.0f;

    // Same generator as PSOLACrackleProbe.cpp/PSOLAWaveformDump.cpp.
    std::vector<float> jitteredVoice (int totalSamples, float jitterDepth)
    {
        constexpr float jitterRate1 = 11.3f;
        constexpr float jitterRate2 = 17.7f;

        std::vector<float> out ((size_t) totalSamples);
        double phase = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            const double wobble = std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate1 * t) * 0.6
                                 + std::sin (2.0 * juce::MathConstants<double>::pi * jitterRate2 * t) * 0.4;
            const double instFreq = (double) baseFreq * (1.0 + (double) jitterDepth * wobble);
            phase += 2.0 * juce::MathConstants<double>::pi * instFreq / sampleRate;
            out[(size_t) i] = (float) std::sin (phase);
        }
        return out;
    }
}

TEST_CASE ("PSOLA real pipeline: does per-block detectedHz variation cause artifacts?", "[psola][real-pipeline]")
{
    std::cout << std::fixed << std::setprecision (4);

    for (float jitterDepth : { 0.02f, 0.05f })
    {
        PSOLACorrector corrector (blockSize, sampleRate, Scale { 0, ScaleMode::major });
        const auto input = jitteredVoice (blockSize * 20, jitterDepth);
        std::vector<float> output (input.size(), 0.0f);

        std::vector<float> detectedHzLog, semitoneShiftLog;
        const int totalBlocks = (int) (input.size() / (size_t) blockSize);
        for (int block = 0; block < totalBlocks; ++block)
        {
            std::vector<float> in (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            std::vector<float> out (blockSize, 0.0f);
            const auto result = corrector.process (in, sampleRate, out);
            std::copy (out.begin(), out.end(), output.begin() + block * blockSize);
            detectedHzLog.push_back (result.detectedHz);
            semitoneShiftLog.push_back (result.semitoneShift);
        }

        std::cout << "\n=== jitterDepth=" << jitterDepth << " ===\n";
        std::cout << "detectedHz per block: ";
        for (float hz : detectedHzLog) std::cout << hz << " ";
        std::cout << "\n";
        float minHz = detectedHzLog[0], maxHz = detectedHzLog[0];
        for (float hz : detectedHzLog) { minHz = std::min (minHz, hz); maxHz = std::max (maxHz, hz); }
        std::cout << "detectedHz range: " << minHz << " to " << maxHz << " (spread " << (maxHz - minHz) << "Hz)\n";

        std::cout << "semitoneShift per block: ";
        for (float s : semitoneShiftLog) std::cout << s << " ";
        std::cout << "\n";

        // Same jump-scan as PSOLAWaveformDump.cpp's [waveform-dump] renders.
        constexpr float threshold = 0.045f;
        std::vector<std::pair<int, float>> jumps;
        for (size_t i = 1; i < output.size(); ++i)
        {
            const float d = std::abs (output[i] - output[(size_t) (i - 1)]);
            if (d > threshold)
                jumps.push_back ({ (int) i, d });
        }
        std::cout << "jumps >" << threshold << ": " << jumps.size() << "\n";
        for (const auto& j : jumps)
            std::cout << "   sample " << j.first << ": " << j.second << "\n";

        // Zero-run scan, same as PSOLAWaveformDump.cpp's [dropout] test.
        int longestZeroRun = 0, currentRun = 0;
        for (size_t i = (size_t) blockSize; i < output.size(); ++i)
        {
            if (output[i] == 0.0f) { ++currentRun; longestZeroRun = std::max (longestZeroRun, currentRun); }
            else currentRun = 0;
        }
        std::cout << "longest zero-run: " << longestZeroRun << " samples\n";

        const std::string filename = "/tmp/psola_real_pipeline_jitter" + std::to_string ((int) (jitterDepth * 100)) + "pct.csv";
        std::ofstream f (filename);
        f << "sample,input,output\n";
        for (size_t i = 0; i < input.size(); ++i)
            f << i << "," << input[i] << "," << output[i] << "\n";
        std::cout << "Wrote " << filename << "\n";
    }
    std::cout << std::endl;
}
