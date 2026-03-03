#include "GraphicEqWidget.h"
#include "FontHelper.h"

#include <cmath>

//==============================================================================
// Colors
//==============================================================================

namespace
{
    const juce::Colour kBgColor     = juce::Colour (25, 25, 35);
    const juce::Colour kAccentColor = juce::Colour (115, 170, 230);
    const juce::Colour kTextColor   = juce::Colour (220, 220, 220);
    const juce::Colour kTextDim     = juce::Colour (150, 150, 150);

    const float kFreqMinDisplay = 20.0f;
    const float kDbMinDisplay   = -120.0f;
    const float kDbMaxDisplay   = 10.0f;
}

//==============================================================================
// Constructor / Destructor
//==============================================================================

GraphicEqWidget::GraphicEqWidget (GateAudioProcessor& processor, float sampleRate)
    : processor_ (processor),
      sampleRate_ (sampleRate),
      nyquist_ (sampleRate * 0.5f)
{
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
    float lo = 0.0f;
    float hi = nyquist_;

    for (char ch : path)
    {
        float mid = (lo + hi) * 0.5f;
        if (ch == 'L')
            hi = mid;
        else
            lo = mid;
    }

    return { lo, hi };
}

float GraphicEqWidget::freqToX (float freq) const
{
    float clampedFreq = juce::jlimit (kFreqMinDisplay, nyquist_, freq);
    float logMin   = std::log10 (kFreqMinDisplay);
    float logMax   = std::log10 (nyquist_);
    float logRange = logMax - logMin;

    float t = (std::log10 (clampedFreq) - logMin) / logRange;
    return static_cast<float> (getWidth()) * t;
}

float GraphicEqWidget::xToFreq (float x) const
{
    float t = x / static_cast<float> (getWidth());
    float logMin   = std::log10 (kFreqMinDisplay);
    float logMax   = std::log10 (nyquist_);
    float logRange = logMax - logMin;

    float logFreq = logMin + t * logRange;
    return std::pow (10.0f, logFreq);
}

float GraphicEqWidget::dbToY (float db) const
{
    float clampedDb = juce::jlimit (kDbMinDisplay, kDbMaxDisplay, db);
    float dbRange   = kDbMaxDisplay - kDbMinDisplay;

    // t is 0.0 at bottom (-120dB) and 1.0 at top (0dB)
    float t = (clampedDb - kDbMinDisplay) / dbRange;
    
    // Y runs top-down
    return static_cast<float> (getHeight()) * (1.0f - t);
}

float GraphicEqWidget::yToDb (float y) const
{
    float t = 1.0f - (y / static_cast<float> (getHeight()));
    float dbRange = kDbMaxDisplay - kDbMinDisplay;
    
    float db = kDbMinDisplay + t * dbRange;
    return juce::jlimit (kDbMinDisplay, kDbMaxDisplay, db);
}

