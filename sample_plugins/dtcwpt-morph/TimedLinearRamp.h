#pragma once

/**
 * @file TimedLinearRamp.h
 * @brief Linear parameter ramp evaluated as a closed-form function of time.
 *
 * Unlike juce::SmoothedValue, which advances one step per getNextValue() call,
 * TimedLinearRamp is queried with a time offset and returns the ramp value at
 * that time. This lets subband processors that visit the same time span at
 * different rates (one DT-CWPT band per tree depth) all see the same value for
 * the same instant, independent of processing order, channel or bypass state.
 *
 * Time is measured in host samples. The origin is the position of the last
 * advance()/retarget() call.
 */

#include <algorithm>

class TimedLinearRamp
{
public:
    /** Exact equality (a repeated identical request, not "close enough"). */
    static bool sameValue (double a, double b) noexcept { return ! (a < b) && ! (a > b); }

    /** Sets the value and the target to @p v and stops any ramp in progress. */
    void setCurrentAndTarget (double v) noexcept
    {
        start_ = target_ = v;
        length_ = remaining_ = 0.0;
    }

    /**
     * @brief Starts a ramp from the current value (at the origin) to @p newTarget.
     *
     * The ramp takes @p rampSamples host samples. A non-positive length snaps to the
     * target. Retargeting to the value that is already the target is a no-op, so
     * repeated identical requests do not stretch a ramp in progress.
     */
    void retarget (double newTarget, double rampSamples) noexcept
    {
        if (sameValue (newTarget, target_))
            return;

        if (rampSamples <= 0.0)
        {
            setCurrentAndTarget (newTarget);
            return;
        }

        start_     = value (0.0);
        target_    = newTarget;
        length_    = rampSamples;
        remaining_ = rampSamples;
    }

    /** Ramp value @p t host samples after the origin (t >= 0). */
    double value (double t) const noexcept
    {
        if (t >= remaining_)
            return target_;

        // remaining_ > t >= 0 implies length_ > 0
        return target_ - (target_ - start_) * ((remaining_ - t) / length_);
    }

    /** Moves the origin @p t host samples forward (t >= 0). */
    void advance (double t) noexcept
    {
        remaining_ = std::max (0.0, remaining_ - t);
    }

    double getTarget() const noexcept { return target_; }

    /** True while the ramp has not reached its target. */
    bool isRamping() const noexcept { return remaining_ > 0.0; }

private:
    double start_     { 0.0 };
    double target_    { 0.0 };
    double length_    { 0.0 };   ///< Total length of the current ramp (host samples)
    double remaining_ { 0.0 };   ///< Host samples until the target is reached
};
