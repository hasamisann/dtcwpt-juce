/**
 * @file TopologyEditorView.cpp
 * @brief Interactive wavelet tree topology editor implementation.
 */

#include "TopologyEditorView.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// Colours (anonymous namespace)
//==============================================================================

namespace
{
    /** Background colour for the canvas. */
    constexpr juce::uint32 kBgColour       = 0xFF1E1E2E;

    /** Leaf node fill colour. */
    constexpr juce::uint32 kLeafFill       = 0xFF2E4A6E;

    /** Leaf node outline colour. */
    constexpr juce::uint32 kLeafOutline    = 0xFF4A9FFF;

    /** Splitter node fill colour. */
    constexpr juce::uint32 kSplitFill      = 0xFF3A3A3A;

    /** Splitter node outline colour. */
    constexpr juce::uint32 kSplitOutline   = 0xFF888888;

    /** Edge line colour. */
    constexpr juce::uint32 kEdgeColour     = 0xFF555555;

    /** Tooltip background colour. */
    constexpr juce::uint32 kTooltipBg      = 0xE0222222;

    /** Node label text colour. */
    constexpr juce::uint32 kLabelColour    = 0xFFDDDDDD;

    /** Default 6-level DWT topology. */
    const std::vector<std::string> kDefaultTopology =
        { "H", "LH", "LLH", "LLLH", "LLLLH", "LLLLLH", "LLLLLL" };

} // namespace

//==============================================================================
// Constructor
//==============================================================================

TopologyEditorView::TopologyEditorView (TopologyState& topoState, double sampleRate)
    : topoState_   (topoState)
    , sampleRate_  (sampleRate)
{
    buildTree (kDefaultTopology);

    // Back button
    addAndMakeVisible (backButton_);
    backButton_.onClick = [this]
    {
        if (onBack_) onBack_();
    };
}

//==============================================================================
// Configuration
//==============================================================================

void TopologyEditorView::setTopology (const std::vector<std::string>& destinations)
{
    buildTree (destinations);
    layoutTree();
    repaint();
}

void TopologyEditorView::setBackButtonCallback (std::function<void()> cb)
{
    onBack_ = std::move (cb);
}

//==============================================================================
// Component overrides
//==============================================================================

void TopologyEditorView::resized()
{
    backButton_.setBounds (4, 4, 70, 24);
    layoutTree();
}

void TopologyEditorView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kBgColour));

    // Draw edges first (behind nodes)
    for (const auto& node : nodes_)
    {
        if (!node.isLeaf)
        {
            for (int childId : node.children)
                drawEdge (g, node, childId);
        }
    }

    // Draw nodes
    for (const auto& node : nodes_)
        drawNode (g, node);

    // Draw hover tooltip
    if (tooltipNodeId_ >= 0)
    {
        if (const auto* node = findNode (tooltipNodeId_))
            drawTooltip (g, *node);
    }
}

void TopologyEditorView::mouseDown (const juce::MouseEvent& e)
{
    lastDragPos_ = e.position;
    tooltipNodeId_ = -1;

    const int hitId = nodeAtPoint (e.position);
    if (hitId < 0) return;

    auto* node = findNode (hitId);
    if (!node) return;

    if (node->isLeaf)
    {
        // Split if depth allows
        if (nodeDepth (hitId) < kMaxDepth)
        {
            splitNode (hitId);
            const auto dests = collectDestinations();
            topoState_.requestChange (dests);
            layoutTree();
            repaint();
        }
    }
    else
    {
        // Merge all descendants back to a leaf
        mergeNode (hitId);
        const auto dests = collectDestinations();
        topoState_.requestChange (dests);
        layoutTree();
        repaint();
    }
}

void TopologyEditorView::mouseMove (const juce::MouseEvent& e)
{
    const int newHover = nodeAtPoint (e.position);
    if (newHover != tooltipNodeId_)
    {
        tooltipNodeId_ = newHover;
        repaint();
    }
}

void TopologyEditorView::mouseWheelMove (const juce::MouseEvent& /*e*/,
                                          const juce::MouseWheelDetails& wheel)
{
    constexpr float kZoomMin = 0.3f;
    constexpr float kZoomMax = 4.0f;
    constexpr float kZoomStep = 0.1f;

    zoomScale_ = std::clamp (zoomScale_ + wheel.deltaY * kZoomStep,
                              kZoomMin, kZoomMax);
    layoutTree();
    repaint();
}

void TopologyEditorView::mouseDrag (const juce::MouseEvent& e)
{
    const juce::Point<float> delta = e.position - lastDragPos_;
    panOffset_ += delta;
    lastDragPos_ = e.position;
    layoutTree();
    repaint();
}

//==============================================================================
// Tree model helpers
//==============================================================================

