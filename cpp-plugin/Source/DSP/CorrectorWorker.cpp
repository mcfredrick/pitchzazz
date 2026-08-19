#include "CorrectorWorker.h"
#include <algorithm>
#include <cmath>

namespace pitchzazz
{

CorrectorWorker::CorrectorWorker (int blockSizeIn, double sampleRateIn, std::unique_ptr<PitchEngine> initialEngine,
                                   juce::AbstractFifo& inputFifoIn, std::vector<float>& inputBufferIn,
                                   juce::AbstractFifo& outputFifoIn, std::vector<float>& outputBufferIn)
    : juce::Thread ("Pitchzazz Corrector"),
      engine (std::move (initialEngine)),
      blockSize (blockSizeIn),
      sampleRate (sampleRateIn),
      inputFifo (inputFifoIn),
      inputBuffer (inputBufferIn),
      outputFifo (outputFifoIn),
      outputBuffer (outputBufferIn),
      analysisBuffer ((size_t) blockSizeIn, 0.0f),
      engineOutput ((size_t) blockSizeIn, 0.0f),
      crossfadeOutput ((size_t) blockSizeIn, 0.0f),
      blendedOutput ((size_t) blockSizeIn, 0.0f),
      pendingTonicPitchClass (0),
      pendingMode (static_cast<int> (ScaleMode::major))
{
    jassert (engine != nullptr);
    activeEngineName.store (engine->getName(), std::memory_order_relaxed);
    activeLatencySamples.store (engine->getLatencySamples(), std::memory_order_relaxed);
    activeSupportsRetuneControls.store (engine->supportsRetuneControls(), std::memory_order_relaxed);
    activeSupportsGrainWidthControl.store (engine->supportsGrainWidthControl(), std::memory_order_relaxed);
}

CorrectorWorker::~CorrectorWorker()
{
    // A swap requested but never picked up (e.g. right before shutdown)
    // would otherwise leak — this thread is guaranteed stopped by the
    // time a subclass-less juce::Thread destructor runs (callers must
    // have already called stopThread(), see PluginProcessor's
    // stopWorker()), so it's safe to just reclaim it here directly.
    delete pendingEngine.exchange (nullptr, std::memory_order_acquire);
}

void CorrectorWorker::setScale (Scale newScale) noexcept
{
    pendingTonicPitchClass.store (newScale.tonicPitchClass, std::memory_order_relaxed);
    pendingMode.store (static_cast<int> (newScale.mode), std::memory_order_relaxed);
}

void CorrectorWorker::setCorrectionAmount (float amount) noexcept
{
    pendingCorrectionAmount.store (amount, std::memory_order_relaxed);
}

void CorrectorWorker::setRetuneSpeedMs (float speedMs) noexcept
{
    pendingRetuneSpeedMs.store (speedMs, std::memory_order_relaxed);
}

bool CorrectorWorker::getActiveSupportsRetuneControls() const noexcept
{
    return activeSupportsRetuneControls.load (std::memory_order_relaxed);
}

void CorrectorWorker::setGrainWidthMultiplier (float multiplier) noexcept
{
    pendingGrainWidthMultiplier.store (multiplier, std::memory_order_relaxed);
}

bool CorrectorWorker::getActiveSupportsGrainWidthControl() const noexcept
{
    return activeSupportsGrainWidthControl.load (std::memory_order_relaxed);
}

void CorrectorWorker::requestEngineSwap (std::unique_ptr<PitchEngine> newEngine) noexcept
{
    // If a previous swap request is still sitting unpicked-up, replace it
    // rather than leak it — the worker only ever wants the latest request.
    auto* old = pendingEngine.exchange (newEngine.release(), std::memory_order_release);
    delete old;
}

const char* CorrectorWorker::getActiveEngineName() const noexcept
{
    return activeEngineName.load (std::memory_order_relaxed);
}

int CorrectorWorker::getActiveLatencySamples() const noexcept
{
    return activeLatencySamples.load (std::memory_order_relaxed);
}

double CorrectorWorker::getLastDetectUs() const noexcept
{
    return lastDetectUs.load (std::memory_order_relaxed);
}

double CorrectorWorker::getLastQuantizeUs() const noexcept
{
    return lastQuantizeUs.load (std::memory_order_relaxed);
}

double CorrectorWorker::getLastShiftUs() const noexcept
{
    return lastShiftUs.load (std::memory_order_relaxed);
}

float CorrectorWorker::getLastDetectedHz() const noexcept
{
    return lastDetectedHz.load (std::memory_order_relaxed);
}

float CorrectorWorker::getLastSemitoneShift() const noexcept
{
    return lastSemitoneShift.load (std::memory_order_relaxed);
}

namespace
{
    void pushToOutputFifo (const std::vector<float>& samples, juce::AbstractFifo& outputFifo, std::vector<float>& outputBuffer)
    {
        int outStart1, outSize1, outStart2, outSize2;
        outputFifo.prepareToWrite ((int) samples.size(), outStart1, outSize1, outStart2, outSize2);
        if (outSize1 > 0)
            std::copy (samples.begin(), samples.begin() + outSize1, outputBuffer.begin() + outStart1);
        if (outSize2 > 0)
            std::copy (samples.begin() + outSize1, samples.begin() + outSize1 + outSize2,
                       outputBuffer.begin() + outStart2);
        outputFifo.finishedWrite (outSize1 + outSize2);
    }
}

void CorrectorWorker::run()
{
    while (! threadShouldExit())
    {
        // Pick up a pending engine swap here, between blocks — never
        // mid-process() call. See the class doc for why this can't cause
        // an audible gap on its own. Only start a new crossfade once any
        // existing one has finished — a rapid second swap request just
        // waits in pendingEngine (requestEngineSwap() already drops any
        // earlier unconsumed request) rather than trying to overlap two
        // crossfades at once.
        if (crossfadeEngine == nullptr)
        {
            if (auto* newEngine = pendingEngine.exchange (nullptr, std::memory_order_acquire))
                crossfadeEngine.reset (newEngine);
        }

        int startIndex1, blockSize1, startIndex2, blockSize2;
        inputFifo.prepareToRead (blockSize - filled, startIndex1, blockSize1, startIndex2, blockSize2);

        if (blockSize1 > 0)
        {
            std::copy (inputBuffer.begin() + startIndex1, inputBuffer.begin() + startIndex1 + blockSize1,
                       analysisBuffer.begin() + filled);
            filled += blockSize1;
        }
        if (blockSize2 > 0)
        {
            std::copy (inputBuffer.begin() + startIndex2, inputBuffer.begin() + startIndex2 + blockSize2,
                       analysisBuffer.begin() + filled);
            filled += blockSize2;
        }
        inputFifo.finishedRead (blockSize1 + blockSize2);

        if (filled < blockSize)
        {
            // No deadline on this thread — a short poll is fine. Mirrors
            // pitch-cli's 1ms thread::sleep poll exactly; juce::Thread::wait()
            // is used instead of a plain sleep so stopThread() wakes this
            // loop immediately rather than waiting out the last poll.
            wait (1);
            continue;
        }

        const Scale scale { pendingTonicPitchClass.load (std::memory_order_relaxed),
                             static_cast<ScaleMode> (pendingMode.load (std::memory_order_relaxed)) };
        const float correctionAmount = pendingCorrectionAmount.load (std::memory_order_relaxed);
        const float retuneSpeedMs = pendingRetuneSpeedMs.load (std::memory_order_relaxed);
        const float grainWidthMultiplier = pendingGrainWidthMultiplier.load (std::memory_order_relaxed);
        engine->setScale (scale);
        engine->setCorrectionAmount (correctionAmount);
        engine->setRetuneSpeedMs (retuneSpeedMs);
        engine->setGrainWidthMultiplier (grainWidthMultiplier);

        if (crossfadeEngine != nullptr)
        {
            // Multi-block crossfade: both engines process the same input
            // every block for crossfadeBlocks blocks — the old one so its
            // output stays continuous right up to (and through) the
            // handoff, the new one so it's warming up its own internal
            // state rather than sitting idle until an abrupt cut. Blended
            // with a continuously-advancing ramp across the whole
            // crossfade period (not reset to 0..1 within each block) —
            // see the class doc for why a single block wasn't gentle
            // enough.
            crossfadeEngine->setScale (scale);
            crossfadeEngine->setCorrectionAmount (correctionAmount);
            crossfadeEngine->setRetuneSpeedMs (retuneSpeedMs);
            crossfadeEngine->setGrainWidthMultiplier (grainWidthMultiplier);
            const auto oldResult = engine->process (analysisBuffer, sampleRate, engineOutput);
            const auto newResult = crossfadeEngine->process (analysisBuffer, sampleRate, crossfadeOutput);

            const int totalCrossfadeSamples = blockSize * crossfadeBlocks;
            CorrectionResult blended;
            for (int i = 0; i < blockSize; ++i)
            {
                const int globalIndex = crossfadeBlockIndex * blockSize + i;
                const float t = totalCrossfadeSamples > 1 ? (float) globalIndex / (float) (totalCrossfadeSamples - 1) : 1.0f;
                // Equal-power (cos/sin) rather than linear (1-t)/t: linear
                // amplitude blending of two independent sources dips below
                // full power in the middle of the transition (gainOld^2 +
                // gainNew^2 < 1 except at the endpoints), audible as a
                // brief loudness dip — a real, separate contributor to the
                // quiet click a listening test caught even after the
                // 4-block crossfade already passed the automated
                // discontinuity test (docs/FINDINGS.md #14). Equal-power
                // keeps gainOld^2 + gainNew^2 == 1 throughout, the standard
                // fix for exactly this artifact.
                const float angle = t * juce::MathConstants<float>::halfPi;
                const float gainOld = std::cos (angle);
                const float gainNew = std::sin (angle);
                blendedOutput[(size_t) i] = engineOutput[(size_t) i] * gainOld + crossfadeOutput[(size_t) i] * gainNew;
            }
            // Detection/timing info reported for this block reflects the
            // incoming engine, since that's what's active once the
            // crossfade finishes.
            blended.detectedHz = newResult.detectedHz;
            blended.detectedClarity = newResult.detectedClarity;
            blended.semitoneShift = newResult.semitoneShift;
            blended.timings = newResult.timings;

            lastDetectUs.store (blended.timings.detectUs, std::memory_order_relaxed);
            lastQuantizeUs.store (blended.timings.quantizeUs, std::memory_order_relaxed);
            lastShiftUs.store (blended.timings.shiftUs, std::memory_order_relaxed);
            lastDetectedHz.store (blended.detectedHz, std::memory_order_relaxed);
            lastSemitoneShift.store (blended.semitoneShift, std::memory_order_relaxed);

            pushToOutputFifo (blendedOutput, outputFifo, outputBuffer);

            ++crossfadeBlockIndex;
            if (crossfadeBlockIndex >= crossfadeBlocks)
            {
                engine = std::move (crossfadeEngine);
                crossfadeBlockIndex = 0;
                activeEngineName.store (engine->getName(), std::memory_order_relaxed);
                activeLatencySamples.store (engine->getLatencySamples(), std::memory_order_relaxed);
                activeSupportsRetuneControls.store (engine->supportsRetuneControls(), std::memory_order_relaxed);
                activeSupportsGrainWidthControl.store (engine->supportsGrainWidthControl(), std::memory_order_relaxed);
            }
        }
        else
        {
            const auto result = engine->process (analysisBuffer, sampleRate, engineOutput);
            lastDetectUs.store (result.timings.detectUs, std::memory_order_relaxed);
            lastQuantizeUs.store (result.timings.quantizeUs, std::memory_order_relaxed);
            lastShiftUs.store (result.timings.shiftUs, std::memory_order_relaxed);
            lastDetectedHz.store (result.detectedHz, std::memory_order_relaxed);
            lastSemitoneShift.store (result.semitoneShift, std::memory_order_relaxed);
            pushToOutputFifo (engineOutput, outputFifo, outputBuffer);
        }

        filled = 0;
    }
}

} // namespace pitchzazz
