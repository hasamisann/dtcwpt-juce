#include "GateAudioProcessor.h"
#include "../common/InvalidTopologyDiagnostics.h"
#include "../common/TopologyPersistence.h"
#include "../common/StateVersioning.h"

#include <juce_core/juce_core.h>
#include <limits>

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
    for (std::size_t i = 0; i < thresholdRaw_.size(); ++i)
        thresholdRaw_[i] = apvts_.getRawParameterValue(getThresholdParameterId(static_cast<int>(i)));
    bypassRaw_ = apvts_.getRawParameterValue("bypass");

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
    currentBlockSize_  = std::max(1, samplesPerBlock);
    samplesPerBlock = currentBlockSize_;

    mainDoubleBuffer_.setSize (2, samplesPerBlock);
    fadeOutBuffer_.setSize    (2, samplesPerBlock);

    inputMonoScratch_.setSize  (1, samplesPerBlock);
    outputMonoScratch_.setSize (1, samplesPerBlock);
    alignedInputMonoScratch_.setSize (1, samplesPerBlock);

    fadeLength_ = std::max (1, juce::roundToInt (sampleRate * kSwitchFadeSeconds));
    latencyBypass_.prepare(sampleRate, samplesPerBlock, 2,
        latency_bypass::maxEngineLatencySamples(kGateTopologyMaxDepth, samplesPerBlock));
    displayWasActive_ = false;
    prepared_ = true;

    // The audio thread is not running here, so the new engine is installed directly.
    releaseEnginesLocked();
    installEngineForPrepareLocked();
    const int latency = committedLatency_;
    lock.unlock();
    notifyCommittedState(latency);
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
    alignedInputMonoScratch_.setSize (0, 0);    latencyBypass_.release();
    committedTailLength_.store(0, 0.0);
    engineAvailable_.store(false, std::memory_order_release);
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
#if DTCWPT_ENABLE_TEST_SEAMS
    if (engineBuildFailuresRemainingForTest_ > 0)
    {
        --engineBuildFailuresRemainingForTest_;
        bundle->engine->setPrepareCandidateFailpointForTesting(engineBuildFailpointForTest_);
    }
#endif

    auto gateProcessor = std::make_unique<GateBandProcessor>();
    bundle->gate = gateProcessor.get();
    bundle->engine->setBandProcessor (std::move (gateProcessor));
    bundle->engine->prepareToPlay (currentSampleRate_, currentBlockSize_, config, 2);

    bundle->frequencyRanges = bundle->engine->getDestinationFrequencyRanges();
    bundle->latency = bundle->engine->getLatency();
    bundle->tailSamples = bundle->engine->getTailLengthSamples();
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
        notifyCommittedState(latency);
    return accepted;
}

std::vector<std::string> GateAudioProcessor::getCommittedTopology() const
{
    std::lock_guard<std::mutex> lock (controlMutex_);
    return committedDestinations_;
}

bool GateAudioProcessor::commitTopologyLocked(const std::vector<std::string>& candidate)
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
        topology::emitInvalidTopologyDiagnostic("Gate");
    }
    catch (...) {}
    return false;
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
        active_->fadeInOnStart = true;
        fadePosition_ = 0;
    }

    latencyBypass_.setDelaySamples(active_->latency);
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

void GateAudioProcessor::notifyCommittedState(int latency)
{
    if (getLatencySamples() != latency)
        setLatencySamples(latency);
    else
        updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
}

void GateAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { processBlockInternal(buffer, false); }
void GateAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { processBlockInternal(buffer, true); }
juce::AudioProcessorParameter* GateAudioProcessor::getBypassParameter() const { return apvts_.getParameter("bypass"); }

