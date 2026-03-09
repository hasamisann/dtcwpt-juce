#include "GraphicEqWidget.h"
#include "FontHelper.h"
#include "../../common/GraphicEqDragPaint.h"

#include <cmath>

//==============================================================================
// Colors
//==============================================================================

namespace
{
    const juce::Colour kBgColor     = juce::Colour (0xFF202026);
    const juce::Colour kAccentColor = juce::Colour (0xFF6CB1FF);
    const juce::Colour kTextColor   = juce::Colour (0xFFF0F0F0);
    const juce::Colour kTextDim     = juce::Colour (0xFFB7BCC1);
}

//==============================================================================
// Constructor / Destructor
//==============================================================================

GraphicEqWidget::GraphicEqWidget (GateAudioProcessor& processor, float sampleRate)
    : processor_ (processor)
{
    const auto sanitized = gate_gui::sanitizeSampleRateAndNyquist (sampleRate);
    sampleRate_ = sanitized.sampleRate;
    nyquist_ = sanitized.nyquist;

    activeBandGestures_.fill (false);

    // Fetch initial destinations from topology state
    // We get a fresh copy each timer tick so we don't query it per-paint
    processor_.getTopologyState().tryConsume (currentDestinations_);
    if (currentDestinations_.empty())
    {
        // default fallback if state hasn't tickled us yet
        currentDestinations_ = { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };
    }

    // Cache parameters
    for (int i = 0; i < GateBandProcessor::kMaxBands; ++i)
    {
        juce::String paramID = juce::String::formatted("threshold_%02d", i);
        thresholdRaw_[i]     = processor_.getAPVTS().getRawParameterValue (paramID);
        thresholdParams_[i]  = processor_.getAPVTS().getParameter (paramID);
    }

    bypassLowestRaw_ = processor_.getAPVTS().getRawParameterValue("bypass_lowest_band");

    // Fire repaint at 30 Hz for live meters
    startTimerHz (30);
}

GraphicEqWidget::~GraphicEqWidget()
{
}

//==============================================================================
// Coordinate Mappings
//==============================================================================

std::pair<float, float> GraphicEqWidget::getBandFreqRange (const std::string& path) const
{
    return gate_gui::getBandFrequencyRange (path, nyquist_);
}

float GraphicEqWidget::freqToX (float freq) const
{
    const auto mapping = gate_gui::makeFrequencyMapping (nyquist_);
    const float clampedFreq = juce::jlimit (gate_gui::kFreqMinDisplay, mapping.displayNyquist, freq);
    const float logRange = mapping.logMax - mapping.logMin;

    const float t = (std::log10 (clampedFreq) - mapping.logMin) / logRange;
    return static_cast<float> (getWidth()) * t;
}

float GraphicEqWidget::xToFreq (float x) const
{
    if (getWidth() <= 0)
        return gate_gui::kFreqMinDisplay;

    const auto mapping = gate_gui::makeFrequencyMapping (nyquist_);
    const float t = x / static_cast<float> (getWidth());
    const float logRange = mapping.logMax - mapping.logMin;

    const float logFreq = mapping.logMin + t * logRange;
    return std::pow (10.0f, logFreq);
}

float GraphicEqWidget::dbToY (float db) const
{
    const float clampedDb = juce::jlimit (gate_gui::kDbMinDisplay, gate_gui::kDbMaxDisplay, db);
    const float dbRange   = gate_gui::kDbMaxDisplay - gate_gui::kDbMinDisplay;

    // t is 0.0 at bottom (-120dB) and 1.0 at top (0dB)
    const float t = (clampedDb - gate_gui::kDbMinDisplay) / dbRange;
    
    // Y runs top-down
    return static_cast<float> (getHeight()) * (1.0f - t);
}

