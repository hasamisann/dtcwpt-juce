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
#include "../common/StateVersioning.h"

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
    bypassRaw_ = apvts_.getRawParameterValue(ParamID::Bypass);
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
    std::unique_lock<std::mutex> lock (controlMutex_);

    currentSampleRate_ = sampleRate;
    currentBlockSize_  = std::max(1, samplesPerBlock);
    samplesPerBlock = currentBlockSize_;

    // Allocate double-precision conversion buffers
    mainDoubleBuffer_.setSize (2, samplesPerBlock);
    scDoubleBuffer_.setSize   (2, samplesPerBlock);
    fadeOutBuffer_.setSize    (2, samplesPerBlock);

    // Pre-allocate mono scratch buffers for spectrum bridge (avoids heap alloc in processBlock)
    inputMonoScratch_.setSize  (1, samplesPerBlock);
    outputMonoScratch_.setSize (1, samplesPerBlock);
    alignedInputMonoScratch_.setSize (1, samplesPerBlock);

    fadeLength_ = std::max (1, juce::roundToInt (sampleRate * kSwitchFadeSeconds));
    latencyBypass_.prepare(sampleRate, samplesPerBlock, 2,
        latency_bypass::maxEngineLatencySamples(dtcwpt::topology_limits::kSupportedMaxDepth, samplesPerBlock));
    displayWasActive_ = false;
    prepared_ = true;

    // The audio thread is not running here, so the new engine is installed directly.
    releaseEnginesLocked();
    installEngineForPrepareLocked();
    const int latency = committedLatency_;
    lock.unlock();
    setLatencySamples (latency);
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
    alignedInputMonoScratch_.setSize (0, 0);    latencyBypass_.release();
    committedTailLength_.store(0, 0.0);
    engineAvailable_.store(false, std::memory_order_release);
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
#if DTCWPT_ENABLE_TEST_SEAMS
    if (engineBuildFailuresRemainingForTest_ > 0)
    {
        --engineBuildFailuresRemainingForTest_;
        bundle->engine->setPrepareCandidateFailpointForTesting(engineBuildFailpointForTest_);
    }
#endif

    auto morphProcessor = std::make_unique<MorphBandProcessor>();
    bundle->morph = morphProcessor.get();
    bundle->engine->setBandProcessor (std::move (morphProcessor));
    bundle->engine->prepareToPlay (currentSampleRate_, currentBlockSize_, config, 2);

    bundle->morph->reserveTargetEvents(static_cast<std::size_t>(std::max(1, currentBlockSize_)));
    bundle->latency = bundle->engine->getLatency();
    bundle->tailSamples = bundle->engine->getTailLengthSamples();
    bundle->aligner.prepare (currentBlockSize_, std::max (currentBlockSize_ * 8, bundle->latency));
    bundle->aligner.setDelaySamples (bundle->latency);

    return bundle;
}

bool MorphAudioProcessor::requestTopologyChange (const std::vector<std::string>& destinations)
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

std::vector<std::string> MorphAudioProcessor::getCommittedTopology() const
{
    std::lock_guard<std::mutex> lock (controlMutex_);
    return committedDestinations_;
}

bool MorphAudioProcessor::commitTopologyLocked(const std::vector<std::string>& candidate)
{
    try
    {
        std::unique_ptr<EngineBundle> bundle;
        if (prepared_)
            bundle = buildEngineBundle(candidate);
        else if (!topology::detail::validatePersistedTopologyDestinations(candidate))
            throw std::invalid_argument("Invalid topology");
        auto committed = candidate;
        auto lastBuilt = candidate;
        // All potentially allocating work is complete before changing the public state.
        committedDestinations_ = std::move(committed);
        topologyGeneration_.fetch_add(1, std::memory_order_acq_rel);
        if (bundle != nullptr)
        {
            lastBuiltDestinations_ = std::move(lastBuilt);
            hasCommittedValidTopology_ = true;
            committedLatency_ = bundle->latency;
            committedTailLength_.store(bundle->tailSamples, currentSampleRate_);
            publishBundleLocked(std::move(bundle));
            engineAvailable_.store(true, std::memory_order_release);
        }
        return true;
    }
    catch (const std::invalid_argument&)
    {
        topology::emitInvalidTopologyDiagnostic("Morph");
    }
    catch (...) {}
    return false;
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
    if (!commitTopologyLocked(committedDestinations_))
    {
        const auto& fallback = hasCommittedValidTopology_ ? lastBuiltDestinations_ : kDefaultTopology;
        if (committedDestinations_ == fallback || !commitTopologyLocked(fallback))
        {
            committedLatency_ = 0;
            committedTailLength_.store(0, 0.0);
            engineAvailable_.store(false, std::memory_order_release);
            return;
        }
    }
    (void) acquirePendingEngine(true);
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
        active_->fadeInOnStart = true;
        fadePosition_ = 0;
    }

    latencyBypass_.setDelaySamples(active_->latency);
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

void MorphAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { processBlockInternal(buffer, false); }
void MorphAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { processBlockInternal(buffer, true); }
juce::AudioProcessorParameter* MorphAudioProcessor::getBypassParameter() const { return apvts_.getParameter("bypass"); }