void TopologyEditorView::buildTree (const std::vector<std::string>& destinations)
{
    nodes_.clear();

    if (destinations.empty()) return;

    // Root node (node ID = 1)
    TreeNode root;
    root.nodeId   = 1;
    root.path     = "";
    root.isLeaf   = false;
    root.freqLow  = 0.0f;
    root.freqHigh = static_cast<float> (sampleRate_ * 0.5);
    nodes_.push_back (root);

    // For each destination, walk down the tree and create nodes as needed
    for (const auto& dest : destinations)
    {
        int currentId = 1;

        for (char c : dest)
        {
            // Determine child ID: 'L' → left (2n), 'H' → right (2n+1)
            const int childId = (c == 'L') ? (2 * currentId) : (2 * currentId + 1);
            const bool isLastChar = (&c == &dest.back());

            // Find or create child node
            auto* childNode = findNode (childId);
            if (childNode == nullptr)
            {
                // Get parent's freq range
                auto* parentNode = findNode (currentId);
                const float parentLow  = parentNode ? parentNode->freqLow  : 0.0f;
                const float parentHigh = parentNode ? parentNode->freqHigh
                                                    : static_cast<float> (sampleRate_ * 0.5);
                const float midFreq = (parentLow + parentHigh) * 0.5f;

                TreeNode newNode;
                newNode.nodeId  = childId;
                // Build path string
                newNode.path    = dest.substr (0, static_cast<std::size_t> (&c - dest.data() + 1));
                newNode.isLeaf  = isLastChar;
                newNode.freqLow  = (c == 'L') ? parentLow  : midFreq;
                newNode.freqHigh = (c == 'L') ? midFreq     : parentHigh;
                nodes_.push_back (newNode);

                // Register as child of parent
                if (parentNode != nullptr)
                    parentNode->children.push_back (childId);
            }
            else if (isLastChar)
            {
                // This node was created as intermediate; mark it as a leaf
                childNode->isLeaf = true;
            }

            // Mark parent as splitter (not leaf)
            if (auto* pn = findNode (currentId))
            {
                if (!pn->children.empty())
                    pn->isLeaf = false;
            }

            currentId = childId;
        }
    }

    // Re-validate leaf flags: a node is a leaf iff it has no children
    for (auto& n : nodes_)
        n.isLeaf = n.children.empty();
}

void TopologyEditorView::layoutTree()
{
    // Assign screen positions based on node depth and frequency position
    // x = depth * kDepthSpacing * zoom + panOffset.x
    // y = frequency-proportional within canvas height * zoom + panOffset.y

    const float canvasHeight = static_cast<float> (getHeight()) - 40.0f; // leave room for button
    const float nyquist = static_cast<float> (sampleRate_ * 0.5);

    for (auto& node : nodes_)
    {
        const int depth = nodeDepth (node.nodeId);
        const float x = panOffset_.x + static_cast<float> (depth) * kDepthSpacing * zoomScale_;

        // y: map frequency centre to canvas (low freq at bottom, high at top)
        const float freqCentre = (node.freqLow + node.freqHigh) * 0.5f;
        const float normFreq   = (nyquist > 0.0f) ? (freqCentre / nyquist) : 0.5f;
        // Flip: high freq → top (low y), low freq → bottom (high y)
        const float y = panOffset_.y + 40.0f +
                        (1.0f - normFreq) * canvasHeight * zoomScale_
                        - kNodeHeight * zoomScale_ * 0.5f;

        node.bounds = juce::Rectangle<float> (x, y,
                                               kNodeWidth  * zoomScale_,
                                               kNodeHeight * zoomScale_);
    }
}

void TopologyEditorView::splitNode (int nodeId)
{
    auto* node = findNode (nodeId);
    if (!node || !node->isLeaf) return;
    if (nodeDepth (nodeId) >= kMaxDepth) return;

    const float midFreq = (node->freqLow + node->freqHigh) * 0.5f;

    // Left child (L)
    TreeNode leftChild;
    leftChild.nodeId   = 2 * nodeId;
    leftChild.path     = node->path + "L";
    leftChild.isLeaf   = true;
    leftChild.freqLow  = node->freqLow;
    leftChild.freqHigh = midFreq;
    nodes_.push_back (leftChild);
    node->children.push_back (leftChild.nodeId);

    // Right child (H)
    TreeNode rightChild;
    rightChild.nodeId   = 2 * nodeId + 1;
    rightChild.path     = node->path + "H";
    rightChild.isLeaf   = true;
    rightChild.freqLow  = midFreq;
    rightChild.freqHigh = node->freqHigh;
    nodes_.push_back (rightChild);

    // Re-find node pointer (push_back may have reallocated)
    if (auto* n = findNode (nodeId))
    {
        n->children.push_back (rightChild.nodeId);
        n->isLeaf = false;
    }
}