int GraphicEqWidget::findBandAtX (float x) const
{
    for (size_t i = 0; i < currentDestinations_.size() && i < GateBandProcessor::kMaxBands; ++i)
    {
        auto [freqMin, freqMax] = getBandFreqRange (currentDestinations_[i]);
        
        float xLeft  = freqToX (std::max(kFreqMinDisplay, freqMin));
        float xRight = freqToX (std::max(kFreqMinDisplay, freqMax));
        
        if (x >= xLeft && x <= xRight)
            return static_cast<int>(i);
    }
    return -1;
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
    g.setFont (FontHelper::getFont (10.0f));

    const std::vector<float> gridDbs = { 10.0f, 0.0f, -20.0f, -40.0f, -60.0f, -80.0f, -100.0f, -120.0f };
    for (float db : gridDbs)
    {
        float y = dbToY (db);
        g.drawLine (0.0f, y, static_cast<float>(getWidth()), y, 1.0f);
        g.drawText (juce::String (static_cast<int>(db)) + " dB",
                    4, static_cast<int>(y) - 14, 40, 14,
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
        g.drawText (label, static_cast<int>(x - 15.0f), static_cast<int>(height - 12.0f), 30, 10,
                    juce::Justification::centredBottom, false);
    }

    // 4. Main Axis Labels
    g.setColour (kTextColor);
    g.drawText ("0 dB",     4, 2, 40, 10, juce::Justification::topLeft, false);
    g.drawText ("-120 dB",  4, static_cast<int>(height - 12.0f), 40, 10, juce::Justification::bottomLeft, false);
    g.drawText ("20 Hz",    4, static_cast<int>(height - 24.0f), 40, 10, juce::Justification::bottomLeft, false);
    g.drawText ("20kHz",    static_cast<int>(width - 42.0f), static_cast<int>(height - 24.0f), 40, 10, juce::Justification::bottomRight, false);

    // Grab current levels
    std::array<float, GateBandProcessor::kMaxBands> levelsDb;
    levelsDb.fill (-120.0f);
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
        
        float xLeft  = freqToX (std::max(kFreqMinDisplay, freqMin));
        float xRight = freqToX (std::max(kFreqMinDisplay, freqMax));
        
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
        if (dbLevel > kDbMinDisplay + 0.1f) // only draw if actively populated
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
        g.setFont (FontHelper::getFont (10.0f));
        g.setColour (kTextColor);
        
        // simple multi-line draw
        juce::StringArray lines = juce::StringArray::fromLines (tooltipText);
        float th = 12.0f; // line height
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
    
    int bandIdx = findBandAtX (e.position.x);
    if (bandIdx >= 0 && thresholdParams_[bandIdx] != nullptr)
    {
        thresholdParams_[bandIdx]->beginChangeGesture();
        updateThresholdFromMouse (e);
    }
}

void GraphicEqWidget::mouseDrag (const juce::MouseEvent& e)
{
    if (isDragging_)
    {
        // For sweeping across multiple bands, we want to start a change gesture
        // for any newly entered band, and update the current one.
        // For simplicity matching the reference, we just update whatever is under the mouse.
        // To be perfectly pure we should track which ones had beginChangeGesture called, 
        // but setValueNotifyingHost works fine generally within a drag loop in JUCE APVTS.
        
        int bandIdx = findBandAtX (e.position.x);
        int lastBandIdx = findBandAtX (lastMousePos_.x);
        
        // Note: The Rust version just overwrites `setter.set_parameter(param, new_db)` indiscriminately 
        // if `(new_db - current_db).abs() > 0.2`.
        if (bandIdx >= 0 && bandIdx != lastBandIdx)
        {
            if (lastBandIdx >= 0 && thresholdParams_[lastBandIdx] != nullptr)
                thresholdParams_[lastBandIdx]->endChangeGesture();
            
            if (thresholdParams_[bandIdx] != nullptr)
                thresholdParams_[bandIdx]->beginChangeGesture();
        }
        
        updateThresholdFromMouse (e);
        lastMousePos_ = e.position;
    }
}

void GraphicEqWidget::mouseUp (const juce::MouseEvent& e)
{
    if (isDragging_)
    {
        int bandIdx = findBandAtX (lastMousePos_.x);
        if (bandIdx >= 0 && thresholdParams_[bandIdx] != nullptr)
        {
            thresholdParams_[bandIdx]->endChangeGesture();
        }
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

void GraphicEqWidget::updateThresholdFromMouse (const juce::MouseEvent& e)
{
    int bandIdx = findBandAtX (e.position.x);
    if (bandIdx >= 0 && thresholdParams_[bandIdx] != nullptr)
    {
        float targetDb = yToDb (e.position.y);
        float currentDb = thresholdRaw_[bandIdx]->load();
        
        if (std::abs(targetDb - currentDb) > 0.2f)
        {
            auto* param = thresholdParams_[bandIdx];
            param->setValueNotifyingHost (param->convertTo0to1 (targetDb));
        }
    }
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
