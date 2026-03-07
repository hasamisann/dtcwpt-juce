#include "GateEditor.h"
#include "GateEditorSampleRate.h"

#include "../../common/TopologyPersistence.h"

//==============================================================================
GateEditor::GateEditor (GateAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor_ (p),
      constructionSampleRate_ (resolveGateEditorConstructionSampleRate (p.getSampleRate())),
      mainView_ (p, constructionSampleRate_),
      topoView_ (p.getTopologyState(), constructionSampleRate_),
      aboutView_ ([this] { showMain(); })
{
    // Hosts may create the editor before prepareToPlay(), so getSampleRate()
    // is not guaranteed to be initialised here.
    // Make sure that before the constructor has finished, you've set the
    // editor's size to whatever you need it to be.
    setSize (500, 420);

    // Add views
    addAndMakeVisible (mainView_);
    addAndMakeVisible (topoView_);
    addAndMakeVisible (aboutView_);

    // Wire up navigations
    mainView_.onSettingsClicked = [this] { showSettings(); };
    mainView_.onAboutClicked = [this] { showAbout(); };

    topoView_.setBackButtonCallback ([this] { showMain(); });
    topoView_.setTopologyChangedCallback ([this](const std::vector<std::string>& dests)
    {
        processor_.getAPVTS().state.setProperty (topology::kTopologyPropertyKey,
                                                 topology::serializeTopologyDestinations (dests),
                                                 nullptr);
    });

    topoView_.setTopology (processor_.getStoredTopologyDestinations());
    // aboutView takes the back callback directly in the constructor

    // Set initial view
    showMain();
}

GateEditor::~GateEditor()
{
}

//==============================================================================
void GateEditor::paint (juce::Graphics& g)
{
    // Background is generally handled by the individual views,
    // but we can provide a fallback
    g.fillAll (juce::Colour (18, 18, 27));
}

void GateEditor::resized()
{
    // All views fill the entire editor area
    auto b = getLocalBounds();
    mainView_.setBounds (b);
    topoView_.setBounds (b);
    aboutView_.setBounds (b);
}

//==============================================================================
void GateEditor::showMain()
{
    mainView_.setVisible (true);
    topoView_.setVisible (false);
    aboutView_.setVisible (false);
}

void GateEditor::showSettings()
{
    topoView_.setTopology (processor_.getStoredTopologyDestinations());
    mainView_.setVisible (false);
    topoView_.setVisible (true);
    aboutView_.setVisible (false);
}

void GateEditor::showAbout()
{
    mainView_.setVisible (false);
    topoView_.setVisible (false);
    aboutView_.setVisible (true);
}
