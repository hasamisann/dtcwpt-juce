/**
 * @file TopologyEditorView.cpp
 * @brief Interactive wavelet tree topology editor implementation.
 */

#include "TopologyEditorView.h"
#include "FontHelper.h"
#include "GateGuiFrequencyMapping.h"
#include "../../common/TopologySplitOrder.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// Colours (anonymous namespace)
//==============================================================================

namespace
{
    /** Background colour for the canvas. */
    constexpr juce::uint32 kBgColour       = 0xFF202026;

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
    , sampleRate_  (gate_gui::sanitizeSampleRateAndNyquist (static_cast<float> (sampleRate)).sampleRate)
{
    buildTree (kDefaultTopology);

    // Back button
    addAndMakeVisible (backButton_);
    backButton_.setLookAndFeel (&buttonLnf_);
    backButton_.onClick = [this]
    {
        if (onBack_) onBack_();
    };

    // Type ComboBox: DWT or Full Tree
    typeCombo_.setLookAndFeel (&buttonLnf_);
    typeCombo_.addItem ("DWT",  1);
    typeCombo_.addItem ("Full", 2);
    typeCombo_.setTextWhenNothingSelected ("Type");

    // Depth ComboBox: 1–8
    depthCombo_.setLookAndFeel (&buttonLnf_);
    for (int d = 1; d <= 8; ++d)
        depthCombo_.addItem (juce::String (d), d);
    depthCombo_.setTextWhenNothingSelected ("Depth");

    // Shared onChange: fires only when both selectors have a valid choice
    auto applyPreset = [this]
    {
        const int typeId  = typeCombo_.getSelectedId();
        const int depthId = depthCombo_.getSelectedId();
        if (typeId <= 0 || depthId <= 0) return;

        std::vector<std::string> dests;
        if (typeId == 1)
            dests = generateDWT (depthId);
        else
            dests = generateFullTree (depthId);

        buildTree (dests);
        layoutTree();
        repaint();

        topoState_.requestChange (dests);
        if (onTopologyChanged_) onTopologyChanged_ (dests);
    };

    typeCombo_.onChange  = applyPreset;
    depthCombo_.onChange = applyPreset;

    addAndMakeVisible (typeCombo_);
    addAndMakeVisible (depthCombo_);
}

//==============================================================================
// Destructor
//==============================================================================

TopologyEditorView::~TopologyEditorView()
{
    // Clear LookAndFeels
    backButton_.setLookAndFeel (nullptr);
    typeCombo_.setLookAndFeel (nullptr);
    depthCombo_.setLookAndFeel (nullptr);

    // Remove all child components before member sub-components are destroyed.
    // Component::~Component() would otherwise call removeAllChildren() after
    // all members are already gone, causing use-after-free / heap corruption.
    removeAllChildren();
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

void TopologyEditorView::setTopologyChangedCallback (
    std::function<void(const std::vector<std::string>&)> cb)
{
    onTopologyChanged_ = std::move (cb);
}

//==============================================================================
// Component overrides
//==============================================================================

void TopologyEditorView::resized()
{
    backButton_.setBounds (4, 4, 70, 24);
    typeCombo_.setBounds  (78,  4, 70, 24);
    depthCombo_.setBounds (152, 4, 60, 24);
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

    // Draw static help text at fixed screen position (not affected by pan/zoom)
    g.setColour (juce::Colour (0x40FFFFFF));
    g.setFont (FontHelper::getFont (10.0f));
    g.drawMultiLineText ("Click leaf: split | Click splitter: merge\nScroll: zoom | Drag: pan",
                         10,
                         44 + static_cast<int> (FontHelper::getFont (10.0f).getHeight()),
                         getWidth() - 20);
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
            if (onTopologyChanged_) onTopologyChanged_ (dests);
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
        if (onTopologyChanged_) onTopologyChanged_ (dests);
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

void TopologyEditorView::mouseWheelMove (const juce::MouseEvent& e,
                                          const juce::MouseWheelDetails& wheel)
{
    constexpr float kZoomMin  = 0.3f;
    constexpr float kZoomMax  = 4.0f;
    constexpr float kZoomStep = 0.1f;

    const float oldZoom = zoomScale_;
    const float newZoom = std::clamp (oldZoom + wheel.deltaY * kZoomStep,
                                      kZoomMin, kZoomMax);

    // Adjust pan so the world-space point under the cursor stays fixed:
    //   screenPos = panOffset + worldPos * zoom
    //   worldPos  = (screenPos - panOffset) / zoom
    //   new panOffset = screenPos - worldPos * newZoom
    const auto mousePos = e.position;
    panOffset_ = mousePos - (mousePos - panOffset_) * (newZoom / oldZoom);
    zoomScale_ = newZoom;

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

    if (destinations.empty())
        return;

    const auto sanitized = gate_gui::sanitizeSampleRateAndNyquist (static_cast<float> (sampleRate_));

    // Root node (node ID = 1)
    TreeNode root;
    root.nodeId   = 1;
    root.path     = "";
    root.isLeaf   = false;
    root.freqLow  = 0.0f;
    root.freqHigh = sanitized.nyquist;
    nodes_.push_back (root);

    // For each destination, walk down the tree and create nodes as needed
    for (const auto& dest : destinations)
    {
        int currentId = 1;

        for (size_t i = 0; i < dest.size(); ++i)
        {
            char c = dest[i];
            // Determine child ID: 'L' → left (2n), 'H' → right (2n+1)
            const int childId = (c == 'L') ? (2 * currentId) : (2 * currentId + 1);
            const bool isLastChar = (i == dest.size() - 1);

            // Find or create child node
            auto* childNode = findNode (childId);
            if (childNode == nullptr)
            {
                // Get parent's freq range
                auto* parentNode = findNode (currentId);
                const float parentLow  = parentNode ? parentNode->freqLow  : 0.0f;
                const float parentHigh = parentNode ? parentNode->freqHigh
                                                    : sanitized.nyquist;
                const float midFreq = (parentLow + parentHigh) * 0.5f;

                TreeNode newNode;
                newNode.nodeId  = childId;
                // Build path string (FIXED: use i+1 instead of pointer arithmetic)
                newNode.path    = dest.substr (0, i + 1);
                newNode.isLeaf  = isLastChar;
                newNode.freqLow  = (c == 'L') ? parentLow  : midFreq;
                newNode.freqHigh = (c == 'L') ? midFreq     : parentHigh;
                nodes_.push_back (newNode);

                // Re-find parent after push_back: the push_back may have reallocated
                // nodes_, invalidating the parentNode pointer obtained above.
                if (parentNode != nullptr)
                {
                    if (auto* pAfterPush = findNode (currentId))
                        pAfterPush->children.push_back (childId);
                }
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
    // Pass 1: assign vertical slot indices (leaf-count-based layout).
    // Each leaf gets a unique sequential slot; internal nodes get the average
    // of their children's slots. This guarantees no overlaps regardless of depth.
    assignSlots (1, 0);

    // Pass 2: convert slot indices to pixel positions.
    //   x = panOffset.x + depth * kDepthSpacing * zoom
    //   y = panOffset.y + headerMargin + slot * kSlotHeight * zoom
    constexpr float kHeaderMargin = 40.0f;

    for (auto& node : nodes_)
    {
        const int   depth = nodeDepth (node.nodeId);
        const float x = panOffset_.x + static_cast<float> (depth) * kDepthSpacing * zoomScale_;
        const float y = panOffset_.y + kHeaderMargin + node.slot * kSlotHeight * zoomScale_;

        node.bounds = juce::Rectangle<float> (x, y,
                                               kNodeWidth  * zoomScale_,
                                               kNodeHeight * zoomScale_);
    }
}

int TopologyEditorView::assignSlots (int nodeId, int nextSlot)
{
    auto* node = findNode (nodeId);
    if (!node) return nextSlot;

    if (node->isLeaf)
    {
        node->slot = static_cast<float> (nextSlot);
        return nextSlot + 1;
    }

    // Internal node: recurse on children (L child first = low-freq at bottom)
    float slotSum = 0.0f;
    for (int childId : node->children)
    {
        nextSlot  = assignSlots (childId, nextSlot);
        if (const auto* child = findNode (childId))
            slotSum += child->slot;
    }

    // Internal node gets the average slot of its children
    node->slot = (node->children.empty())
                     ? static_cast<float> (nextSlot)
                     : slotSum / static_cast<float> (node->children.size());

    return nextSlot;
}


void TopologyEditorView::splitNode (int nodeId)
{
    auto* node = findNode (nodeId);
    if (!node || !node->isLeaf) return;
    if (nodeDepth (nodeId) >= kMaxDepth) return;

    // Cache parent properties before any push_back, which may reallocate
    // nodes_ and invalidate the `node` pointer (use-after-free fix).
    const float parentFreqLow  = node->freqLow;
    const float parentFreqHigh = node->freqHigh;
    const std::string parentPath = node->path;
    node = nullptr; // Discard to prevent accidental use after reallocation.

    const float midFreq = (parentFreqLow + parentFreqHigh) * 0.5f;

    const auto splitChildren = topology::makeHighFirstSplitChildren (nodeId, parentPath);

    TreeNode highChild;
    highChild.nodeId   = splitChildren.highChild.nodeId;
    highChild.path     = splitChildren.highChild.path;
    highChild.isLeaf   = true;
    highChild.freqLow  = midFreq;
    highChild.freqHigh = parentFreqHigh;
    nodes_.push_back (highChild); // May reallocate nodes_.

    TreeNode lowChild;
    lowChild.nodeId   = splitChildren.lowChild.nodeId;
    lowChild.path     = splitChildren.lowChild.path;
    lowChild.isLeaf   = true;
    lowChild.freqLow  = parentFreqLow;
    lowChild.freqHigh = midFreq;
    nodes_.push_back (lowChild); // May reallocate nodes_.

    // Re-acquire parent pointer after all push_backs are complete.
    if (auto* n = findNode (nodeId))
    {
        n->children.push_back (highChild.nodeId);
        n->children.push_back (lowChild.nodeId);
        n->isLeaf = false;
    }
}

void TopologyEditorView::mergeNode (int nodeId)
{
    // Prevent merging the root node (nodeId 1) to enforce a minimum depth of 1 (H/L split).
    if (nodeId == 1) return;

    auto* node = findNode (nodeId);
    if (!node || node->isLeaf) return;

    // Recursively remove all descendants
    std::function<void(int)> removeDescendants = [&] (int id)
    {
        auto* n = findNode (id);
        if (!n) return;
        // Cache children before erasing from nodes_, which shifts elements
        // and may invalidate the `n` pointer (iterator/pointer invalidation fix).
        const std::vector<int> grandchildren = n->children;
        for (int childId : grandchildren)
            removeDescendants (childId);
        nodes_.erase (std::remove_if (nodes_.begin(), nodes_.end(),
                                      [id] (const TreeNode& tn) { return tn.nodeId == id; }),
                      nodes_.end());
    };

    // Cache the immediate children before calling removeDescendants, because
    // erasing descendants from nodes_ shifts elements and invalidates `node`.
    const std::vector<int> childrenToMerge = node->children;
    node = nullptr; // Discard to prevent accidental use after pointer invalidation.

    for (int childId : childrenToMerge)
        removeDescendants (childId);

    // Re-acquire parent pointer after all erasures are complete.
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
// Preset topology generators
//==============================================================================

std::vector<std::string> TopologyEditorView::generateDWT (int depth)
{
    // A DWT of depth N produces N+1 leaves:
    //   "H", "LH", "LLH", ..., "L...LH", "L...L" (N L's)
    // The low-frequency band is recursively split at each level.
    std::vector<std::string> dests;
    dests.reserve (static_cast<std::size_t> (depth + 1));

    std::string prefix;
    for (int i = 0; i < depth; ++i)
    {
        dests.push_back (prefix + "H");
        prefix += "L";
    }
    // Final leaf: all L's (deepest low-frequency band)
    dests.push_back (prefix);

    return dests;
}

std::vector<std::string> TopologyEditorView::generateFullTree (int depth)
{
    // A Full Tree of depth N produces 2^N leaves by recursively splitting
    // every node at each level. Enumerate in DFS order (H before L) so the
    // highest-frequency leaf receives slot 0 (top of canvas), matching DWT
    // display convention.
    std::vector<std::string> dests;
    dests.reserve (static_cast<std::size_t> (1) << static_cast<std::size_t> (depth));

    // Enumerate in DFS order (H before L) so high-frequency leaves get the
    // lowest slot index (top of canvas), matching DWT display convention.
    std::function<void(std::string, int)> enumerate = [&] (std::string path, int remaining)
    {
        if (remaining == 0)
        {
            dests.push_back (std::move (path));
            return;
        }
        enumerate (path + "H", remaining - 1);
        enumerate (path + "L", remaining - 1);
    };

    enumerate ("", depth);
    return dests;
}

//==============================================================================
// Drawing helpers
//==============================================================================

void TopologyEditorView::drawNode (juce::Graphics& g, const TreeNode& node) const
{
    const float cornerRadius = 0.0f; // Flat nodes

    if (node.isLeaf)
    {
        g.setColour (juce::Colour (kLeafFill));
        g.fillRect (node.bounds);
        g.setColour (juce::Colour (kLeafOutline));
        g.drawRect (node.bounds, 1.5f);
    }
    else
    {
        g.setColour (juce::Colour (kSplitFill));
        g.fillRect (node.bounds);
        g.setColour (juce::Colour (kSplitOutline));
        g.drawRect (node.bounds, 1.0f);
    }

    // Label: path string (or "Root")
    const juce::String label = node.path.empty() ? juce::String ("Root")
                                                   : juce::String (node.path);
    g.setColour (juce::Colour (kLabelColour));
    g.setFont (FontHelper::getFont (10.0f * zoomScale_));
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
    juce::Font font = FontHelper::getFont (11.0f);
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
