/**
 * @file MorphAudioProcessor.cpp
 * @brief Full implementation of the DT-CWPT Morph Plugin AudioProcessor.
 *
 * Implements parameter management, topology rebuild, float↔double conversion,
 * sidechain handling, spectrum bridge writes, and state persistence.
 */

#include "MorphAudioProcessor.h"
#include "../common/InvalidTopologyDiagnostics.h"
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

    int getTopologyDepth (const std::vector<std::string>& destinations) noexcept
    {
        int depth = 0;

        for (const auto& destination : destinations)
            depth = std::max (depth, static_cast<int> (destination.size()));

        return depth;
    }

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
    apvts_.state.setProperty (topology::kTopologyPropertyKey,
                               topology::serializeTopologyDestinations (currentDestinations_),
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

    // Pre-allocate mono scratch buffers for spectrum bridge (avoids heap alloc in processBlock)
    inputMonoScratch_.setSize  (1, samplesPerBlock);
    outputMonoScratch_.setSize (1, samplesPerBlock);
    alignedInputMonoScratch_.setSize (1, samplesPerBlock);

    analyzerInputAlignerMaxDelay_ = std::max (samplesPerBlock * 8, 0);
    analyzerInputAligner_.prepare (samplesPerBlock, analyzerInputAlignerMaxDelay_);
    analyzerInputAligner_.setDelaySamples (0);

    // Build the DTCWPTProcessor
    rebuildDTCWPT();
}

void MorphAudioProcessor::releaseResources()
{
    dtcwptMain_.reset();
    morphProcessor_ = nullptr;
    mainDoubleBuffer_.setSize (0, 0);
    scDoubleBuffer_.setSize   (0, 0);
    inputMonoScratch_.setSize  (0, 0);
    outputMonoScratch_.setSize (0, 0);
    alignedInputMonoScratch_.setSize (0, 0);
    analyzerInputAligner_.reset();
}

//==============================================================================
// Private helper: rebuild DTCWPTProcessor
//==============================================================================

void MorphAudioProcessor::rebuildDTCWPT()
{
    if (! tryApplyPendingTopologyCandidate())
        (void) tryApplyTopologyCandidate (currentDestinations_);
}

bool MorphAudioProcessor::tryApplyTopologyCandidate (const std::vector<std::string>& candidateDestinations) noexcept
{
    try
    {
        dtcwpt::TopologyConfig config;
        config.destinations = candidateDestinations;
        config.maxDepth = dtcwpt::topology_limits::kSupportedMaxDepth;

        auto newEngine = std::make_unique<dtcwpt::DTCWPTProcessor>();
        auto newMorphProcessor = std::make_unique<MorphBandProcessor>();
        auto* const observedMorphProcessor = newMorphProcessor.get();
        newEngine->setBandProcessor (std::move (newMorphProcessor));
        newEngine->prepareToPlay (currentSampleRate_, currentBlockSize_, config, 2);

        const int newLatency = newEngine->getLatency();
        const int newAnalyzerMaxDelay = std::max (analyzerInputAlignerMaxDelay_, newLatency);

        AnalyzerLatencyAligner newAnalyzerAligner;
        newAnalyzerAligner.prepare (currentBlockSize_, newAnalyzerMaxDelay);
        newAnalyzerAligner.setDelaySamples (newLatency);

        const auto serializedTopology = topology::serializeTopologyDestinations (candidateDestinations);

        dtcwptMain_ = std::move (newEngine);
        morphProcessor_ = observedMorphProcessor;
        currentDestinations_ = candidateDestinations;
        analyzerInputAligner_ = std::move (newAnalyzerAligner);
        analyzerInputAlignerMaxDelay_ = newAnalyzerMaxDelay;
        apvts_.state.setProperty (topology::kTopologyPropertyKey, serializedTopology, nullptr);
        setLatencySamples (newLatency);
        hasCommittedValidTopology_ = true;
        return true;
    }
    catch (const std::invalid_argument&)
    {
        topology::emitInvalidTopologyDiagnostic ("Morph");

        if (! hasCommittedValidTopology_)
            return tryApplyTopologyCandidate (kDefaultTopology);

        return false;
    }
}

bool MorphAudioProcessor::tryApplyPendingTopologyCandidate() noexcept
{
    std::vector<std::string> candidateDestinations;
    if (! topoState_.tryConsume (candidateDestinations))
        return false;

    return tryApplyTopologyCandidate (candidateDestinations);
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
    (void) tryApplyPendingTopologyCandidate();

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
    //    Use the pre-allocated inputMonoScratch_ member to avoid heap allocation
    //    on the audio thread.
    // -------------------------------------------------------------------------
    inputMonoScratch_.clear();
    for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
    {
        inputMonoScratch_.addFrom (0, 0,
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
    //    Use pre-allocated outputMonoScratch_ member to avoid heap allocation
    //    on the audio thread.
    // -------------------------------------------------------------------------
    outputMonoScratch_.clear();
    for (int ch = 0; ch < 2; ++ch)
    {
        outputMonoScratch_.addFrom (0, 0,
                                     buffer,
                                     ch, 0,
                                     numSamples,
                                     0.5f);
    }

    analyzerInputAligner_.processBlock (inputMonoScratch_.getReadPointer (0),
                                        alignedInputMonoScratch_.getWritePointer (0),
                                        numSamples);

    spectrumBridge_.push (alignedInputMonoScratch_.getReadPointer (0),
                           outputMonoScratch_.getReadPointer (0),
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

        const auto restoredTopology = topology::resolvePersistedTopology (apvts_.state, currentDestinations_);
        if (restoredTopology.persistedState != topology::PersistedTopologyState::valid)
        {
            apvts_.state.setProperty (topology::kTopologyPropertyKey,
                                      topology::serializeTopologyDestinations (currentDestinations_),
                                      nullptr);
        }

        if (restoredTopology.persistedState == topology::PersistedTopologyState::invalid)
            topoState_.requestChange ({});
        else
            topoState_.requestChange (restoredTopology.destinations);
    }
}

std::vector<std::string> MorphAudioProcessor::getStoredTopologyDestinations() const
{
    return topology::resolvePersistedTopology (apvts_.state, currentDestinations_).destinations;
}

bool MorphAudioProcessor::applyTopologyCandidateForTest (const std::vector<std::string>& candidateDestinations) noexcept
{
    return tryApplyTopologyCandidate (candidateDestinations);
}

bool MorphAudioProcessor::rebuildPendingTopologyForTest() noexcept
{
    return tryApplyPendingTopologyCandidate();
}

juce::String MorphAudioProcessor::getPersistedTopologyStringForTest() const
{
    return apvts_.state.getProperty (topology::kTopologyPropertyKey).toString();
}

int MorphAudioProcessor::getCommittedTopologyDepthForTest() const noexcept
{
    return getTopologyDepth (currentDestinations_);
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

    // threshold: -200→0 dB, default -20 dB
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::Threshold, 1 },
        "Threshold",
        juce::NormalisableRange<float> (-200.0f, 0.0f),
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
