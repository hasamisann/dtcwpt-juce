/**
 * @file dtcwpt_analysis_node_factory.h
 * @brief Shared analysis-node factory helper for consistent filter-family selection.
 * 
 * Centralizes the current node-ID-to-filter-family mapping used during analysis-node
 * construction so production prepare code and test utilities can both call the same
 * helper without duplicating filter-selection logic.
 */

#pragma once

#include "dtcwpt_analysis_node.h"

namespace dtcwpt {

enum class AnalysisTreeKind {
    real,
    imag,
};

/**
 * @brief Create an analysis node with the correct filter family for the given node ID and tree.
 * 
 * Filter-family selection rules:
 * - Node ID 1 (root), real tree: CDF_RE filters
 * - Node ID 1 (root), imaginary tree: CDF_IM filters
 * - Odd non-root node IDs, both trees: PACKET filters
 * - Even non-root node IDs, real tree: QSHIFT14_RE filters
 * - Even non-root node IDs, imaginary tree: QSHIFT14_IM filters
 * 
 * @param nodeId The node ID in the binary tree (1 = root)
 * @param treeKind The analysis tree kind (real or imag)
 * @return Constructed AnalysisNode with the appropriate filter family
 */
AnalysisNode createAnalysisNodeForId(int nodeId, AnalysisTreeKind treeKind);

} // namespace dtcwpt
