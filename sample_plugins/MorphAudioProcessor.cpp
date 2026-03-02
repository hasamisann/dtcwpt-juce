/**
 * @file MorphAudioProcessor.cpp
 * @brief Full implementation of the DT-CWPT Morph Plugin AudioProcessor.
 *
 * Implements parameter management, topology rebuild, float↔double conversion,
 * sidechain handling, spectrum bridge writes, and state persistence.
 */

#include "MorphAudioProcessor.h"
#include "gui/MorphEditor.h"

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

MorphAudioProcessor::MorphAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts_ (*this, nullptr, "MorphParams", createParameterLayout()),
      currentDestinations_ (kDefaultTopology)
{
    // Store the default topology as a ValueTree property so it is
    // included in getStateInformation() right from the start.
    juce::StringArray destArray;
    for (const auto& d : currentDestinations_)
        destArray.add (juce::String (d));

    apvts_.state.setProperty (kTopologyPropertyKey,
                               destArray.joinIntoString (kTopologySeparator),
                               nullptr);
}

//==============================================================================
// Lifecycle
//==============================================================================

void MorphAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_  = samplesPerBlock;

    // Allocate double-precision conversion buffers
    mainDoubleBuffer_.setSize (2, samplesPerBlock);
    scDoubleBuffer_.setSize   (2, samplesPerBlock);

    // Build the DTCWPTProcessor
    rebuildDTCWPT();
}

void MorphAudioProcessor::releaseResources()
{
    dtcwptMain_.reset();
    morphProcessor_ = nullptr;
    mainDoubleBuffer_.setSize (0, 0);
    scDoubleBuffer_.setSize   (0, 0);
}

//==============================================================================
// Private helper: rebuild DTCWPTProcessor
//==============================================================================

void MorphAudioProcessor::rebuildDTCWPT()
{
    // Build topology config from current destinations
    dtcwpt::TopologyConfig config;
    config.destinations = currentDestinations_;
    config.maxDepth     = 8;

    // Create a fresh DTCWPTProcessor
    auto newEngine = std::make_unique<dtcwpt::DTCWPTProcessor>();

    // Create MorphBandProcessor and transfer ownership; keep raw observer pointer
    auto mbp     = std::make_unique<MorphBandProcessor>();
    morphProcessor_ = mbp.get();
    newEngine->setBandProcessor (std::move (mbp));

    // Prepare the engine
    newEngine->prepareToPlay (currentSampleRate_,
                               currentBlockSize_,
                               config,
                               2 /* stereo */);

    // Swap in the new engine
    dtcwptMain_ = std::move (newEngine);

    // Report latency to the host
    setLatencySamples (dtcwptMain_->getLatency());
}

//==============================================================================
// Processing
//==============================================================================

void MorphAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // Safety check: processor must be prepared
    if (dtcwptMain_ == nullptr || morphProcessor_ == nullptr)
        return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // -------------------------------------------------------------------------
    // 1. Topology dirty check — rebuild if the GUI has requested a change
    // -------------------------------------------------------------------------
    {
        std::vector<std::string> newDestinations;
        if (topoState_.tryConsume (newDestinations))
        {
            currentDestinations_ = newDestinations;

            // Mirror to APVTS state property (for serialisation)
            juce::StringArray destArray;
            for (const auto& d : currentDestinations_)
                destArray.add (juce::String (d));
            apvts_.state.setProperty (kTopologyPropertyKey,
                                       destArray.joinIntoString (kTopologySeparator),
                                       nullptr);

            // Rebuild the engine (brief allocation on audio thread — see Decision 2)
            rebuildDTCWPT();
            updateHostDisplay (juce::AudioProcessorListener::ChangeDetails()
                                   .withLatencyChanged (true));
        }
    }

    // -------------------------------------------------------------------------
    // 2. Read APVTS atomic float parameters → set targets on MorphBandProcessor
    // -------------------------------------------------------------------------
    const float magnitudeF  = apvts_.getRawParameterValue (ParamID::Magnitude)->load();
    const float phaseF      = apvts_.getRawParameterValue (ParamID::Phase)->load();
    const float thresholdDB = apvts_.getRawParameterValue (ParamID::Threshold)->load();
    const bool  bypassLow   = apvts_.getRawParameterValue (ParamID::BypassLow)->load() >= 0.5f;
    const bool  bypassHigh  = apvts_.getRawParameterValue (ParamID::BypassHigh)->load() >= 0.5f;

    const double threshLinear = juce::Decibels::decibelsToGain (thresholdDB);

    morphProcessor_->setTargets (static_cast<double> (magnitudeF),
                                  static_cast<double> (phaseF),
                                  threshLinear,
                                  bypassLow,
                                  bypassHigh);

    // -------------------------------------------------------------------------
    // 3. Save a copy of the float input for the spectrum bridge (mono-averaged)
    //    We'll do this after step 6 from the double buffer, but we need to
    //    average across input channels here first.
    // -------------------------------------------------------------------------
    // Allocate a small stack buffer for mono-averaged input samples.
    // (numSamples ≤ currentBlockSize_, which is bounded at prepare time.)
    juce::AudioBuffer<float> inputMonoSnapshot (1, numSamples);
    inputMonoSnapshot.clear();
    for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
    {
        inputMonoSnapshot.addFrom (0, 0,
                                    buffer,
                                    ch, 0,
                                    numSamples,
                                    1.0f / static_cast<float> (std::min (numChannels, 2)));
    }

    // -------------------------------------------------------------------------
    // 4. Convert float main buffer → mainDoubleBuffer_ (channels 0 & 1)
    // -------------------------------------------------------------------------
    mainDoubleBuffer_.setSize (2, numSamples, false, false, true /*keep existing*/);
    for (int ch = 0; ch < 2; ++ch)
    {
        const int srcCh = (ch < numChannels) ? ch : 0;
        const float* src = buffer.getReadPointer (srcCh);
        double*       dst = mainDoubleBuffer_.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<double> (src[i]);
    }

    // -------------------------------------------------------------------------
    // 5. Build sidechain double buffer (channels 2–3 if enabled, else copy main)
    // -------------------------------------------------------------------------
    scDoubleBuffer_.setSize (2, numSamples, false, false, true);

    const bool hasSidechain = (numChannels >= 4);

    for (int ch = 0; ch < 2; ++ch)
    {
        double* dst = scDoubleBuffer_.getWritePointer (ch);
        if (hasSidechain)
        {
            const int srcCh = 2 + ch;
            const float* src = buffer.getReadPointer (srcCh);
            for (int i = 0; i < numSamples; ++i)
                dst[i] = static_cast<double> (src[i]);
        }
        else
        {
            // Fallback: use main buffer as sidechain
            const double* main = mainDoubleBuffer_.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
                dst[i] = main[i];
        }
    }

    // -------------------------------------------------------------------------
    // 6. Process sidechain (must be called BEFORE processBlock)
    // -------------------------------------------------------------------------
    dtcwptMain_->processSidechain (scDoubleBuffer_);

    // -------------------------------------------------------------------------
    // 7. Process main audio
    // -------------------------------------------------------------------------
    dtcwptMain_->processBlock (mainDoubleBuffer_);

    // -------------------------------------------------------------------------
    // 8. Convert double output → float main buffer (channels 0 & 1 only)
    // -------------------------------------------------------------------------
    for (int ch = 0; ch < 2; ++ch)
    {
        const double* src = mainDoubleBuffer_.getReadPointer (ch);
        float*         dst = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<float> (src[i]);
    }

    // -------------------------------------------------------------------------
    // 9. Push mono-averaged input + output to SpectrumDataBridge
    // -------------------------------------------------------------------------
    juce::AudioBuffer<float> outputMonoSnapshot (1, numSamples);
    outputMonoSnapshot.clear();
    for (int ch = 0; ch < 2; ++ch)
    {
        outputMonoSnapshot.addFrom (0, 0,
                                     buffer,
                                     ch, 0,
                                     numSamples,
                                     0.5f);
    }

    spectrumBridge_.push (inputMonoSnapshot.getReadPointer (0),
                           outputMonoSnapshot.getReadPointer (0),
                           numSamples);
}

