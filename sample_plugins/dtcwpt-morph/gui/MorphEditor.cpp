/**
 * @file MorphEditor.cpp
 * @brief Implementation of the root AudioProcessorEditor for the DT-CWPT Morph plugin.
 */

#include "MorphEditor.h"

//==============================================================================
// Constructor
//==============================================================================

MorphEditor::MorphEditor (MorphAudioProcessor& processor)
    : AudioProcessorEditor (processor)
    , mainView_     (processor)
    , topologyView_ ([this, &processor] (const std::vector<std::string>& dests)
                     {
                         // Builds the engine here (message thread) and hands it to the audio thread.
                         if (! processor.requestTopologyChange (dests))
                             topologyView_.setTopology (processor.getCommittedTopology());
                     },
                     44100.0)
    , aboutView_    ([this] { showMain(); })
{
    // -----------------------------------------------------------------------
    // Wire MainView navigation callbacks
    // -----------------------------------------------------------------------
    mainView_.setSettingsCallback ([this, &processor]
    {
        topologyView_.setTopology (processor.getCommittedTopology());
        showTopology();
    });
    mainView_.setAboutCallback    ([this] { showAbout(); });

    // -----------------------------------------------------------------------
    // Wire TopologyEditorView back callback
    // -----------------------------------------------------------------------
    topologyView_.setBackButtonCallback ([this] { showMain(); });

    topologyView_.setTopology (processor.getCommittedTopology());

    // -----------------------------------------------------------------------
    // Add sub-views as children
    // -----------------------------------------------------------------------
    addChildComponent (mainView_);
    addChildComponent (topologyView_);
    addChildComponent (aboutView_);

    // -----------------------------------------------------------------------
    // Show MainView initially; hide the others
    // -----------------------------------------------------------------------
    showMain();

    // -----------------------------------------------------------------------
    // Resizable editor with fixed 7:10 aspect ratio.
    // Min size: 280×400 (1×), Max size: 560×800 (2×).
    // setResizable(true, true) enables the resize handle for Standalone and
    // allows the host to resize the editor window in VST3/AU.
    // -----------------------------------------------------------------------
    setSize (kEditorWidth, kEditorHeight);
    setResizable (true, true);
    setResizeLimits (kEditorWidth, kEditorHeight, kEditorWidth * 2, kEditorHeight * 2);
    getConstrainer()->setFixedAspectRatio (
        static_cast<double> (kEditorWidth) / static_cast<double> (kEditorHeight));
}

//==============================================================================
// Destructor
//==============================================================================

MorphEditor::~MorphEditor()
{
    // Remove all child components before member sub-views (mainView_, topologyView_,
    // aboutView_) are destroyed by the compiler-generated member destruction sequence.
    // Without this call, Component::~Component() would call removeAllChildren() AFTER
    // the members are already destroyed, causing use-after-free / heap corruption.
    removeAllChildren();
}

//==============================================================================
// resized
//==============================================================================

void MorphEditor::resized()
{
    const juce::Rectangle<int> area = getLocalBounds();

    // The visible sub-view always fills the entire editor area.
    mainView_.setBounds     (area);
    topologyView_.setBounds (area);
    aboutView_.setBounds    (area);
}

//==============================================================================
// View switching
//==============================================================================

void MorphEditor::showMain()
{
    currentView_ = ViewState::Main;
    mainView_.setVisible     (true);
    topologyView_.setVisible (false);
    aboutView_.setVisible    (false);
    resized();
}

void MorphEditor::showTopology()
{
    currentView_ = ViewState::Topology;
    mainView_.setVisible     (false);
    topologyView_.setVisible (true);
    aboutView_.setVisible    (false);
    resized();
}

void MorphEditor::showAbout()
{
    currentView_ = ViewState::About;
    mainView_.setVisible     (false);
    topologyView_.setVisible (false);
    aboutView_.setVisible    (true);
    resized();
}