void GateAudioProcessor::processBlockInternal(juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    if (buffer.getNumSamples() <= 0) return;
    (void) acquirePendingEngine(false);
    if (active_ == nullptr || buffer.getNumChannels() < 1)
    {
        buffer.clear();
        return;
    }
    std::array<double, gate_threshold::kNumPoints> points {};
    for (std::size_t i = 0; i < points.size(); ++i)
        points[i] = thresholdRaw_[i]->load();
    auto value = [this](const char* id) { return static_cast<double>(apvts_.getRawParameterValue(id)->load()); };
    GateBandProcessor::DynamicsParams dynamics;
    dynamics.attackMs = value("attack"); dynamics.holdMs = value("hold"); dynamics.releaseMs = value("release");
    dynamics.hysteresisDb = value("hysteresis");
    dynamics.rangeDb = value("range") <= -100.0 ? -std::numeric_limits<double>::infinity() : value("range");
    dynamics.stereoLink = value("stereo_link") >= 0.5;
    lastDynamicsForTest_ = dynamics;
    lastBypassLowestForTest_ = bypassLowestRaw_->load() >= 0.5f;
    for (auto* bundle : { active_, fadingOut_ })
        if (bundle != nullptr)
        {
            std::array<double, GateBandProcessor::kMaxBands> thresholds {};
            for (int band = 0; band < bundle->numBands; ++band)
            {
                const auto range = bundle->frequencyRanges[static_cast<std::size_t>(band)];
                thresholds[static_cast<std::size_t>(band)] = gate_threshold::dbToLinear(gate_threshold::bandThresholdDb(
                    points.data(), range.lo * currentSampleRate_ * 0.5, range.hi * currentSampleRate_ * 0.5));
            }
            bundle->gate->setThresholds(thresholds.data(), bundle->numBands);
            bundle->gate->setDynamics(dynamics);
            bundle->gate->setBypassLowest(lastBypassLowestForTest_);
            if (bundle == active_) lastThresholdsForTest_ = thresholds;
        }

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
    if (display)
    {
        float levels[GateBandProcessor::kMaxBands];
        const int count = active_->gate->getBandLevelsDb(levels);
        bandLevelBridge_.update(levels, count);
    }
}

