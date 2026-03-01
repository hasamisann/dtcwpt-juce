/*
  BEGIN_JUCE_MODULE_DECLARATION

  ID:                   dtcwpt
  vendor:               nalum
  version:              0.1.0
  name:                 DT-CWPT Core Library
  description:          Dual-Tree Complex Wavelet Packet Transform for real-time audio processing.
  minimumCppStandard:   20
  dependencies:         juce_core juce_audio_basics

  END_JUCE_MODULE_DECLARATION
*/

#pragma once

#include "dtcwpt_filter_structs.h"
#include "dtcwpt_filter_coeffs.h"
#include "dtcwpt_stateful_filter.h"
#include "dtcwpt_analysis_node.h"
#include "dtcwpt_synthesis_node.h"
#include "dtcwpt_delay_buffer.h"
#include "dtcwpt_topology_planner.h"
#include "dtcwpt_band_processor.h"
#include "dtcwpt_complex_utils.h"
#include "dtcwpt_processor.h"
