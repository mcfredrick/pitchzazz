#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <array>

//==============================================================================
PitchzazzAudioProcessor::PitchzazzAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      currentScale { 0, pitchzazz::ScaleMode::major }, // C major, matches pitch-cli's default
      currentEngineId (pitchzazz::availableEngines().front().id)
{
}

PitchzazzAudioProcessor::~PitchzazzAudioProcessor()
{
    stopWorker();
}

void PitchzazzAudioProcessor::stopWorker()
{
    if (worker != nullptr)
        worker->stopThread (1000);
    worker.reset();
}

const juce::String PitchzazzAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PitchzazzAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PitchzazzAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PitchzazzAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double PitchzazzAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PitchzazzAudioProcessor::getNumPrograms()
{
    return 1;
}

int PitchzazzAudioProcessor::getCurrentProgram()
{
    return 0;
}

void PitchzazzAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String PitchzazzAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void PitchzazzAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

void PitchzazzAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);

    // Stop any existing worker before resizing the FIFOs it holds
    // references into — prepareToPlay isn't real-time-constrained (unlike
    // processBlock), so blocking here to join the thread is fine.
    stopWorker();

    const int ringCapacity = juce::jmax (correctorBlockSize * 2,
                                          (int) (sampleRate * (double) ringBufferSeconds));
    inputRingBuffer.assign ((size_t) ringCapacity, 0.0f);
    outputRingBuffer.assign ((size_t) ringCapacity, 0.0f);
    inputFifo = std::make_unique<juce::AbstractFifo> (ringCapacity);
    outputFifo = std::make_unique<juce::AbstractFifo> (ringCapacity);

    // Pre-fill the output FIFO with silence so processBlock has something
    // to drain before the worker produces its first corrected block —
    // same startup-latency/robustness trade pitch-cli makes, see
    // docs/ARCHITECTURE.md's Latency section.
    const int startupSilence = juce::jmin (ringCapacity,
                                            (int) (sampleRate * (double) startupLatencyMs / 1000.0));
    int start1, size1, start2, size2;
    outputFifo->prepareToWrite (startupSilence, start1, size1, start2, size2);
    outputFifo->finishedWrite (size1 + size2);

    const pitchzazz::EngineConfig engineConfig { correctorBlockSize, sampleRate, windowSizeMs, currentScale };
    auto initialEngine = pitchzazz::createEngine (currentEngineId, engineConfig);
    jassert (initialEngine != nullptr); // currentEngineId always comes from availableEngines()

    // Read latency before moving initialEngine into the worker below —
    // std::move leaves it null. Now the actual measured algorithmic
    // latency (docs/PERFORMANCE_LOG.md's "Measured pipeline latency"
    // entry), not the block-accumulation-only lower bound this used to
    // report — that number understated the host-facing latency
    // compensation this plugin actually needs.
    setLatencySamples (initialEngine->getLatencySamples());

    worker = std::make_unique<pitchzazz::CorrectorWorker> (
        correctorBlockSize, sampleRate, std::move (initialEngine),
        *inputFifo, inputRingBuffer, *outputFifo, outputRingBuffer);
    worker->startThread();
}

void PitchzazzAudioProcessor::releaseResources()
{
    stopWorker();
    inputFifo.reset();
    outputFifo.reset();
    inputRingBuffer.clear();
    outputRingBuffer.clear();
}

bool PitchzazzAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}

void PitchzazzAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);

    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numInputChannels = getTotalNumInputChannels();
    const int numOutputChannels = getTotalNumOutputChannels();

    for (int ch = numInputChannels; ch < numOutputChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (bypassed.load (std::memory_order_relaxed) || inputFifo == nullptr || outputFifo == nullptr)
        return; // pass-through: leave the host's buffer untouched

    // Everything below is real-time-safe by construction: fixed-size
    // stack buffer (no heap allocation), juce::AbstractFifo (atomic
    // indices, no lock — audited in PluginProcessor.h's class doc), and
    // push/pop that copy fewer samples than requested rather than
    // blocking when the FIFO is full/empty. Direct port of pitch-cli's
    // input/output callbacks — see docs/ARCHITECTURE.md.
    std::array<float, 256> mono {};

    // Downmix to mono and push into the input FIFO.
    for (int offset = 0; offset < numSamples; offset += (int) mono.size())
    {
        const int chunk = juce::jmin ((int) mono.size(), numSamples - offset);
        for (int i = 0; i < chunk; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < numInputChannels; ++ch)
                sum += buffer.getReadPointer (ch)[offset + i];
            mono[(size_t) i] = numInputChannels > 0 ? sum / (float) numInputChannels : 0.0f;
        }

        int start1, size1, start2, size2;
        inputFifo->prepareToWrite (chunk, start1, size1, start2, size2);
        if (size1 > 0)
            std::copy (mono.begin(), mono.begin() + size1, inputRingBuffer.begin() + start1);
        if (size2 > 0)
            std::copy (mono.begin() + size1, mono.begin() + size1 + size2, inputRingBuffer.begin() + start2);
        inputFifo->finishedWrite (size1 + size2);
    }

    // Pop corrected mono samples and up-mix to the output channel count.
    // An underrun (not enough ready yet) fills with silence rather than
    // blocking to wait — the correct real-time-safe failure mode here,
    // not a stall (docs/ARCHITECTURE.md).
    for (int offset = 0; offset < numSamples; offset += (int) mono.size())
    {
        const int chunk = juce::jmin ((int) mono.size(), numSamples - offset);

        int start1, size1, start2, size2;
        outputFifo->prepareToRead (chunk, start1, size1, start2, size2);
        if (size1 > 0)
            std::copy (outputRingBuffer.begin() + start1, outputRingBuffer.begin() + start1 + size1, mono.begin());
        if (size2 > 0)
            std::copy (outputRingBuffer.begin() + start2, outputRingBuffer.begin() + start2 + size2, mono.begin() + size1);
        outputFifo->finishedRead (size1 + size2);
        const int read = size1 + size2;

        for (int i = 0; i < chunk; ++i)
        {
            const float sample = i < read ? mono[(size_t) i] : 0.0f;
            for (int ch = 0; ch < numOutputChannels; ++ch)
                buffer.getWritePointer (ch)[offset + i] = sample;
        }
    }
}

