#include "GateAudioProcessor.h"
#include "../common/TopologyPersistence.h"
#include <juce_core/juce_core.h>

//==============================================================================
// Default topology (6-level DWT)
//==============================================================================

namespace
{
    /** Default topology: 6-level DWT destination strings. */
    const std::vector<std::string> kDefaultTopology =
        { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };

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
    apvts_.state.setProperty (topology::kTopologyPropertyKey,
                               topology::serializeTopologyDestinations (currentDestinations_),
                               nullptr);

    // Cache the 256 parameter pointers
    for (int i = 0; i < GateBandProcessor::kMaxBands; ++i)
    {
        juce::String paramID = juce::String::formatted("threshold_%02d", i);
        thresholdRaw_[i] = apvts_.getRawParameterValue(paramID);
    }
    
    // Cache the bypass parameter
    bypassLowestRaw_ = apvts_.getRawParameterValue("bypass_lowest_band");
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

    if (numSamples == 0 || numChannels == 0)
        return;

    // 1. Topology dirty check
    {
        std::vector<std::string> newDestinations;
        if (topoState_.tryConsume (newDestinations))
        {
            currentDestinations_ = newDestinations;
            rebuildDTCWPT();
        }
    }

    // 2. Read 256 threshold params → linear → set thresholds
    size_t numBands = std::min(currentDestinations_.size(), static_cast<size_t>(GateBandProcessor::kMaxBands));
    std::array<double, GateBandProcessor::kMaxBands> thresholds {};
    for (size_t i = 0; i < numBands; ++i)
    {
        if (thresholdRaw_[i] != nullptr)
        {
            float db = thresholdRaw_[i]->load();
            thresholds[i] = std::pow(10.0, db / 20.0);
        }
    }
    gateProcessor_->setThresholds(thresholds.data(), static_cast<int>(numBands));

    if (bypassLowestRaw_ != nullptr)
        gateProcessor_->setBypassLowest(bypassLowestRaw_->load() >= 0.5f);

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
    mainDoubleBuffer_.clear();
    for (int ch = 0; ch < std::min(numChannels, 2); ++ch)
    {
        const float* src = buffer.getReadPointer (ch);
        double*       dst = mainDoubleBuffer_.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<double> (src[i]);
    }

    // 5. Process audio
    dtcwptMain_->processBlock (mainDoubleBuffer_);

    // 6. Push levels to bandLevelBridge_
    float levels[GateBandProcessor::kMaxBands];
    int nBands = gateProcessor_->getBandLevelsDb(levels);
    bandLevelBridge_.update(levels, std::min(nBands, GateBandProcessor::kMaxBands));

    // 7. Double → Float conversion
    for (int ch = 0; ch < std::min(numChannels, 2); ++ch)
    {
        const double* src = mainDoubleBuffer_.getReadPointer (ch);
        float*         dst = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<float> (src[i]);
    }

    // 8. Snapshot output and push spectrum to bridge
    outputMonoScratch_.clear();
    for (int ch = 0; ch < std::min(numChannels, 2); ++ch)
    {
        outputMonoScratch_.addFrom (0, 0,
                                     buffer,
                                     ch, 0,
                                     numSamples,
                                     1.0f / static_cast<float>(std::min(numChannels, 2)));
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

        const auto resolved = getStoredTopologyDestinations();
        topoState_.requestChange (resolved);
    }
}

std::vector<std::string> GateAudioProcessor::getStoredTopologyDestinations() const
{
    return topology::resolvePersistedTopologyDestinations (apvts_.state, currentDestinations_);
}

//==============================================================================
// Parameter layout factory
//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout GateAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < GateBandProcessor::kMaxBands; ++i)
    {
        juce::String paramID = juce::String::formatted("threshold_%02d", i);

        auto attributes = juce::AudioParameterFloatAttributes()
                              .withStringFromValueFunction ([](float value, int)
                              {
                                  return juce::String (value, 1) + " dB";
                              })
                              .withValueFromStringFunction ([](const juce::String& text)
                              {
                                  return text.dropLastCharacters (3).getFloatValue();
                              });

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{paramID, 1},
            "Threshold " + juce::String(i),
            juce::NormalisableRange<float>(-120.0f, 10.0f, 0.1f),
            -60.0f,
            attributes
        ));
    }

    // Bypass lowest frequency band toggle
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"bypass_lowest_band", 1},
        "Bypass Lowest",
        false
    ));

    return layout;
}
