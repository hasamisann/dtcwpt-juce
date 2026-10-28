/**
 * @file MorphTopologyHandoffTests.cpp
 * @brief Topology change / state handoff scenarios (#3, #4, #5) for MorphAudioProcessor.
 */

#include "MorphAudioProcessor.h"

#include "../util/TopologyHandoffScenarios.h"

class MorphTopologyHandoffTests : public TopologyHandoffScenarios<MorphAudioProcessor>
{
public:
    MorphTopologyHandoffTests() : TopologyHandoffScenarios ("MorphTopologyHandoffTests") {}
};

static MorphTopologyHandoffTests morphTopologyHandoffTests;