float GraphicEqWidget::yToDb (float y) const
{
    if (getHeight() <= 0)
        return gate_gui::kDbMinDisplay;

    float t = 1.0f - (y / static_cast<float> (getHeight()));
    float dbRange = gate_gui::kDbMaxDisplay - gate_gui::kDbMinDisplay;
    
    float db = gate_gui::kDbMinDisplay + t * dbRange;
    return juce::jlimit (gate_gui::kDbMinDisplay, gate_gui::kDbMaxDisplay, db);
}

int GraphicEqWidget::findBandAtX (float x) const
{
    for (size_t i = 0; i < currentDestinations_.size() && i < GateBandProcessor::kMaxBands; ++i)
    {
        auto [freqMin, freqMax] = getBandFreqRange (currentDestinations_[i]);
        
        float xLeft  = freqToX (std::max (gate_gui::kFreqMinDisplay, freqMin));
        float xRight = freqToX (std::max (gate_gui::kFreqMinDisplay, freqMax));
        
        if (x >= xLeft && x <= xRight)
            return static_cast<int>(i);
    }
    return -1;
}

float GraphicEqWidget::getBandCenterX (int bandIdx) const
{
    if (bandIdx < 0 || static_cast<std::size_t> (bandIdx) >= currentDestinations_.size())
        return -1.0f;

    auto [freqMin, freqMax] = getBandFreqRange (currentDestinations_[static_cast<std::size_t> (bandIdx)]);
    const float xLeft  = freqToX (std::max (gate_gui::kFreqMinDisplay, freqMin));
    const float xRight = freqToX (std::max (gate_gui::kFreqMinDisplay, freqMax));
    return 0.5f * (xLeft + xRight);
}

void GraphicEqWidget::beginGestureForBand (int bandIdx)
{
    if (bandIdx < 0 || bandIdx >= GateBandProcessor::kMaxBands)
        return;

    if (activeBandGestures_[static_cast<std::size_t> (bandIdx)])
        return;

    auto* param = thresholdParams_[static_cast<std::size_t> (bandIdx)];
    if (param == nullptr)
        return;

    param->beginChangeGesture();
    activeBandGestures_[static_cast<std::size_t> (bandIdx)] = true;
}

void GraphicEqWidget::endAllActiveGestures()
{
    for (int bandIdx = 0; bandIdx < GateBandProcessor::kMaxBands; ++bandIdx)
    {
        if (!activeBandGestures_[static_cast<std::size_t> (bandIdx)])
            continue;

        if (auto* param = thresholdParams_[static_cast<std::size_t> (bandIdx)]; param != nullptr)
            param->endChangeGesture();

        activeBandGestures_[static_cast<std::size_t> (bandIdx)] = false;
    }
}

void GraphicEqWidget::updateThresholdForBandFromY (int bandIdx, float y)
{
    if (bandIdx < 0 || bandIdx >= GateBandProcessor::kMaxBands)
        return;

    auto* param = thresholdParams_[static_cast<std::size_t> (bandIdx)];
    auto* raw = thresholdRaw_[static_cast<std::size_t> (bandIdx)];
    if (param == nullptr || raw == nullptr)
        return;

    const float targetDb = yToDb (y);
    const float currentDb = raw->load();

    if (std::abs (targetDb - currentDb) <= 0.2f)
        return;

    param->setValueNotifyingHost (param->convertTo0to1 (targetDb));
}

