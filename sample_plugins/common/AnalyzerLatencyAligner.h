#pragma once

#include <algorithm>
#include <vector>

class AnalyzerLatencyAligner
{
public:
    void prepare (int maxBlockSize, int maxDelaySamples)
    {
        maxBlockSize_ = std::max (1, maxBlockSize);
        maxDelaySamples_ = std::max (0, maxDelaySamples);

        const int ringSize = std::max (1, maxBlockSize_ + maxDelaySamples_ + 1);
        ringBuffer_.assign (static_cast<std::size_t> (ringSize), 0.0f);
        writeIndex_ = 0;
        delaySamples_ = 0;
    }

    void reset()
    {
        std::fill (ringBuffer_.begin(), ringBuffer_.end(), 0.0f);
        writeIndex_ = 0;
    }

    void setDelaySamples (int delaySamples)
    {
        const int clamped = std::clamp (delaySamples, 0, maxDelaySamples_);
        if (clamped != delaySamples_)
        {
            delaySamples_ = clamped;
            reset();
        }
    }

    int getDelaySamples() const noexcept
    {
        return delaySamples_;
    }

    void processBlock (const float* input, float* output, int numSamples) noexcept
    {
        if (ringBuffer_.empty() || input == nullptr || output == nullptr || numSamples <= 0)
            return;

        const int ringSize = static_cast<int> (ringBuffer_.size());
        const int samplesToProcess = std::min (numSamples, maxBlockSize_);

        for (int i = 0; i < samplesToProcess; ++i)
        {
            ringBuffer_[static_cast<std::size_t> (writeIndex_)] = input[i];

            int readIndex = writeIndex_ - delaySamples_;
            if (readIndex < 0)
                readIndex += ringSize;

            output[i] = ringBuffer_[static_cast<std::size_t> (readIndex)];

            ++writeIndex_;
            if (writeIndex_ >= ringSize)
                writeIndex_ = 0;
        }
    }

private:
    std::vector<float> ringBuffer_;
    int maxBlockSize_ { 1 };
    int maxDelaySamples_ { 0 };
    int delaySamples_ { 0 };
    int writeIndex_ { 0 };
};
