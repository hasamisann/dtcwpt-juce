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

    /** Length of the cross-fade when the audio thread switches to a new engine. */
    constexpr double kSwitchFadeSeconds = 0.010;

    /** How often the message thread destroys engines retired by the audio thread. */
    constexpr int kReclaimIntervalMs = 250;

    int getTopologyDepth (const std::vector<std::string>& destinations) noexcept
    {
        int depth = 0;

        for (const auto& destination : destinations)
            depth = std::max (depth, static_cast<int> (destination.size()));

        return depth;
    }

} // namespace

//==============================================================================
// Constructor / Destructor
//==============================================================================

MorphAudioProcessor::MorphAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts_ (*this, nullptr, "MorphParams", createParameterLayout()),
      committedDestinations_ (kDefaultTopology)
{
    // Engines retired by the audio thread are destroyed on the message thread.
    startTimer (kReclaimIntervalMs);
}

MorphAudioProcessor::~MorphAudioProcessor()
{
    stopTimer();

    std::lock_guard<std::mutex> lock (controlMutex_);
    releaseEnginesLocked();
}

//==============================================================================
// Lifecycle
//==============================================================================

void MorphAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    std::lock_guard<std::mutex> lock (controlMutex_);

    currentSampleRate_ = sampleRate;
    currentBlockSize_  = samplesPerBlock;

    // Allocate double-precision conversion buffers
    mainDoubleBuffer_.setSize (2, samplesPerBlock);
    scDoubleBuffer_.setSize   (2, samplesPerBlock);
    fadeOutBuffer_.setSize    (2, samplesPerBlock);

    // Pre-allocate mono scratch buffers for spectrum bridge (avoids heap alloc in processBlock)
    inputMonoScratch_.setSize  (1, samplesPerBlock);
    outputMonoScratch_.setSize (1, samplesPerBlock);
    alignedInputMonoScratch_.setSize (1, samplesPerBlock);

    fadeLength_ = std::max (1, juce::roundToInt (sampleRate * kSwitchFadeSeconds));
    prepared_ = true;

    // The audio thread is not running here, so the new engine is installed directly.
    releaseEnginesLocked();
    installEngineForPrepareLocked();
}

void MorphAudioProcessor::releaseResources()
{
    std::lock_guard<std::mutex> lock (controlMutex_);

    prepared_ = false;
    releaseEnginesLocked();
    mainDoubleBuffer_.setSize (0, 0);
    scDoubleBuffer_.setSize   (0, 0);
    fadeOutBuffer_.setSize    (0, 0);
    inputMonoScratch_.setSize  (0, 0);
    outputMonoScratch_.setSize (0, 0);
    alignedInputMonoScratch_.setSize (0, 0);
}

//==============================================================================
// Topology (control side: message / host threads)
//==============================================================================

std::unique_ptr<MorphAudioProcessor::EngineBundle>
MorphAudioProcessor::buildEngineBundle (const std::vector<std::string>& destinations) const
{
    dtcwpt::TopologyConfig config;
    config.destinations = destinations;
    config.maxDepth = dtcwpt::topology_limits::kSupportedMaxDepth;

    auto bundle = std::make_unique<EngineBundle>();
    bundle->engine = std::make_unique<dtcwpt::DTCWPTProcessor>();

    auto morphProcessor = std::make_unique<MorphBandProcessor>();
    bundle->morph = morphProcessor.get();
    bundle->engine->setBandProcessor (std::move (morphProcessor));
    bundle->engine->prepareToPlay (currentSampleRate_, currentBlockSize_, config, 2);

    bundle->latency = bundle->engine->getLatency();
    bundle->aligner.prepare (currentBlockSize_, std::max (currentBlockSize_ * 8, bundle->latency));
    bundle->aligner.setDelaySamples (bundle->latency);

    return bundle;
}

bool MorphAudioProcessor::requestTopologyChange (const std::vector<std::string>& destinations)
{
    std::lock_guard<std::mutex> lock (controlMutex_);
    return commitTopologyLocked (destinations);
}

std::vector<std::string> MorphAudioProcessor::getCommittedTopology() const
{
    std::lock_guard<std::mutex> lock (controlMutex_);
    return committedDestinations_;
}

bool MorphAudioProcessor::commitTopologyLocked (const std::vector<std::string>& candidate)
{
    std::unique_ptr<EngineBundle> bundle;

    if (prepared_)
    {
        try
        {
            bundle = buildEngineBundle (candidate);
        }
        catch (const std::invalid_argument&)
        {
            topology::emitInvalidTopologyDiagnostic ("Morph");
            return false;
        }
    }
    else if (! topology::detail::validatePersistedTopologyDestinations (candidate))
    {
        // No sample rate / block size yet, so no engine can be built. Obviously malformed topologies are
        // rejected here; the rest is accepted and validated for real in prepareToPlay().
        topology::emitInvalidTopologyDiagnostic ("Morph");
        return false;
    }

    setCommittedTopologyLocked (candidate);

    if (bundle != nullptr)
    {
        lastBuiltDestinations_ = candidate;
        hasCommittedValidTopology_ = true;
        setLatencySamples (bundle->latency);
        publishBundleLocked (std::move (bundle));
    }

    return true;
}

