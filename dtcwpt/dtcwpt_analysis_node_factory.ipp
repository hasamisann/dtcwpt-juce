/**
 * @file dtcwpt_analysis_node_factory.ipp
 * @brief Implementation of shared analysis-node factory helper.
 */

#include "dtcwpt_analysis_node_factory.h"
#include "dtcwpt_filter_coeffs.h"

namespace dtcwpt {

const filters::FilterCoefficients& filterCoefficientsForNode(int nodeId, AnalysisTreeKind treeKind) {
    if (nodeId == 1) {
        return treeKind == AnalysisTreeKind::real ? filters::CDF_RE : filters::CDF_IM;
    }

    if (nodeId % 2 == 1) {
        return filters::PACKET;
    }

    return treeKind == AnalysisTreeKind::real ? filters::QSHIFT14_RE : filters::QSHIFT14_IM;
}

AnalysisNode createAnalysisNodeForId(int nodeId, AnalysisTreeKind treeKind) {
    return AnalysisNode(filterCoefficientsForNode(nodeId, treeKind), nodeId == 1);
}

} // namespace dtcwpt