void GraphicEqWidget::applyDragSegment (juce::Point<float> from, juce::Point<float> to)
{
    if (getWidth() <= 0)
        return;

    const auto clampX = [this] (float x)
    {
        return juce::jlimit (0.0f, static_cast<float> (getWidth() - 1), x);
    };

    from.x = clampX (from.x);
    to.x = clampX (to.x);

    const int startBandIdx = findBandAtX (from.x);
    const int endBandIdx = findBandAtX (to.x);
    const int numBands = static_cast<int> (std::min (currentDestinations_.size(),
                                                      static_cast<std::size_t> (GateBandProcessor::kMaxBands)));

    const auto crossedBands = graphic_eq::enumerateCrossedBands (startBandIdx, endBandIdx, numBands);
    if (!crossedBands.empty())
    {
        for (int bandIdx : crossedBands)
        {
            beginGestureForBand (bandIdx);

            const float xCenter = getBandCenterX (bandIdx);
            if (xCenter < 0.0f)
                continue;

            const float y = graphic_eq::interpolateYForX (from.x, from.y, to.x, to.y, xCenter);
            updateThresholdForBandFromY (bandIdx, y);
        }
        return;
    }

    const int fallbackBand = (endBandIdx >= 0) ? endBandIdx : startBandIdx;
    if (fallbackBand >= 0)
    {
        beginGestureForBand (fallbackBand);
        updateThresholdForBandFromY (fallbackBand, to.y);
    }
}

//==============================================================================
// juce::Component interface
//==============================================================================

