#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace graphic_eq
{
inline std::vector<int> enumerateCrossedBands (int startBand, int endBand, int numBands)
{
    if (numBands <= 0)
        return {};

    if (startBand < 0 || endBand < 0 || startBand >= numBands || endBand >= numBands)
        return {};

    std::vector<int> bands;

    if (startBand <= endBand)
    {
        bands.reserve (static_cast<std::size_t> (endBand - startBand + 1));
        for (int band = startBand; band <= endBand; ++band)
            bands.push_back (band);
    }
    else
    {
        bands.reserve (static_cast<std::size_t> (startBand - endBand + 1));
        for (int band = startBand; band >= endBand; --band)
            bands.push_back (band);
    }

    return bands;
}

inline float interpolateYForX (float x0, float y0, float x1, float y1, float x)
{
    constexpr float epsilon = 1.0e-5f;
    const float dx = x1 - x0;

    if (std::abs (dx) <= epsilon)
        return 0.5f * (y0 + y1);

    float t = (x - x0) / dx;
    if (t < 0.0f)
        t = 0.0f;
    else if (t > 1.0f)
        t = 1.0f;
    return y0 + ((y1 - y0) * t);
}
} // namespace graphic_eq
