/**
 * @file TestUtils.cpp
 * @brief Implementation of common test utilities for DT-CWPT test suites.
 */

#include "TestUtils.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <random>
#include <set>

//==============================================================================
// Allocation Tracking - Global operator new/delete override
// These MUST be in global namespace
//==============================================================================

// Thread-local counter to avoid cross-test interference
static thread_local size_t g_allocationCount = 0;

// Global operator new/delete overrides (MUST be in global namespace)
void* operator new(size_t size) {
    ++g_allocationCount;
    return std::malloc(size);
}

void* operator new[](size_t size) {
    ++g_allocationCount;
    return std::malloc(size);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    std::free(ptr);
}

// Accessor for the global counter (used by AllocationCounter)
namespace TestUtils {

size_t getGlobalAllocationCount() {
    return g_allocationCount;
}

AllocationCounter::AllocationCounter()
    : startCount_(0), endCount_(0), counting_(false) {}

AllocationCounter::~AllocationCounter() = default;

void AllocationCounter::begin() {
    startCount_ = g_allocationCount;
    counting_ = true;
}

void AllocationCounter::end() {
    endCount_ = g_allocationCount;
    counting_ = false;
}

size_t AllocationCounter::getCount() const {
    if (counting_) {
        return g_allocationCount - startCount_;
    }
    return endCount_ - startCount_;
}

//==============================================================================
// Signal Generators
//==============================================================================

std::vector<double> generateSine(double frequency, double duration, double sampleRate) {
    const int length = static_cast<int>(sampleRate * duration);
    std::vector<double> out(static_cast<size_t>(length), 0.0);
    constexpr double twoPi = 6.28318530717958647692;
    
    for (int i = 0; i < length; ++i) {
        out[static_cast<size_t>(i)] = 0.5 * std::sin(twoPi * frequency * static_cast<double>(i) / sampleRate);
    }
    
    return out;
}

std::vector<double> generateNoise(double duration, double sampleRate, unsigned int seed) {
    const int length = static_cast<int>(sampleRate * duration);
    std::vector<double> out(static_cast<size_t>(length), 0.0);
    
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> dist(-0.5, 0.5);
    
    for (int i = 0; i < length; ++i) {
        out[static_cast<size_t>(i)] = dist(gen);
    }
    
    return out;
}

std::vector<double> generateImpulse(int totalSamples, int impulsePos) {
    std::vector<double> out(static_cast<size_t>(totalSamples), 0.0);
    if (impulsePos >= 0 && impulsePos < totalSamples) {
        out[static_cast<size_t>(impulsePos)] = 1.0;
    }
    return out;
}

std::vector<double> generateSweep(double f0, double f1, double duration, double sampleRate) {
    const int length = static_cast<int>(sampleRate * duration);
    std::vector<double> out(static_cast<size_t>(length), 0.0);
    constexpr double twoPi = 6.28318530717958647692;
    
    for (int i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double phase = twoPi * (f0 * t + (f1 - f0) * t * t / (2.0 * duration));
        out[static_cast<size_t>(i)] = 0.5 * std::sin(phase);
    }
    
    return out;
}

//==============================================================================
// MSE Calculation
//==============================================================================

double calculateMSEdB(const std::vector<double>& input, 
                      const std::vector<double>& output, 
                      int offset) {
    if (input.empty() || output.size() < input.size() + static_cast<size_t>(offset)) {
        return 0.0;
    }
    
    double mse = 0.0;
    double signalPower = 0.0;
    
    for (size_t i = 0; i < input.size(); ++i) {
        const double diff = input[i] - output[static_cast<size_t>(offset) + i];
        mse += diff * diff;
        signalPower += input[i] * input[i];
    }
    
    mse /= static_cast<double>(input.size());
    signalPower /= static_cast<double>(input.size());
    
    constexpr double eps = 1e-30;
    return 10.0 * std::log10((mse + eps) / (signalPower + eps));
}

//==============================================================================
// Topology Utilities
//==============================================================================

std::vector<std::string> getFullPacketDestinations(int maxLevel) {
    std::vector<std::string> dests;
    
    std::function<void(const std::string&, int)> addPaths = [&](const std::string& currentPath, int level) {
        if (level > maxLevel) {
            return;
        }
        
        // Add path only at maxLevel (non-empty)
        if (!currentPath.empty() && level == maxLevel) {
            dests.push_back(currentPath);
        }
        
        // Recurse to children
        addPaths(currentPath + "L", level + 1);
        addPaths(currentPath + "H", level + 1);
    };
    
    addPaths("", 0);
    return dests;
}

std::vector<std::string> getWaveletTreeDestinations(int maxLevel) {
    std::vector<std::string> dests;
    
    std::function<void(const std::string&, int)> addPaths = [&](const std::string& currentPath, int level) {
        // Stop if past maxLevel or if path ends with 'H' (and has at least 2 chars)
        if (level > maxLevel || (currentPath.length() >= 2 && currentPath[currentPath.length() - 2] == 'H')) {
            return;
        }
        
        // Add path at maxLevel or if it ends with 'H'
        if ((!currentPath.empty() && level == maxLevel) || 
            (!currentPath.empty() && currentPath.back() == 'H')) {
            dests.push_back(currentPath);
        }
        
        // Recurse to children
        addPaths(currentPath + "L", level + 1);
        addPaths(currentPath + "H", level + 1);
    };
    
    addPaths("", 0);
    return dests;
}

std::vector<std::string> getRandomPacketDestinations(int maxLevel, unsigned int seed) {
    std::vector<std::string> leaves;
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    
    std::function<void(const std::string&)> buildTree = [&](const std::string& path) {
        const int currentDepth = static_cast<int>(path.length());
        
        // If at max level, must be a leaf
        if (currentDepth >= maxLevel) {
            leaves.push_back(path);
            return;
        }
        
        // Root ('') always splits
        // Others: 60% chance to split
        if (path.empty() || dist(gen) < 0.6) {
            // Split: add L and H children
            buildTree(path + "L");
            buildTree(path + "H");
        } else {
            // Make this node a leaf
            leaves.push_back(path);
        }
    };
    
    buildTree("");
    return leaves;
}

bool validateDestinations(const std::vector<std::string>& destinations) {
    // Build set of all paths and their ancestors
    std::set<std::string> leafSet(destinations.begin(), destinations.end());
    std::set<std::string> allNodes;
    
    for (const auto& path : destinations) {
        // Add all ancestors
        for (size_t len = 0; len <= path.length(); ++len) {
            allNodes.insert(path.substr(0, len));
        }
    }
    
    // For every internal node (non-leaf), check it has exactly 2 children
    for (const auto& node : allNodes) {
        if (leafSet.count(node) > 0) {
            // It's a leaf. Must NOT have any child in allNodes.
            const std::string childL = node + "L";
            const std::string childH = node + "H";
            if (allNodes.count(childL) > 0 || allNodes.count(childH) > 0) {
                return false;
            }
        } else {
            // Internal node. Must have BOTH L and H children in allNodes.
            const std::string childL = node + "L";
            const std::string childH = node + "H";
            if (allNodes.count(childL) == 0 || allNodes.count(childH) == 0) {
                return false;
            }
        }
    }
    
    // Check no duplicates
    if (leafSet.size() != destinations.size()) {
        return false;
    }
    
    return true;
}

} // namespace TestUtils