void MorphAudioProcessor::setCommittedTopologyLocked (const std::vector<std::string>& destinations)
{
    committedDestinations_ = destinations;
    topologyGeneration_.fetch_add (1, std::memory_order_acq_rel);
}

void MorphAudioProcessor::publishBundleLocked (std::unique_ptr<EngineBundle> bundle)
{
    handoff_.reclaim();

    // A bundle that is still deferred belongs to an older request and is superseded.
    deferredBundle_.reset();

    if (! handoff_.publish (bundle))
        deferredBundle_ = std::move (bundle);   // too many engines alive; reclaimLocked() retries
}

void MorphAudioProcessor::installEngineForPrepareLocked()
{
    std::unique_ptr<EngineBundle> bundle;

    try
    {
        bundle = buildEngineBundle (committedDestinations_);
    }
    catch (const std::invalid_argument&)
    {
        topology::emitInvalidTopologyDiagnostic ("Morph");
        setCommittedTopologyLocked (hasCommittedValidTopology_ ? lastBuiltDestinations_ : kDefaultTopology);
        bundle = buildEngineBundle (committedDestinations_);
    }

    lastBuiltDestinations_ = committedDestinations_;
    hasCommittedValidTopology_ = true;
    setLatencySamples (bundle->latency);
    publishBundleLocked (std::move (bundle));
    (void) acquirePendingEngine (true);
}

void MorphAudioProcessor::releaseEnginesLocked()
{
    // Only called while the audio thread is not running, so this thread may act as its consumer.
    if (fadingOut_ != nullptr)
    {
        (void) handoff_.retire (fadingOut_);
        fadingOut_ = nullptr;
    }

    if (active_ != nullptr)
    {
        (void) handoff_.retire (active_);
        active_ = nullptr;
    }

    fadePosition_ = 0;
    activeHasProcessed_ = false;

    handoff_.discardPending();
    deferredBundle_.reset();
    handoff_.reclaim();
}

void MorphAudioProcessor::reclaimLocked()
{
    handoff_.reclaim();

    if (deferredBundle_ != nullptr)
        (void) handoff_.publish (deferredBundle_);
}

void MorphAudioProcessor::timerCallback()
{
    std::unique_lock<std::mutex> lock (controlMutex_, std::try_to_lock);

    if (lock.owns_lock())
        reclaimLocked();
}

//==============================================================================
// Topology (audio side)
//==============================================================================

bool MorphAudioProcessor::acquirePendingEngine (bool immediate) noexcept
{
    if (fadingOut_ != nullptr)
    {
        // Finish the running switch first; the latest pending engine waits in the hand-off.
        if (! immediate)
            return false;

        (void) handoff_.retire (fadingOut_);
        fadingOut_ = nullptr;
        fadePosition_ = 0;
    }

    EngineBundle* const incoming = handoff_.acquire();
    if (incoming == nullptr)
        return false;

    if (active_ == nullptr || immediate || ! activeHasProcessed_)
    {
        // Nothing audible to fade from.
        if (active_ != nullptr)
            (void) handoff_.retire (active_);

        active_ = incoming;
    }
    else
    {
        fadingOut_ = active_;
        active_ = incoming;
        fadePosition_ = 0;
    }

    activeHasProcessed_ = false;
    return true;
}

double MorphAudioProcessor::fadeInGain (int position) const noexcept
{
    if (position + 1 >= fadeLength_)
        return 1.0;

    return static_cast<double> (position + 1) / static_cast<double> (fadeLength_);
}

//==============================================================================
// Processing
//==============================================================================

void MorphAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    // Safety check: processor must be prepared
    if (active_ == nullptr)
        return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // -------------------------------------------------------------------------
    // 1. Take an engine built by the control side, if there is one. Switching to it
    //    is a short cross-fade; nothing is built, freed or reported to the host here.
    // -------------------------------------------------------------------------
    (void) acquirePendingEngine (false);

    // -------------------------------------------------------------------------
    // 2. Read APVTS atomic float parameters → set targets on MorphBandProcessor(s)
    // -------------------------------------------------------------------------
    const float magnitudeF  = apvts_.getRawParameterValue (ParamID::Magnitude)->load();
    const float phaseF      = apvts_.getRawParameterValue (ParamID::Phase)->load();
    const float thresholdDB = apvts_.getRawParameterValue (ParamID::Threshold)->load();
    const bool  bypassLow   = apvts_.getRawParameterValue (ParamID::BypassLow)->load() >= 0.5f;
    const bool  bypassHigh  = apvts_.getRawParameterValue (ParamID::BypassHigh)->load() >= 0.5f;

    const double threshLinear = juce::Decibels::decibelsToGain (thresholdDB);

    active_->morph->setTargets (static_cast<double> (magnitudeF),
                                static_cast<double> (phaseF),
                                threshLinear,
                                bypassLow,
                                bypassHigh);

    EngineBundle* const outgoing = fadingOut_;

    if (outgoing != nullptr)
    {
        outgoing->morph->setTargets (static_cast<double> (magnitudeF),
                                     static_cast<double> (phaseF),
                                     threshLinear,
                                     bypassLow,
                                     bypassHigh);
    }

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
    // 6. Process sidechain (must be called BEFORE processBlock) and main audio.
    //    During a switch the outgoing engine keeps running on the unfaded input
    //    and the incoming engine gets the input faded in.
    // -------------------------------------------------------------------------
    if (outgoing != nullptr)
    {
        fadeOutBuffer_.setSize (2, numSamples, false, false, true);

        for (int ch = 0; ch < 2; ++ch)
        {
            const double* main = mainDoubleBuffer_.getReadPointer (ch);
            double* const old = fadeOutBuffer_.getWritePointer (ch);
            std::copy (main, main + numSamples, old);
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            double* const main = mainDoubleBuffer_.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                main[i] *= fadeInGain (fadePosition_ + i);
        }

        outgoing->engine->processSidechain (scDoubleBuffer_);
        outgoing->engine->processBlock (fadeOutBuffer_);
    }

    active_->engine->processSidechain (scDoubleBuffer_);
    active_->engine->processBlock (mainDoubleBuffer_);

    if (outgoing != nullptr)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            double* const main = mainDoubleBuffer_.getWritePointer (ch);
            const double* const old = fadeOutBuffer_.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
                main[i] += old[i] * (1.0 - fadeInGain (fadePosition_ + i));
        }

        fadePosition_ += numSamples;

        if (fadePosition_ >= fadeLength_)
        {
            (void) handoff_.retire (outgoing);
            fadingOut_ = nullptr;
            fadePosition_ = 0;
        }
    }

    activeHasProcessed_ = true;

    // -------------------------------------------------------------------------
    // 7. Convert double output → float main buffer (channels 0 & 1 only)
    // -------------------------------------------------------------------------
    for (int ch = 0; ch < 2; ++ch)
    {
        const double* src = mainDoubleBuffer_.getReadPointer (ch);
        float*         dst = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            dst[i] = static_cast<float> (src[i]);
    }

    // -------------------------------------------------------------------------
    // 8. Push mono-averaged input + output to SpectrumDataBridge
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

    active_->aligner.processBlock (inputMonoScratch_.getReadPointer (0),
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
    std::lock_guard<std::mutex> lock (controlMutex_);

    // Save a copy: the committed topology is attached to the copy, the live ValueTree is not written to.
    juce::ValueTree state = apvts_.copyState();
    state.setProperty (topology::kTopologyPropertyKey,
                       topology::serializeTopologyDestinations (committedDestinations_),
                       nullptr);

    juce::MemoryOutputStream stream (destData, true);
    state.writeToStream (stream);
}

void MorphAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree state = juce::ValueTree::readFromData (data,
                                                            static_cast<std::size_t> (sizeInBytes));
    if (! state.isValid())
        return;

    std::lock_guard<std::mutex> lock (controlMutex_);

    const auto restoredTopology = topology::resolvePersistedTopology (state, committedDestinations_);
    apvts_.replaceState (state);

    if (restoredTopology.persistedState == topology::PersistedTopologyState::invalid)
    {
        topology::emitInvalidTopologyDiagnostic ("Morph");

        if (! hasCommittedValidTopology_)
            setCommittedTopologyLocked (kDefaultTopology);

        return;
    }

    if (restoredTopology.persistedState == topology::PersistedTopologyState::valid)
        (void) commitTopologyLocked (restoredTopology.destinations);
}

bool MorphAudioProcessor::applyTopologyCandidateForTest (const std::vector<std::string>& candidateDestinations)
{
    const bool accepted = requestTopologyChange (candidateDestinations);
    (void) adoptPendingEngineForTest();
    return accepted;
}

bool MorphAudioProcessor::adoptPendingEngineForTest() noexcept
{
    return acquirePendingEngine (true);
}

int MorphAudioProcessor::getCommittedTopologyDepthForTest() const
{
    return getTopologyDepth (getCommittedTopology());
}

int MorphAudioProcessor::getAnalyzerInputDelaySamplesForTest() const noexcept
{
    return active_ != nullptr ? active_->aligner.getDelaySamples() : 0;
}

bool MorphAudioProcessor::consumeSpectrumSnapshotForTest (
    std::array<float, SpectrumDataBridge::kSlotSize>& inputOut,
    std::array<float, SpectrumDataBridge::kSlotSize>& outputOut)
{
    return spectrumBridge_.tryConsume (inputOut, outputOut);
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