void GraphicEqWidget::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    float width = bounds.getWidth();
    float height = bounds.getHeight();

    // 1. Fill Background
    g.fillAll (kBgColor);

    // 2. Draw Horizontal Grid (dB lines)
    // Horizontal grid lines (dB)
    g.setColour (kTextDim.withAlpha (0.4f));
    g.setFont (FontHelper::getFont (12.0f));

    const std::vector<float> gridDbs = { 10.0f, 0.0f, -20.0f, -40.0f, -60.0f, -80.0f, -100.0f, -120.0f };
    for (float db : gridDbs)
    {
        float y = dbToY (db);
        g.drawLine (0.0f, y, static_cast<float>(getWidth()), y, 1.0f);
        g.drawText (juce::String (static_cast<int>(db)) + " dB",
                    4, static_cast<int>(y) - 16, 40, 16,
                    juce::Justification::bottomLeft, false);
    }

    // 3. Draw Vertical Grid (Freq lines)
    g.setColour (kTextDim.withMultipliedAlpha (0.3f));
    const float freqLines[] = { 100.0f, 1000.0f, 10000.0f };
    for (float f : freqLines)
    {
        float x = freqToX (f);
        g.drawLine (x, 0.0f, x, height, 1.0f);
        
        juce::String label = (f >= 1000.0f) ? juce::String (static_cast<int>(f) / 1000) + "k"
                                            : juce::String (static_cast<int>(f));
        g.drawText (label, static_cast<int>(x - 15.0f), static_cast<int>(height - 14.0f), 30, 12,
                    juce::Justification::centredBottom, false);
    }

    // 4. Main Axis Labels
    g.setColour (kTextColor);
    g.drawText ("0 dB",     4, 2, 40, 12, juce::Justification::topLeft, false);
    g.drawText ("-120 dB",  4, static_cast<int>(height - 14.0f), 45, 12, juce::Justification::bottomLeft, false);
    g.drawText ("20 Hz",    4, static_cast<int>(height - 28.0f), 40, 12, juce::Justification::bottomLeft, false);
    g.drawText ("20kHz",    static_cast<int>(width - 46.0f), static_cast<int>(height - 28.0f), 40, 12, juce::Justification::bottomRight, false);

    // Grab current levels
    std::array<float, GateBandProcessor::kMaxBands> levelsDb;
    levelsDb.fill (gate_gui::kDbMinDisplay);
    processor_.getBandLevelBridge().read (levelsDb.data());

    // Determine hover state
    auto mousePos = getMouseXYRelative().toFloat();
    bool isMouseOver = isMouseButtonDown() || isMouseOverOrDragging();

    juce::String tooltipText;
    juce::Point<float> tooltipPos;

    // 5. Draw Bands
    for (size_t i = 0; i < currentDestinations_.size() && i < GateBandProcessor::kMaxBands; ++i)
    {
        auto [freqMin, freqMax] = getBandFreqRange (currentDestinations_[i]);
        
        float xLeft  = freqToX (std::max (gate_gui::kFreqMinDisplay, freqMin));
        float xRight = freqToX (std::max (gate_gui::kFreqMinDisplay, freqMax));
        
        float currentDb = thresholdRaw_[i] ? thresholdRaw_[i]->load() : -60.0f;
        float yTop      = dbToY (currentDb);
        
        bool isHovered = isMouseOver && (mousePos.x >= xLeft && mousePos.x <= xRight);
        
        float padding = (xRight - xLeft > 3.0f) ? 1.0f : 0.0f;
        float barWidth = std::max(1.0f, (xRight - xLeft) - (padding * 2.0f));
        
        bool isLowestBand = true;
        for (char c : currentDestinations_[i]) {
            if (c != 'L') {
                isLowestBand = false;
                break;
            }
        }
        
        bool isBypassed = isLowestBand && (bypassLowestRaw_ != nullptr && bypassLowestRaw_->load() >= 0.5f);
        
        juce::Colour baseColor = isBypassed ? juce::Colours::darkgrey : kAccentColor;
        juce::Colour fillColor = isHovered ? baseColor.withMultipliedAlpha (isBypassed ? 0.40f : 0.55f)
                                           : baseColor.withMultipliedAlpha (isBypassed ? 0.25f : 0.40f);
        
        // Bar rectangle from threshold to bottom edge
        juce::Rectangle<float> barRect (xLeft + padding, yTop, barWidth, height - yTop);
        
        g.setColour (fillColor);
        g.fillRect (barRect);
        
        // Threshold Line (accent color top line)
        float xEnd = std::max(xLeft + padding, xRight - padding);
        g.setColour (baseColor);
        g.drawLine (xLeft + padding, yTop, xEnd, yTop, 2.0f);
        
        // Level Meter (white overlay line)
        float dbLevel = levelsDb[i];
        if (dbLevel > gate_gui::kDbMinDisplay + 0.1f) // only draw if actively populated
        {
            float yLevel = dbToY (dbLevel);
            float padLevel = (xRight - xLeft > 4.0f) ? 2.0f : padding;
            float xEndLevel = std::max(xLeft + padLevel, xRight - padLevel);
            g.setColour (juce::Colours::white);
            g.drawLine (xLeft + padLevel, yLevel, xEndLevel, yLevel, 2.0f);
        }
        
        // Capture tooltip properties if hovering and not actively dragging
        if (isHovered && !isDragging_)
        {
            tooltipText = "Band: " + juce::String (currentDestinations_[i]) + "\n"
                          + juce::String (freqMin, 0) + " Hz - " + juce::String (freqMax, 0) + " Hz\n"
                          + "Threshold: " + juce::String (currentDb, 1) + " dB";
            tooltipPos  = juce::Point<float> ((xLeft + xRight) * 0.5f, yTop - 4.0f);
        }
    }
    
    // 6. Draw Tooltip text on top
    if (tooltipText.isNotEmpty())
    {
        g.setFont (FontHelper::getFont (12.0f));
        g.setColour (kTextColor);
        
        // simple multi-line draw
        juce::StringArray lines = juce::StringArray::fromLines (tooltipText);
        float th = 14.0f; // line height
        float totalH = lines.size() * th;
        
        float txtY = tooltipPos.y - totalH;
        for (const auto& line : lines)
        {
            g.drawText (line, static_cast<int>(tooltipPos.x - 100.0f), static_cast<int>(txtY), 200, static_cast<int>(th),
                        juce::Justification::centredBottom, false);
            txtY += th;
        }
    }
}

void GraphicEqWidget::resized()
{
}

//==============================================================================
// Mouse Interactions
//==============================================================================

void GraphicEqWidget::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return; // handle context menu on mouseUp instead

    isDragging_ = true;
    lastMousePos_ = e.position;

    const int bandIdx = findBandAtX (e.position.x);
    if (bandIdx >= 0)
    {
        beginGestureForBand (bandIdx);
        updateThresholdForBandFromY (bandIdx, e.position.y);
    }
}

