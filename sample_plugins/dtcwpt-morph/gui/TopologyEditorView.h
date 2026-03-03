#pragma once

/**
 * @file TopologyEditorView.h
 * @brief Interactive wavelet tree topology editor component.
 *
 * TopologyEditorView is a juce::Component that renders an interactive binary-tree
 * canvas representing the DT-CWPT subband decomposition topology.
 *
 * Features (per SPEC.md US-006):
 *  - Display wavelet tree horizontally (depth left→right, frequency bottom→top).
 *  - Click leaf → split into L/H children (if depth < maxDepth 8).
 *  - Click splitter node → merge descendants (collapse subtree to leaf).
 *  - Hover tooltip: path string, node type, frequency range in Hz.
 *  - Pan (drag) and zoom (scroll wheel/pinch).
 *
 * The view maintains its own internal tree model (std::vector<TreeNode>).
 * On edit, it rebuilds the destination list and calls TopologyState::requestChange().
 */

#include "TopologyState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

/**
 * @brief Interactive wavelet tree canvas component.
 *
 * Constructor args: TopologyState reference + sample rate.
 * The view owns the GUI-side tree model. Topology changes are signalled to the
 * audio thread via TopologyState::requestChange().
 */
class TopologyEditorView : public juce::Component
{
public:
    //==============================================================================
    // Constructor / Destructor
    //==============================================================================

    /**
     * @brief Constructs the view with a topology state reference and sample rate.
     *
     * @param topoState   Reference to the shared topology state (audio ↔ GUI handoff).
     *                    Must outlive this view.
     * @param sampleRate  Current sample rate used to compute frequency ranges per node.
     */
    TopologyEditorView (TopologyState& topoState, double sampleRate);

    /** Destructor. Removes child components before member sub-components are destroyed. */
    ~TopologyEditorView() override;

    //==============================================================================
    // Configuration
    //==============================================================================

    /**
     * @brief Rebuilds the internal tree model from an external destination list.
     *
     * @param destinations  Vector of destination path strings (e.g., {"H","LH","LL"}).
     */
    void setTopology (const std::vector<std::string>& destinations);

    /**
     * @brief Sets the callback invoked when the topology changes (split or merge).
     *
     * Called on the Message Thread immediately after TopologyState::requestChange().
     * Use this to update the APVTS topology property safely on the GUI thread.
     *
     * @param cb  Callback with signature void(const std::vector<std::string>&).
     */
    void setTopologyChangedCallback (std::function<void(const std::vector<std::string>&)> cb);

    /**
     * @brief Sets the callback invoked when the "Back" button is clicked.
     *
     * @param cb  Callback with signature void().
     */
    void setBackButtonCallback (std::function<void()> cb);

    //==============================================================================
    // juce::Component overrides
    //==============================================================================

    /** Draws the tree canvas. */
    void paint (juce::Graphics& g) override;

    /** Lays out the back button. */
    void resized() override;

    /** Handles split/merge clicks and back-button interactions. */
    void mouseDown (const juce::MouseEvent& e) override;

    /** Updates hover tooltip node. */
    void mouseMove (const juce::MouseEvent& e) override;

    /** Zooms the tree canvas. */
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;

    /** Pans the tree canvas. */
    void mouseDrag (const juce::MouseEvent& e) override;

private:
    //==============================================================================
    // Internal tree node
    //==============================================================================

    /**
     * @brief Internal binary-tree node for GUI representation.
     *
     * Node IDs use 1-based binary tree indexing (root = 1, left = 2n, right = 2n+1).
     */
    struct TreeNode
    {
        int                     nodeId    { 1 };   ///< 1-based binary tree index
        std::string             path;              ///< e.g., "LLH"
        bool                    isLeaf    { true };
        float                   freqLow   { 0.0f };  ///< Hz
        float                   freqHigh  { 0.0f };  ///< Hz
        float                   slot      { 0.0f };  ///< Vertical slot index (set by assignSlots())
        std::vector<int>        children;            ///< nodeIds of children (empty if leaf)
        juce::Rectangle<float>  bounds;              ///< Screen rect (set by layoutTree())
    };

    //==============================================================================
    // Tree model helpers
    //==============================================================================

    /**
     * @brief Builds the internal tree from a destination list.
     *
     * @param destinations  List of leaf path strings (e.g., {"H","LH","LL"}).
     */
    void buildTree (const std::vector<std::string>& destinations);

    /** Recomputes node screen bounds using current pan/zoom state. */
    void layoutTree();

    /**
     * @brief Recursive bottom-up pass: assigns sequential vertical slot indices
     *        to all nodes. Leaves get unique integer slots; internal nodes get
     *        the average slot of their children.
     *
     * @param nodeId    The node to process.
     * @param nextSlot  The next available leaf slot index.
     * @return          The updated next available slot index after this subtree.
     */
    int assignSlots (int nodeId, int nextSlot);