void GateAudioProcessor::processChunk(juce::AudioBuffer<float>& buffer, int start, int count)
{
    juce::AudioBuffer<double> main(mainDoubleBuffer_.getArrayOfWritePointers(), 2, count);
    juce::AudioBuffer<double> old(fadeOutBuffer_.getArrayOfWritePointers(), 2, count);

    for (int ch = 0; ch < 2; ++ch)
    {
        const float* input = buffer.getReadPointer(std::min(ch, buffer.getNumChannels() - 1)) + start;
        for (int i = 0; i < count; ++i)
        {
            main.getWritePointer(ch)[i] = input[i];
            if (fadingOut_ != nullptr) old.getWritePointer(ch)[i] = input[i];
        }

    }
    if (displayWasActive_)
        for (int i = 0; i < count; ++i)
            inputMonoScratch_.getWritePointer(0)[i] = static_cast<float>((main.getReadPointer(0)[i] + main.getReadPointer(1)[i]) * 0.5);
    latencyBypass_.captureDry(main);
    if (fadingOut_ != nullptr) fadingOut_->engine->processBlock(old);
    active_->engine->processBlock(main);

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

void GateAudioProcessor::reset()
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
    float levels[GateBandProcessor::kMaxBands];
    std::fill(std::begin(levels), std::end(levels), -80.0f);
    bandLevelBridge_.update(levels, active_ != nullptr ? active_->numBands : 0);
    latencyBypass_.reset();
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
    return committedTailLength_.getSeconds();
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

    state.setProperty(statever::kStateVersionPropertyKey, statever::kCurrentStateVersion, nullptr);
    juce::MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void GateAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree state = statever::readVersionedState(data, sizeInBytes, "GateParams");
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
        notifyCommittedState(latency);
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

juce::String GateAudioProcessor::getThresholdParameterId (int pointIndex)
{
    return juce::String ("threshold_") + gate_threshold::kPoints[static_cast<std::size_t> (pointIndex)].idSuffix;
}

//==============================================================================
// Parameter layout factory
//==============================================================================

namespace {
const char* const kAttackId = "attack";
const char* const kHoldId = "hold";
const char* const kReleaseId = "release";
const char* const kHysteresisId = "hysteresis";
const char* const kRangeId = "range";
const char* const kStereoLinkId = "stereo_link";
constexpr float kRangeMinusInfinityDb = -100.0f;
}
namespace
{
    /** Adds a millisecond / dB style float parameter whose text is parsed by its numeric part only. */
    void addUnitParameter (juce::AudioProcessorValueTreeState::ParameterLayout& layout,
                           const char* id, const juce::String& name,
                           juce::NormalisableRange<float> range, float defaultValue,
                           const juce::String& unit, int decimals)
    {
        auto attributes = juce::AudioParameterFloatAttributes()
                              .withStringFromValueFunction ([unit, decimals] (float value, int)
                              {
                                  return juce::String (value, decimals) + " " + unit;
                              })
                              .withValueFromStringFunction ([] (const juce::String& text)
                              {
                                  return text.getFloatValue();
                              });

        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue, attributes));
    }

    void addDynamicsParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
    {
        // Typical noise-gate values: fast attack, short hold, release of a few 100 ms, a few dB of hysteresis.
        addUnitParameter (layout, kAttackId, "Attack",
                          juce::NormalisableRange<float> (0.1f, 100.0f, 0.01f, 0.4f), 1.0f, "ms", 2);
        addUnitParameter (layout, kHoldId, "Hold",
                          juce::NormalisableRange<float> (0.0f, 1000.0f, 0.1f, 0.4f), 50.0f, "ms", 1);
        addUnitParameter (layout, kReleaseId, "Release",
                          juce::NormalisableRange<float> (1.0f, 2000.0f, 0.1f, 0.4f), 100.0f, "ms", 1);
        addUnitParameter (layout, kHysteresisId, "Hysteresis",
                          juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 3.0f, "dB", 1);

        // Range: closed-state attenuation. The minimum means -inf (full mute).
        auto rangeAttributes = juce::AudioParameterFloatAttributes()
                                   .withStringFromValueFunction ([] (float value, int)
                                   {
                                       return value <= kRangeMinusInfinityDb ? juce::String ("-inf dB")
                                                                             : juce::String (value, 1) + " dB";
                                   })
                                   .withValueFromStringFunction ([] (const juce::String& text)
                                   {
                                       return text.containsIgnoreCase ("inf") ? kRangeMinusInfinityDb
                                                                              : text.getFloatValue();
                                   });
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { kRangeId, 1 }, "Range",
                                                                 juce::NormalisableRange<float> (kRangeMinusInfinityDb, 0.0f, 0.1f),
                                                                 kRangeMinusInfinityDb, rangeAttributes));

        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { kStereoLinkId, 1 }, "Stereo Link", true));
    }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout GateAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int k = 0; k < gate_threshold::kNumPoints; ++k)
    {
        auto attributes = juce::AudioParameterFloatAttributes()
                              .withStringFromValueFunction ([](float value, int)
                              {
                                  return juce::String (value, 1) + " dB";
                              })
                              .withValueFromStringFunction ([](const juce::String& text)
                              {
                                  // Leading number only, so "-30", "-30dB" and "-30.0 dB" all parse.
                                  return text.getFloatValue();
                              });

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{getThresholdParameterId (k), 1},
            juce::String ("Threshold ") + gate_threshold::kPoints[static_cast<std::size_t> (k)].label,
            juce::NormalisableRange<float>(gate_threshold::kMinDb, gate_threshold::kMaxDb, 0.1f),
            gate_threshold::kDefaultDb,
            attributes
        ));
    }

    // Bypass lowest frequency band toggle
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"bypass_lowest_band", 1},
        "Bypass Lowest",
        false
    ));

    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"bypass", 1}, "Bypass", false));
    addDynamicsParameters(layout);
    return layout;
}