void GraphicEqWidget::mouseDrag (const juce::MouseEvent& e)
{
    if (isDragging_)
    {
        applyDragSegment (lastMousePos_, e.position);
        lastMousePos_ = e.position;
    }
}

void GraphicEqWidget::mouseUp (const juce::MouseEvent& e)
{
    if (isDragging_)
    {
        endAllActiveGestures();
        isDragging_ = false;
        return; // Was a paint operation, so exit.
    }

    if (e.mods.isPopupMenu())
    {
        int bandIdx = findBandAtX (e.position.x);
        if (bandIdx >= 0 && static_cast<size_t>(bandIdx) < currentDestinations_.size())
        {
            juce::PopupMenu m;
            
            auto [freqMin, freqMax] = getBandFreqRange (currentDestinations_[bandIdx]);
            
            m.addSectionHeader (juce::String::formatted ("Band: %s (%.0f-%.0f Hz)", 
                                                          currentDestinations_[bandIdx].c_str(), 
                                                          freqMin, freqMax));
            m.addSeparator();
            
            m.addItem (1, "Reset to -60 dB");
            m.addItem (2, "Open gate (-120 dB)");
            m.addItem (3, "Close gate (0 dB)");

            m.showMenuAsync (juce::PopupMenu::Options(), [this, bandIdx](int result)
            {
                if (result > 0 && thresholdParams_[bandIdx] != nullptr)
                {
                    float targetDb = 0.0f;
                    if (result == 1) targetDb = -60.0f;
                    else if (result == 2) targetDb = -120.0f;
                    else if (result == 3) targetDb = 0.0f;

                    auto* param = thresholdParams_[bandIdx];
                    param->beginChangeGesture();
                    param->setValueNotifyingHost (param->convertTo0to1 (targetDb));
                    param->endChangeGesture();
                }
            });
        }
    }
}

void GraphicEqWidget::mouseMove (const juce::MouseEvent&)
{
    // Necessary to redraw if we hover over a different band
    // (the paint method changes the fill color and displays a tooltip)
    repaint();
}

void GraphicEqWidget::mouseExit (const juce::MouseEvent&)
{
    repaint();
}

//==============================================================================
// juce::Timer interface
//==============================================================================

void GraphicEqWidget::timerCallback()
{
    // Also poll topology state in case we need to add/remove bands visually
    std::vector<std::string> pendingDestinations;
    
    // We can't actually consume the main topo state here safely since the audio thread
    // is ALSO trying to consume it. BUT the topo change comes FROM the GUI and should 
    // be persistent globally. Ideally we ask GateAudioProcessor for the current known destinations.
    // Hack around to avoid reading from topo state directly, instead we rely on audio processor
    // Wait... this GUI widget might occasionally miss band destinations if they change dynamically.
    // In our specific case, GateAudioProcessor doesn't expose it. We could read from TopoState directly, 
    // but the AudioWorker might steal it first.
    // Let's implement TopoState peek or just read from ValueTree.
    
    // Actually, reading from TopoState `tryConsume` is flawed if multiple consumers exist.
    // A better approach is to read the ValueTree topology property, but we can't do that easily without a lock.
    // For now, let's just use the current APVTS ValueTree property `topology`.
    
    juce::var prop = processor_.getAPVTS().state.getProperty ("topology");
    if (prop.isString())
    {
        std::vector<std::string> newDests;
        if (prop.toString().isEmpty())
        {
            newDests.push_back ("");
        }
        else
        {
            auto tokens = juce::StringArray::fromTokens (prop.toString(), ",", "");
            for (const auto& t : tokens)
                newDests.push_back (t.toStdString());
        }
        
        if (newDests != currentDestinations_)
        {
            currentDestinations_ = newDests;
        }
    }
    
    repaint();
}