    /**
     * @brief Splits a leaf node into L/H children (if depth < kMaxDepth).
     *
     * @param nodeId  ID of the leaf to split.
     */
    void splitNode (int nodeId);

    /**
     * @brief Merges all descendants of a splitter node, making it a leaf.
     *
     * @param nodeId  ID of the splitter to merge.
     */
    void mergeNode (int nodeId);

    /**
     * @brief Returns the node ID at the given screen point, or -1 if none.
     *
     * @param point  Screen-space point.
     * @return nodeId if hit, -1 otherwise.
     */
    int nodeAtPoint (juce::Point<float> point) const noexcept;

    /**
     * @brief Collects the leaf destination path strings from the current tree.
     *
     * @return Vector of destination path strings in frequency order.
     */
    std::vector<std::string> collectDestinations() const;

    /**
     * @brief Recursive helper to collect leaves in DFS order.
     *
     * @param nodeId      Starting node ID.
     * @param destinations  Output vector.
     */
    void collectLeaves (int nodeId, std::vector<std::string>& destinations) const;

    /** Finds a TreeNode by nodeId. Returns nullptr if not found. */
    TreeNode* findNode (int nodeId) noexcept;
    const TreeNode* findNode (int nodeId) const noexcept;

    /** Returns the depth of nodeId (root = 0). */
    static int nodeDepth (int nodeId) noexcept;

    /** Draws a single tree node. */
    void drawNode (juce::Graphics& g, const TreeNode& node) const;

    /** Draws the edge between a parent and child node. */
    void drawEdge (juce::Graphics& g, const TreeNode& parent, int childId) const;

    /** Draws the hover tooltip. */
    void drawTooltip (juce::Graphics& g, const TreeNode& node) const;

    //==============================================================================
    // Preset topology generators
    //==============================================================================

    /**
     * @brief Generates a DWT (Discrete Wavelet Transform) topology of the given depth.
     *
     * A DWT of depth N produces N+1 leaves: "H", "LH", "LLH", ..., "L...LH", "L...L".
     * The low-frequency band is recursively split at each level.
     *
     * @param depth  Number of decomposition levels (1–8).
     * @return       Vector of destination path strings in frequency order.
     */
    static std::vector<std::string> generateDWT (int depth);

    /**
     * @brief Generates a Full Tree topology of the given depth.
     *
     * A Full Tree of depth N produces 2^N leaves by recursively splitting
     * every node at each level. Leaves are enumerated in DFS order (H before L),
     * so the highest-frequency leaf receives slot 0 (top of canvas), matching
     * DWT display convention.
     *
     * @param depth  Number of decomposition levels (1–8).
     * @return       Vector of destination path strings in DFS order.
     */
    static std::vector<std::string> generateFullTree (int depth);

    //==============================================================================
    // Constants
    //==============================================================================

    /** Maximum allowed decomposition depth. */
    static constexpr int kMaxDepth = 8;

    /** Node bounding box size (before zoom). */
    static constexpr float kNodeWidth  = 24.0f;
    static constexpr float kNodeHeight = 24.0f;

    /** Horizontal spacing between depth levels. */
    static constexpr float kDepthSpacing = 35.0f;

    /** Vertical height per leaf slot (kNodeHeight + padding). */
    static constexpr float kSlotHeight = kNodeHeight + 3.0f;

    //==============================================================================
    // Members
    //==============================================================================

    TopologyState& topoState_;
    double         sampleRate_   { 44100.0 };

    std::vector<TreeNode> nodes_;    ///< All tree nodes (valid state after buildTree())

    /** Pan offset (applied before zoom). */
    juce::Point<float> panOffset_ { 20.0f, 20.0f };

    /** Zoom scale factor (1.0 = default). */
    float zoomScale_ { 1.0f };

    /** nodeId of the hovered node for tooltip, or -1. */
    int tooltipNodeId_ { -1 };

    /** Previous mouse position for drag delta computation. */
    juce::Point<float> lastDragPos_;

    /** Back button */
    juce::TextButton backButton_ { "< Back" };

    /** Preset type selector: "DWT" or "Full". */
    juce::ComboBox typeCombo_;

    /** Preset depth selector: 1–8. */
    juce::ComboBox depthCombo_;

    /** Callback for back-button click. */
    std::function<void()> onBack_;

    /**
     * Callback fired on the Message Thread after every topology change.
     * Receives the new destination list so the caller can persist it to APVTS.
     */
    std::function<void(const std::vector<std::string>&)> onTopologyChanged_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopologyEditorView)
};
