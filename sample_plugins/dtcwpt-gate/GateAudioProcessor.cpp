#include "GateAudioProcessor.h"
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

    constexpr int kGateTopologyMaxDepth = 8;

    /** Length of the cross-fade when the audio thread switches to a new engine. */
    constexpr double kSwitchFadeSeconds = 0.010;

    /** How often the message thread destroys engines retired by the audio thread. */
    constexpr int kReclaimIntervalMs = 250;

} // namespace

//==============================================================================
// Constructor / Destructor
//==============================================================================

GateAudioProcessor::GateAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)),
      apvts_ (*this, nullptr, "GateParams", createParameterLayout()),
      committedDestinations_ (kDefaultTopology)
{
    // Cache the 256 parameter pointers
    for (int i = 0; i < GateBandProcessor::kMaxBands; ++i)
    {
        juce::String paramID = juce::String::formatted("threshold_%02d", i);
        thresholdRaw_[i] = apvts_.getRawParameterValue(paramID);
    }
    
    // Cache the bypass parameter
    bypassLowestRaw_ = apvts_.getRawParameterValue("bypass_lowest_band");

    // Engines retired by the audio thread are destroyed on the message thread.
    startTimer (kReclaimIntervalMs);
}

GateAudioProcessor::~GateAudioProcessor()
{
    stopTimer();

    std::lock_guard<std::mutex> lock (controlMutex_);
    releaseEnginesLocked();
}

//==============================================================================
// Lifecycle
//==============================================================================

void GateAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    std::unique_lock<std::mutex> lock (controlMutex_);

    currentSampleRate_ = sampleRate;
    currentBlockSize_  = samplesPerBlock;

    mainDoubleBuffer_.setSize (2, samplesPerBlock);
    fadeOutBuffer_.setSize    (2, samplesPerBlock);

    inputMonoScratch_.setSize  (1, samplesPerBlock);
    outputMonoScratch_.setSize (1, samplesPerBlock);
    alignedInputMonoScratch_.setSize (1, samplesPerBlock);

    fadeLength_ = std::max (1, juce::roundToInt (sampleRate * kSwitchFadeSeconds));
    prepared_ = true;

    // The audio thread is not running here, so the new engine is installed directly.
    releaseEnginesLocked();
    installEngineForPrepareLocked();
    const int latency = committedLatency_;
    lock.unlock();
    setLatencySamples (latency);
}

void GateAudioProcessor::releaseResources()
{
    std::lock_guard<std::mutex> lock (controlMutex_);

    prepared_ = false;
    releaseEnginesLocked();
    mainDoubleBuffer_.setSize (0, 0);
    fadeOutBuffer_.setSize    (0, 0);
    inputMonoScratch_.setSize  (0, 0);
    outputMonoScratch_.setSize (0, 0);
    alignedInputMonoScratch_.setSize (0, 0);
}

//==============================================================================
// Topology (control side: message / host threads)
//==============================================================================

std::unique_ptr<GateAudioProcessor::EngineBundle>
GateAudioProcessor::buildEngineBundle (const std::vector<std::string>& destinations) const
{
    dtcwpt::TopologyConfig config;
    config.destinations = destinations;
    config.maxDepth = kGateTopologyMaxDepth;

    auto bundle = std::make_unique<EngineBundle>();
    bundle->engine = std::make_unique<dtcwpt::DTCWPTProcessor>();

    auto gateProcessor = std::make_unique<GateBandProcessor>();
    bundle->gate = gateProcessor.get();
    bundle->engine->setBandProcessor (std::move (gateProcessor));
    bundle->engine->prepareToPlay (currentSampleRate_, currentBlockSize_, config, 2);

    bundle->latency = bundle->engine->getLatency();
    bundle->numBands = static_cast<int> (std::min (destinations.size(),
                                                   static_cast<std::size_t> (GateBandProcessor::kMaxBands)));
    bundle->aligner.prepare (currentBlockSize_, std::max (currentBlockSize_ * 8, bundle->latency));
    bundle->aligner.setDelaySamples (bundle->latency);

    return bundle;
}

