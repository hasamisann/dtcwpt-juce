#pragma once

inline double resolveGateEditorConstructionSampleRate (double hostSampleRate) noexcept
{
    return hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
}
