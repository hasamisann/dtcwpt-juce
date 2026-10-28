/**
 * @file GateTopologyHandoffTests.cpp
 * @brief Topology change / state handoff scenarios (#3, #4, #5) for GateAudioProcessor.
 */

#include "GateAudioProcessor.h"

#include "../util/TopologyHandoffScenarios.h"

class GateTopologyHandoffTests : public TopologyHandoffScenarios<GateAudioProcessor>
{
public:
    GateTopologyHandoffTests() : TopologyHandoffScenarios ("GateTopologyHandoffTests") {}
};

static GateTopologyHandoffTests gateTopologyHandoffTests;