bool GateAudioProcessor::requestTopologyChange (const std::vector<std::string>& destinations)
{
    bool accepted;
    int latency = -1;
    {
        std::lock_guard<std::mutex> lock (controlMutex_);
        accepted = commitTopologyLocked (destinations);
        if (accepted && prepared_)
            latency = committedLatency_;
    }
    // JUCE synchronously invokes host listeners; they may read our state.
    if (latency >= 0)
        setLatencySamples (latency);
    return accepted;
}

std::vector<std::string> GateAudioProcessor::getCommittedTopology() const
{
    std::lock_guard<std::mutex> lock (controlMutex_);
    return committedDestinations_;
}

bool GateAudioProcessor::commitTopologyLocked (const std::vector<std::string>& candidate)
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
            topology::emitInvalidTopologyDiagnostic ("Gate");
            return false;
        }
    }
    else if (! topology::detail::validatePersistedTopologyDestinations (candidate))
    {
        // No sample rate / block size yet, so no engine can be built. Obviously malformed topologies are
        // rejected here; the rest is accepted and validated for real in prepareToPlay().
        topology::emitInvalidTopologyDiagnostic ("Gate");
        return false;
    }

    setCommittedTopologyLocked (candidate);

    if (bundle != nullptr)
    {
        lastBuiltDestinations_ = candidate;
        hasCommittedValidTopology_ = true;
        committedLatency_ = bundle->latency;
        publishBundleLocked (std::move (bundle));
    }

    return true;
}

void GateAudioProcessor::setCommittedTopologyLocked (const std::vector<std::string>& destinations)
{
    committedDestinations_ = destinations;
    topologyGeneration_.fetch_add (1, std::memory_order_acq_rel);
}

void GateAudioProcessor::publishBundleLocked (std::unique_ptr<EngineBundle> bundle)
{
    handoff_.reclaim();

    // A bundle that is still deferred belongs to an older request and is superseded.
    deferredBundle_.reset();

    if (! handoff_.publish (bundle))
        deferredBundle_ = std::move (bundle);   // too many engines alive; reclaimLocked() retries
}

void GateAudioProcessor::installEngineForPrepareLocked()
{
    std::unique_ptr<EngineBundle> bundle;

    try
    {
        bundle = buildEngineBundle (committedDestinations_);
    }
    catch (const std::invalid_argument&)
    {
        topology::emitInvalidTopologyDiagnostic ("Gate");
        setCommittedTopologyLocked (hasCommittedValidTopology_ ? lastBuiltDestinations_ : kDefaultTopology);
        bundle = buildEngineBundle (committedDestinations_);
    }

    lastBuiltDestinations_ = committedDestinations_;
    hasCommittedValidTopology_ = true;
    committedLatency_ = bundle->latency;
    publishBundleLocked (std::move (bundle));
    (void) acquirePendingEngine (true);
}

void GateAudioProcessor::releaseEnginesLocked()
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

void GateAudioProcessor::reclaimLocked()
{
    handoff_.reclaim();

    if (deferredBundle_ != nullptr)
        (void) handoff_.publish (deferredBundle_);
}

void GateAudioProcessor::timerCallback()
{
    std::unique_lock<std::mutex> lock (controlMutex_, std::try_to_lock);

    if (lock.owns_lock())
        reclaimLocked();
}

//==============================================================================
// Topology (audio side)
//==============================================================================

bool GateAudioProcessor::acquirePendingEngine (bool immediate) noexcept
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

double GateAudioProcessor::fadeInGain (int position) const noexcept
{
    if (position + 1 >= fadeLength_)
        return 1.0;

    return static_cast<double> (position + 1) / static_cast<double> (fadeLength_);
}

//==============================================================================
// Processing
//==============================================================================

void GateAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    if (active_ == nullptr)
        return;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (numSamples == 0 || numChannels == 0)
        return;

    // 1. Take an engine built by the control side, if there is one. Switching to it is a
    //    short cross-fade; nothing is built, freed or reported to the host here.
    (void) acquirePendingEngine (false);

    EngineBundle* const outgoing = fadingOut_;

    // 2. Read 256 threshold params → linear → set thresholds
    const size_t numBands = static_cast<size_t> (active_->numBands);
    std::array<double, GateBandProcessor::kMaxBands> thresholds {};
    for (size_t i = 0; i < numBands; ++i)
    {
        if (thresholdRaw_[i] != nullptr)
        {
            float db = thresholdRaw_[i]->load();
            thresholds[i] = std::pow(10.0, db / 20.0);
        }
    }
    lastThresholdsForTest_ = thresholds;
    active_->gate->setThresholds(thresholds.data(), static_cast<int>(numBands));

    lastBypassLowestForTest_ = bypassLowestRaw_ != nullptr && bypassLowestRaw_->load() >= 0.5f;
    active_->gate->setBypassLowest(lastBypassLowestForTest_);

    if (outgoing != nullptr)
    {
        // The outgoing engine has its own band count; thresholds beyond it are ignored by setThresholds.
        std::array<double, GateBandProcessor::kMaxBands> outgoingThresholds {};
        const size_t outgoingBands = static_cast<size_t> (outgoing->numBands);
        for (size_t i = 0; i < outgoingBands; ++i)
        {
            if (thresholdRaw_[i] != nullptr)
                outgoingThresholds[i] = std::pow (10.0, thresholdRaw_[i]->load() / 20.0);
        }

        outgoing->gate->setThresholds (outgoingThresholds.data(), static_cast<int> (outgoingBands));
        outgoing->gate->setBypassLowest (lastBypassLowestForTest_);
    }

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

    // 5. Process audio. During a switch the outgoing engine keeps running on the unfaded
    //    input and the incoming engine gets the input faded in.
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

        outgoing->engine->processBlock (fadeOutBuffer_);
    }

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

    // 6. Push levels to bandLevelBridge_
    float levels[GateBandProcessor::kMaxBands];
    int nBands = active_->gate->getBandLevelsDb(levels);
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
    std::lock_guard<std::mutex> lock (controlMutex_);

    // Save a copy: the committed topology is attached to the copy, the live ValueTree is not written to.
    juce::ValueTree state = apvts_.copyState();
    state.setProperty (topology::kTopologyPropertyKey,
                       topology::serializeTopologyDestinations (committedDestinations_),
                       nullptr);

    juce::MemoryOutputStream stream (destData, true);
    state.writeToStream (stream);
}

void GateAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree state = juce::ValueTree::readFromData (data,
                                                             static_cast<std::size_t> (sizeInBytes));
    if (! state.isValid())
        return;

    std::unique_lock<std::mutex> lock (controlMutex_);

    const auto restoredTopology = topology::resolvePersistedTopology (state, committedDestinations_);
    apvts_.replaceState (state);

    if (restoredTopology.persistedState == topology::PersistedTopologyState::invalid)
    {
        topology::emitInvalidTopologyDiagnostic ("Gate");

        if (! hasCommittedValidTopology_)
            setCommittedTopologyLocked (kDefaultTopology);

        return;
    }

    if (restoredTopology.persistedState == topology::PersistedTopologyState::valid)
        (void) commitTopologyLocked (restoredTopology.destinations);

    const int latency = prepared_ ? committedLatency_ : -1;
    lock.unlock();
    if (latency >= 0)
        setLatencySamples (latency);
}

bool GateAudioProcessor::applyTopologyCandidateForTest (const std::vector<std::string>& candidateDestinations)
{
    const bool accepted = requestTopologyChange (candidateDestinations);
    (void) adoptPendingEngineForTest();
    return accepted;
}

bool GateAudioProcessor::adoptPendingEngineForTest() noexcept
{
    return acquirePendingEngine (true);
}

int GateAudioProcessor::getAnalyzerInputDelaySamplesForTest() const noexcept
{
    return active_ != nullptr ? active_->aligner.getDelaySamples() : 0;
}

double GateAudioProcessor::getLastThresholdLinearForTest (int bandIndex) const noexcept
{
    if (bandIndex < 0 || bandIndex >= GateBandProcessor::kMaxBands)
        return 0.0;

    return lastThresholdsForTest_[static_cast<std::size_t> (bandIndex)];
}

bool GateAudioProcessor::getLastBypassLowestForTest() const noexcept
{
    return lastBypassLowestForTest_;
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