void PitchzazzAudioProcessor::setScale (pitchzazz::Scale newScale)
{
    currentScale = newScale;
    if (worker != nullptr)
        worker->setScale (newScale);
}

void PitchzazzAudioProcessor::setEngine (const std::string& engineId)
{
    currentEngineId = engineId;
    if (worker == nullptr)
        return; // prepareToPlay hasn't run yet — the new engine will be picked up when it does

    const pitchzazz::EngineConfig engineConfig { correctorBlockSize, getSampleRate(), windowSizeMs, currentScale };
    auto newEngine = pitchzazz::createEngine (engineId, engineConfig);
    if (newEngine != nullptr)
    {
        // Different engines can have different latency (they do, between
        // the two built in — docs/COMPARISON.md's FFT-size-constraints
        // section) — read before the move below, same reason as
        // prepareToPlay's read. Reported slightly ahead of when the
        // worker actually finishes the crossfade (docs/ROADMAP.md Phase
        // 3), not synchronized to it exactly — an acceptable eventual-
        // consistency gap given the crossfade window is short.
        setLatencySamples (newEngine->getLatencySamples());
        worker->requestEngineSwap (std::move (newEngine));
    }
}

juce::String PitchzazzAudioProcessor::getActiveEngineName() const noexcept
{
    if (worker != nullptr)
        return worker->getActiveEngineName();
    return {};
}

double PitchzazzAudioProcessor::getActiveLatencyMs() const noexcept
{
    if (worker == nullptr)
        return 0.0;
    const double sr = getSampleRate();
    if (sr <= 0.0)
        return 0.0;
    return ((double) worker->getActiveLatencySamples() / sr) * 1000.0;
}

double PitchzazzAudioProcessor::getLastDetectUs() const noexcept { return worker != nullptr ? worker->getLastDetectUs() : 0.0; }
double PitchzazzAudioProcessor::getLastQuantizeUs() const noexcept { return worker != nullptr ? worker->getLastQuantizeUs() : 0.0; }
double PitchzazzAudioProcessor::getLastShiftUs() const noexcept { return worker != nullptr ? worker->getLastShiftUs() : 0.0; }

float PitchzazzAudioProcessor::getLastDetectedHz() const noexcept { return worker != nullptr ? worker->getLastDetectedHz() : 0.0f; }
float PitchzazzAudioProcessor::getLastSemitoneShift() const noexcept { return worker != nullptr ? worker->getLastSemitoneShift() : 0.0f; }

double PitchzazzAudioProcessor::getBudgetUs() const noexcept
{
    const double sr = getSampleRate();
    if (sr <= 0.0)
        return 0.0;
    return ((double) correctorBlockSize / sr) * 1'000'000.0;
}

bool PitchzazzAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* PitchzazzAudioProcessor::createEditor()
{
    return new PitchzazzAudioProcessorEditor (*this);
}

void PitchzazzAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ignoreUnused (destData);
}

void PitchzazzAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ignoreUnused (data, sizeInBytes);
}

//==============================================================================
// Path Helper Functions - Use macOS Application Support (no permission prompts)
//
// These functions provide standard paths for plugin data following Apple's
// Human Interface Guidelines. Using Application Support prevents permission
// dialogs during installation.
//
// Example usage:
//   auto samplesDir = PitchzazzAudioProcessor::getSamplesPath();
//   if (!samplesDir.exists())
//       samplesDir.createDirectory();
//

juce::File PitchzazzAudioProcessor::getApplicationSupportPath()
{
    auto appSupport = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory
    );

    auto projectFolder = appSupport.getChildFile(JucePlugin_Name);

    // Create if doesn't exist
    if (!projectFolder.exists())
        projectFolder.createDirectory();

    return projectFolder;
}

juce::File PitchzazzAudioProcessor::getSamplesPath()
{
    auto samplesDir = getApplicationSupportPath().getChildFile("Samples");

    if (!samplesDir.exists())
        samplesDir.createDirectory();

    return samplesDir;
}

juce::File PitchzazzAudioProcessor::getPresetsPath()
{
    auto presetsDir = getApplicationSupportPath().getChildFile("Presets");

    if (!presetsDir.exists())
        presetsDir.createDirectory();

    return presetsDir;
}

juce::File PitchzazzAudioProcessor::getUserDataPath()
{
    auto userDataDir = getApplicationSupportPath().getChildFile("UserData");

    if (!userDataDir.exists())
        userDataDir.createDirectory();

    return userDataDir;
}

juce::File PitchzazzAudioProcessor::getLogsPath()
{
    // Logs go to ~/Library/Logs/PluginName (standard macOS location)
    auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    auto logsDir = home.getChildFile("Library").getChildFile("Logs").getChildFile(JucePlugin_Name);

    if (!logsDir.exists())
        logsDir.createDirectory();

    return logsDir;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchzazzAudioProcessor();
}
