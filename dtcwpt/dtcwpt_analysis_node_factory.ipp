/**
 * @file dtcwpt_analysis_node_factory.ipp
 * @brief Implementation of shared analysis-node factory helper.
 */

#include "dtcwpt_analysis_node_factory.h"
#include "dtcwpt_filter_coeffs.h"

namespace dtcwpt {

AnalysisNode createAnalysisNodeForId(int nodeId, AnalysisTreeKind treeKind) {
    if (nodeId == 1) {
        if (treeKind == AnalysisTreeKind::real) {
            return AnalysisNode(filters::CDF_RE, true);
        }

        return AnalysisNode(filters::CDF_IM, true);
    }

    if (nodeId % 2 == 1) {
        return AnalysisNode(filters::PACKET, false);
    }

    if (treeKind == AnalysisTreeKind::real) {
        return AnalysisNode(filters::QSHIFT14_RE, false);
    }

    return AnalysisNode(filters::QSHIFT14_IM, false);
}

} // namespace dtcwpt