void MorphAudioProcessor::processBlockInternal(juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    if (buffer.getNumSamples() <= 0) return;
    (void) acquirePendingEngine(false);
    if (active_ == nullptr || buffer.getNumChannels() < 1)
    {
        buffer.clear();
        return;
    }
    const double magnitude = apvts_.getRawParameterValue(ParamID::Magnitude)->load();
    const double phase = apvts_.getRawParameterValue(ParamID::Phase)->load();
    const double threshold = juce::Decibels::decibelsToGain(apvts_.getRawParameterValue(ParamID::Threshold)->load());
    const bool bypassLow = apvts_.getRawParameterValue(ParamID::BypassLow)->load() >= 0.5f;
    const bool bypassHigh = apvts_.getRawParameterValue(ParamID::BypassHigh)->load() >= 0.5f;
    for (auto* bundle : { active_, fadingOut_ })
        if (bundle != nullptr)
            bundle->morph->setTargets(magnitude, phase, threshold, bypassLow, bypassHigh, bundle->inputPosition);

    latencyBypass_.setBypassed(forceBypass || bypassRaw_->load() >= 0.5f);
    const bool display = displayConsumers_.isActive();
    if (display && !displayWasActive_)
    {
        active_->aligner.reset();
        spectrumBridge_.discardPartialSlot();
    }
    displayWasActive_ = display;
    const int chunkSize = mainDoubleBuffer_.getNumSamples();
    for (int start = 0; start < buffer.getNumSamples(); start += chunkSize)
        processChunk(buffer, start, std::min(chunkSize, buffer.getNumSamples() - start));
}

void MorphAudioProcessor::processChunk(juce::AudioBuffer<float>& buffer, int start, int count)
{
    juce::AudioBuffer<double> main(mainDoubleBuffer_.getArrayOfWritePointers(), 2, count);
    juce::AudioBuffer<double> old(fadeOutBuffer_.getArrayOfWritePointers(), 2, count);
    juce::AudioBuffer<double> sidechain(scDoubleBuffer_.getArrayOfWritePointers(), 2, count);

    for (int ch = 0; ch < 2; ++ch)
    {
        const float* input = buffer.getReadPointer(std::min(ch, buffer.getNumChannels() - 1)) + start;
        for (int i = 0; i < count; ++i)
        {
            main.getWritePointer(ch)[i] = input[i];
            if (fadingOut_ != nullptr) old.getWritePointer(ch)[i] = input[i];
        }
        const float* control = buffer.getReadPointer(buffer.getNumChannels() >= 4 ? 2 + ch : std::min(ch, buffer.getNumChannels() - 1)) + start;
        for (int i = 0; i < count; ++i) sidechain.getWritePointer(ch)[i] = control[i];

    }
    if (displayWasActive_)
        for (int i = 0; i < count; ++i)
            inputMonoScratch_.getWritePointer(0)[i] = static_cast<float>((main.getReadPointer(0)[i] + main.getReadPointer(1)[i]) * 0.5);
    latencyBypass_.captureDry(main);
    if (fadingOut_ != nullptr) fadingOut_->engine->processBlock(old, &sidechain);
    active_->engine->processBlock(main, &sidechain);

    if (active_->fadeInOnStart)
    {
        for (int i = 0; i < count; ++i)
        {
            const double gain = std::clamp(static_cast<double>(active_->inputPosition + i - active_->latency + 1)
                                           / fadeLength_, 0.0, 1.0);
            for (int ch = 0; ch < 2; ++ch) main.getWritePointer(ch)[i] *= gain;
        }
        if (active_->inputPosition + count >= active_->latency + fadeLength_) active_->fadeInOnStart = false;
    }
    if (fadingOut_ != nullptr)
    {
        for (int i = 0; i < count; ++i)
            for (int ch = 0; ch < 2; ++ch)
                main.getWritePointer(ch)[i] += old.getReadPointer(ch)[i] * (1.0 - fadeInGain(fadePosition_ + i));
        fadingOut_->inputPosition += count;
        fadePosition_ += count;
        if (fadePosition_ >= fadeLength_)
        {
            (void) handoff_.retire(fadingOut_);
            fadingOut_ = nullptr;
            fadePosition_ = 0;
        }
    }
    active_->inputPosition += count;
    activeHasProcessed_ = true;
    latencyBypass_.mixInto(main);
    for (int ch = 0; ch < std::min(2, buffer.getNumChannels()); ++ch)
        for (int i = 0; i < count; ++i)
            buffer.getWritePointer(ch)[start + i] = static_cast<float>(main.getReadPointer(ch)[i]);
    if (displayWasActive_)
    {
        for (int i = 0; i < count; ++i)
            outputMonoScratch_.getWritePointer(0)[i] = static_cast<float>((main.getReadPointer(0)[i] + main.getReadPointer(1)[i]) * 0.5);
        active_->aligner.processBlock(inputMonoScratch_.getReadPointer(0), alignedInputMonoScratch_.getWritePointer(0), count);
        spectrumBridge_.push(alignedInputMonoScratch_.getReadPointer(0), outputMonoScratch_.getReadPointer(0), count);
    }
}

void MorphAudioProcessor::reset()
{
    for (auto* bundle : { active_, fadingOut_ })
        if (bundle != nullptr)
        {
            bundle->engine->reset();
            bundle->aligner.reset();
            bundle->inputPosition = 0;
            bundle->fadeInOnStart = false;
        }
    spectrumBridge_.reset();
    fadePosition_ = 0;
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
    return committedTailLength_.getSeconds();
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

    state.setProperty(statever::kStateVersionPropertyKey, statever::kCurrentStateVersion, nullptr);
    juce::MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void MorphAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree state = statever::readVersionedState(data, sizeInBytes, "MorphParams");
    if (! state.isValid())
        return;

    std::unique_lock<std::mutex> lock (controlMutex_);

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

    const int latency = prepared_ ? committedLatency_ : -1;
    lock.unlock();
    if (latency >= 0)
        setLatencySamples (latency);
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

    // bypass: host bypass, latency-compensated; default off
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamID::Bypass, 1 },
        "Bypass",
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