void TopologyEditorView::mergeNode (int nodeId)
{
    auto* node = findNode (nodeId);
    if (!node || node->isLeaf) return;

    // Recursively remove all descendants
    std::function<void(int)> removeDescendants = [&] (int id)
    {
        auto* n = findNode (id);
        if (!n) return;
        for (int childId : n->children)
            removeDescendants (childId);
        nodes_.erase (std::remove_if (nodes_.begin(), nodes_.end(),
                                      [id] (const TreeNode& tn) { return tn.nodeId == id; }),
                      nodes_.end());
    };

    for (int childId : node->children)
        removeDescendants (childId);

    // Re-find after erase
    if (auto* n = findNode (nodeId))
    {
        n->children.clear();
        n->isLeaf = true;
    }
}

int TopologyEditorView::nodeAtPoint (juce::Point<float> point) const noexcept
{
    for (const auto& node : nodes_)
    {
        if (node.bounds.contains (point))
            return node.nodeId;
    }
    return -1;
}

std::vector<std::string> TopologyEditorView::collectDestinations() const
{
    std::vector<std::string> dests;
    collectLeaves (1, dests);
    return dests;
}

void TopologyEditorView::collectLeaves (int nodeId,
                                         std::vector<std::string>& destinations) const
{
    const auto* node = findNode (nodeId);
    if (!node) return;

    if (node->isLeaf)
    {
        destinations.push_back (node->path);
        return;
    }

    for (int childId : node->children)
        collectLeaves (childId, destinations);
}

TopologyEditorView::TreeNode* TopologyEditorView::findNode (int nodeId) noexcept
{
    for (auto& n : nodes_)
        if (n.nodeId == nodeId) return &n;
    return nullptr;
}

const TopologyEditorView::TreeNode* TopologyEditorView::findNode (int nodeId) const noexcept
{
    for (const auto& n : nodes_)
        if (n.nodeId == nodeId) return &n;
    return nullptr;
}

int TopologyEditorView::nodeDepth (int nodeId) noexcept
{
    // Depth = floor(log2(nodeId))
    int depth = 0;
    while (nodeId > 1) { nodeId >>= 1; ++depth; }
    return depth;
}

//==============================================================================
// Drawing helpers
//==============================================================================

void TopologyEditorView::drawNode (juce::Graphics& g, const TreeNode& node) const
{
    const float cornerRadius = 4.0f * zoomScale_;

    if (node.isLeaf)
    {
        g.setColour (juce::Colour (kLeafFill));
        g.fillRoundedRectangle (node.bounds, cornerRadius);
        g.setColour (juce::Colour (kLeafOutline));
        g.drawRoundedRectangle (node.bounds, cornerRadius, 1.5f);
    }
    else
    {
        g.setColour (juce::Colour (kSplitFill));
        g.fillRoundedRectangle (node.bounds, cornerRadius);
        g.setColour (juce::Colour (kSplitOutline));
        g.drawRoundedRectangle (node.bounds, cornerRadius, 1.0f);
    }

    // Label: path string (or "Root")
    const juce::String label = node.path.empty() ? juce::String ("Root")
                                                   : juce::String (node.path);
    g.setColour (juce::Colour (kLabelColour));
    g.setFont (juce::Font (10.0f * zoomScale_));
    g.drawText (label, node.bounds.toNearestInt(), juce::Justification::centred, true);
}

void TopologyEditorView::drawEdge (juce::Graphics& g,
                                    const TreeNode& parent,
                                    int childId) const
{
    const auto* child = findNode (childId);
    if (!child) return;

    const juce::Point<float> from = parent.bounds.getCentre();
    const juce::Point<float> to   = child->bounds.getCentre();

    g.setColour (juce::Colour (kEdgeColour));
    g.drawLine (from.x, from.y, to.x, to.y, 1.0f);
}

void TopologyEditorView::drawTooltip (juce::Graphics& g, const TreeNode& node) const
{
    const juce::String pathStr  = node.path.empty() ? "Root" : juce::String (node.path);
    const juce::String typeStr  = node.isLeaf ? "Leaf" : "Splitter";
    const juce::String freqStr  = juce::String (static_cast<int> (node.freqLow))
                                  + " – "
                                  + juce::String (static_cast<int> (node.freqHigh))
                                  + " Hz";

    const juce::String tooltip  = pathStr + "\n" + typeStr + "\n" + freqStr;

    // Measure tooltip size
    juce::Font font (11.0f);
    g.setFont (font);

    constexpr int kPad = 6;
    const int tooltipW = 110;
    const int tooltipH = 50;

    // Position: above/right of node
    const int tx = static_cast<int> (node.bounds.getRight())  + 4;
    const int ty = static_cast<int> (node.bounds.getY())      - kPad;

    const juce::Rectangle<int> tooltipRect (tx, ty, tooltipW, tooltipH);

    g.setColour (juce::Colour (kTooltipBg));
    g.fillRoundedRectangle (tooltipRect.toFloat(), 4.0f);

    g.setColour (juce::Colours::white);
    g.drawMultiLineText (tooltip,
                         tooltipRect.getX() + kPad,
                         tooltipRect.getY() + kPad + static_cast<int> (font.getHeight()),
                         tooltipRect.getWidth() - 2 * kPad);
}