//==============================================================================
// Bus layout
//==============================================================================

bool MorphAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Require stereo main output
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Require stereo main input
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Sidechain input (bus index 1) must be either disabled or stereo
    const auto scLayout = layouts.getChannelSet (true /*isInput*/, 1 /*busIndex*/);
    if (! scLayout.isDisabled() && scLayout != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

//==============================================================================
// Editor
//==============================================================================

juce::AudioProcessorEditor* MorphAudioProcessor::createEditor()
{
    return new MorphEditor (*this);
}

bool MorphAudioProcessor::hasEditor() const
{
    return true;
}

//==============================================================================
// Plugin identity
//==============================================================================

const juce::String MorphAudioProcessor::getName() const
{
    return "DT-CWPT Morph";
}

bool MorphAudioProcessor::acceptsMidi()  const { return false; }
bool MorphAudioProcessor::producesMidi() const { return false; }
bool MorphAudioProcessor::isMidiEffect() const { return false; }

double MorphAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==============================================================================
// Programs
//==============================================================================

int MorphAudioProcessor::getNumPrograms()    { return 1; }
int MorphAudioProcessor::getCurrentProgram() { return 0; }

void MorphAudioProcessor::setCurrentProgram (int /*index*/) {}

const juce::String MorphAudioProcessor::getProgramName (int /*index*/)
{
    return {};
}

void MorphAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/) {}

//==============================================================================
// State serialisation
//==============================================================================

void MorphAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // APVTS copyState serialises all parameters AND any ValueTree properties,
    // including the "topology" string property we set on apvts_.state.
    juce::MemoryOutputStream stream (destData, true);
    apvts_.state.writeToStream (stream);
}

void MorphAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree state = juce::ValueTree::readFromData (data,
                                                            static_cast<std::size_t> (sizeInBytes));
    if (state.isValid())
    {
        apvts_.replaceState (state);

        // Restore topology from the property
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
                // Signal the audio thread to rebuild on the next processBlock call
                topoState_.requestChange (newDests);
            }
        }
    }
}

//==============================================================================
// Parameter layout factory
//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout MorphAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // magnitude: 0→1, default 0
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::Magnitude, 1 },
        "Magnitude",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.0f));

    // phase: 0→1, default 0.7
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::Phase, 1 },
        "Phase",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.7f));

    // threshold: -60→0 dB, default -20 dB
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::Threshold, 1 },
        "Threshold",
        juce::NormalisableRange<float> (-60.0f, 0.0f),
        -20.0f));

    // bypassLow: bool, default true
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamID::BypassLow, 1 },
        "Bypass Low",
        true));

    // bypassHigh: bool, default false
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamID::BypassHigh, 1 },
        "Bypass High",
        false));

    return layout;
}

//==============================================================================
// Plugin entry point — required by JUCE
//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MorphAudioProcessor();
}
