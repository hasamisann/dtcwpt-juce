#include "GateAudioProcessor.h"
#include <juce_core/juce_core.h>

//==============================================================================
// Default topology (6-level DWT)
//==============================================================================

namespace
{
    /** Default topology: 6-level DWT destination strings. */
    const std::vector<std::string> kDefaultTopology =
        { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };

    /** Topology property key on apvts_.state. */
    constexpr const char* kTopologyPropertyKey = "topology";

    /** Topology separator used when serialising to a comma-separated string. */
    constexpr const char* kTopologySeparator = ",";

} // namespace

//==============================================================================
// Constructor
//==============================================================================

GateAudioProcessor::GateAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)),
      apvts_ (*this, nullptr, "GateParams", createParameterLayout()),
      currentDestinations_ (kDefaultTopology)
{
    // Store the default topology as a ValueTree property
    juce::StringArray destArray;
    for (const auto& d : currentDestinations_)
        destArray.add (juce::String (d));

    apvts_.state.setProperty (kTopologyPropertyKey,
                               destArray.joinIntoString (kTopologySeparator),
                               nullptr);

    // Cache the 64 parameter pointers
    for (int i = 0; i < 64; ++i)
    {
        juce::String paramID = juce::String::formatted("threshold_%02d", i);
        thresholdRaw_[i] = apvts_.getRawParameterValue(paramID);
    }
}

//==============================================================================
// Lifecycle
//==============================================================================

void GateAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_  = samplesPerBlock;

    mainDoubleBuffer_.setSize (2, samplesPerBlock);

    inputMonoScratch_.setSize  (1, samplesPerBlock);
    outputMonoScratch_.setSize (1, samplesPerBlock);

    rebuildDTCWPT();
}

void GateAudioProcessor::releaseResources()
{
    dtcwptMain_.reset();
    gateProcessor_ = nullptr;
    mainDoubleBuffer_.setSize (0, 0);
    inputMonoScratch_.setSize  (0, 0);
    outputMonoScratch_.setSize (0, 0);
}

//==============================================================================
// Private helper: rebuild DTCWPTProcessor
//==============================================================================

void GateAudioProcessor::rebuildDTCWPT()
{
    dtcwpt::TopologyConfig config;
    config.destinations = currentDestinations_;
    config.maxDepth     = 8;

    auto newEngine = std::make_unique<dtcwpt::DTCWPTProcessor>();

    auto proc      = std::make_unique<GateBandProcessor>();
    gateProcessor_ = proc.get();
    newEngine->setBandProcessor (std::move (proc));

    newEngine->prepareToPlay (currentSampleRate_,
                               currentBlockSize_,
                               config,
                               2 /* stereo */);

    dtcwptMain_ = std::move (newEngine);
    setLatencySamples (dtcwptMain_->getLatency());
}

//==============================================================================
// Processing
//==============================================================================

void GateAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    if (dtcwptMain_ == nullptr || gateProcessor_ == nullptr)
        return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // 1. Topology dirty check
    {
        std::vector<std::string> newDestinations;
        if (topoState_.tryConsume (newDestinations))
        {
            currentDestinations_ = newDestinations;
            rebuildDTCWPT();
        }
    }

    // 2. Read 64 threshold params → linear → set thresholds
    std::vector<double> thresholds(64, 0.0);
    for (int i = 0; i < 64; ++i)
    {
        if (thresholdRaw_[i] != nullptr)
        {
            float db = thresholdRaw_[i]->load();
            thresholds[i] = std::pow(10.0, db / 20.0);
        }
    }
    gateProcessor_->setThresholds(thresholds.data(), static_cast<int>(currentDestinations_.size()));

    // 3. Snapshot input (mono-averaged) for spectrum
    inputMonoScratch_.clear();
    for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
    {
        inputMonoScratch_.addFrom (0, 0,
                                    buffer,
                                    ch, 0,
                                    numSamples,
                                    1.0f / static_cast<float> (std::min (numChannels, 2)));
    }

    // 4. Float → Double conversion for main input
    mainDoubleBuffer_.setSize (2, numSamples, false, false, true);
    for (int ch = 0; ch < 2; ++ch)
    {
        const int srcCh = (ch < numChannels) ? ch : 0;
        const float* src = buffer.getReadPointer (srcCh);
        double*       dst = mainDoubleBuffer_.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<double> (src[i]);
    }

    // 5. Process audio
    dtcwptMain_->processBlock (mainDoubleBuffer_);

    // 6. Push levels to bandLevelBridge_
    float levels[64];
    int nBands = gateProcessor_->getBandLevelsDb(levels);
    bandLevelBridge_.update(levels, nBands);

    // 7. Double → Float conversion
    for (int ch = 0; ch < 2; ++ch)
    {
        const double* src = mainDoubleBuffer_.getReadPointer (ch);
        float*         dst = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<float> (src[i]);
    }

    // 8. Snapshot output and push spectrum to bridge
    outputMonoScratch_.clear();
    for (int ch = 0; ch < 2; ++ch)
    {
        outputMonoScratch_.addFrom (0, 0,
                                     buffer,
                                     ch, 0,
                                     numSamples,
                                     0.5f);
    }

    spectrumBridge_.push (inputMonoScratch_.getReadPointer (0),
                           outputMonoScratch_.getReadPointer (0),
                           numSamples);
}

//==============================================================================
// Bus layout
//==============================================================================

bool GateAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

//==============================================================================
// Plugin identity
//==============================================================================

const juce::String GateAudioProcessor::getName() const
{
    return "DT-CWPT Gate";
}

bool GateAudioProcessor::acceptsMidi()  const { return false; }
bool GateAudioProcessor::producesMidi() const { return false; }
bool GateAudioProcessor::isMidiEffect() const { return false; }

double GateAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==============================================================================
// Programs
//==============================================================================

int GateAudioProcessor::getNumPrograms()    { return 1; }
int GateAudioProcessor::getCurrentProgram() { return 0; }
void GateAudioProcessor::setCurrentProgram (int /*index*/) {}
const juce::String GateAudioProcessor::getProgramName (int /*index*/) { return {}; }
void GateAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/) {}

//==============================================================================
// State serialisation
//==============================================================================

void GateAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream (destData, true);
    apvts_.state.writeToStream (stream);
}

void GateAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree state = juce::ValueTree::readFromData (data,
                                                            static_cast<std::size_t> (sizeInBytes));
    if (state.isValid())
    {
        apvts_.replaceState (state);

        const juce::var prop = apvts_.state.getProperty (kTopologyPropertyKey);
        if (prop.isString() && prop.toString().isNotEmpty())
        {
            const juce::StringArray tokens =
                juce::StringArray::fromTokens (prop.toString(), kTopologySeparator, "");

            std::vector<std::string> newDests;
            newDests.reserve (static_cast<std::size_t> (tokens.size()));
            for (const auto& t : tokens)
                newDests.push_back (t.toStdString());

            if (!newDests.empty())
            {
                topoState_.requestChange (newDests);
            }
        }
    }
}

//==============================================================================
// Parameter layout factory
//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout GateAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < 64; ++i)
    {
        juce::String paramID = juce::String::formatted("threshold_%02d", i);
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramID, 1},
            "Threshold " + juce::String(i),
            juce::NormalisableRange<float>(-120.0f, 0.0f, 0.1f),
            -60.0f,
            juce::String(),
            juce::AudioProcessorParameter::genericParameter,
            [](float value, int) { return juce::String(value, 1) + " dB"; },
            [](const juce::String& text) { return text.dropLastCharacters(3).getFloatValue(); }
        ));
    }

    return layout;
}
