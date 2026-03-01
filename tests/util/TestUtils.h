/**
 * @file TestUtils.h
 * @brief Common test utilities for DT-CWPT test suites.
 * 
 * Provides:
 * - Test configuration constants (deterministic)
 * - Allocation tracking for real-time safety verification
 * - Signal generators (sine, noise, impulse, sweep)
 * - MSE calculation
 * - Topology generators and validators
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace TestUtils {

//==============================================================================
// Test Configuration Constants
//==============================================================================

/**
 * @brief Test configuration constants for deterministic testing.
 */
struct TestConfig {
    static constexpr double SAMPLE_RATE = 44100.0;
    static constexpr double EPSILON = 1e-10;
    static constexpr double STRICT_EPSILON = 1e-12;
    static constexpr double MSE_THRESHOLD_DB = -80.0;
    static constexpr unsigned int RANDOM_SEED = 42;
};

//==============================================================================
// Allocation Tracking
//==============================================================================

/**
 * @brief Allocation counter for verifying real-time safety.
 * 
 * Uses global operator new/delete overrides with thread-local storage
 * to track heap allocations during critical sections.
 * 
 * Usage:
 *   AllocationCounter counter;
 *   counter.begin();
 *   // ... code under test ...
 *   counter.end();
 *   size_t allocs = counter.getCount();
 */
class AllocationCounter {
public:
    AllocationCounter();
    ~AllocationCounter();
    
    /**
     * @brief Start counting allocations.
     * Resets the counter and enables tracking.
     */
    void begin();
    
    /**
     * @brief Stop counting allocations.
     */
    void end();
    
    /**
     * @brief Get the number of allocations since begin().
     * @return Number of heap allocations
     */
    size_t getCount() const;

private:
    size_t startCount_;
    size_t endCount_;
    bool counting_;
};

//==============================================================================
// Signal Generators
//==============================================================================

/**
 * @brief Generate a sine wave signal.
 * 
 * @param frequency Frequency in Hz
 * @param duration Duration in seconds
 * @param sampleRate Sample rate in Hz
 * @return Vector of samples (amplitude = 0.5)
 */
std::vector<double> generateSine(double frequency, double duration, double sampleRate);

/**
 * @brief Generate white noise signal.
 * 
 * @param duration Duration in seconds
 * @param sampleRate Sample rate in Hz
 * @param seed Random seed (default: 42 for deterministic output)
 * @return Vector of samples (amplitude in [-0.5, 0.5])
 */
std::vector<double> generateNoise(double duration, double sampleRate, unsigned int seed = TestConfig::RANDOM_SEED);

/**
 * @brief Generate an impulse signal.
 * 
 * @param totalSamples Total number of samples
 * @param impulsePos Position of the impulse (default: 0)
 * @return Vector with single 1.0 at impulsePos, zeros elsewhere
 */
std::vector<double> generateImpulse(int totalSamples, int impulsePos = 0);

/**
 * @brief Generate a frequency sweep (exponential chirp).
 * 
 * @param f0 Start frequency in Hz
 * @param f1 End frequency in Hz
 * @param duration Duration in seconds
 * @param sampleRate Sample rate in Hz
 * @return Vector of samples (amplitude = 0.5)
 */
std::vector<double> generateSweep(double f0, double f1, double duration, double sampleRate);

//==============================================================================
// MSE Calculation
//==============================================================================

/**
 * @brief Calculate MSE between input and output in dB.
 * 
 * Computes: MSE_dB = 10 * log10(MSE / signalPower)
 * 
 * @param input Input signal
 * @param output Output signal (must be same length as input)
 * @param offset Offset into output for alignment (default: 0)
 * @return MSE in dB (negative values indicate good reconstruction)
 */
double calculateMSEdB(const std::vector<double>& input, 
                      const std::vector<double>& output, 
                      int offset = 0);

//==============================================================================
// Topology Utilities
//==============================================================================

/**
 * @brief Generate full packet destinations (all leaves at max depth).
 * 
 * Returns all 2^depth paths (e.g., depth 2 -> {"LL", "LH", "HL", "HH"}).
 * 
 * @param maxLevel Maximum tree depth
 * @return Vector of destination path strings
 */
std::vector<std::string> getFullPacketDestinations(int maxLevel);

/**
 * @brief Generate wavelet tree destinations.
 * 
 * Wavelet tree: L-only deep branch, H leaves at each level.
 * 
 * @param maxLevel Maximum tree depth
 * @return Vector of destination path strings
 */
std::vector<std::string> getWaveletTreeDestinations(int maxLevel);

/**
 * @brief Generate random valid packet destinations.
 * 
 * Randomly decides to split (60% chance) or make leaf (40% chance).
 * Root always splits. Ensures valid binary tree structure.
 * 
 * @param maxLevel Maximum tree depth
 * @param seed Random seed (default: 42)
 * @return Vector of destination path strings
 */
std::vector<std::string> getRandomPacketDestinations(int maxLevel, unsigned int seed = TestConfig::RANDOM_SEED);

/**
 * @brief Validate that destinations form a valid binary tree leaf set.
 * 
 * A valid leaf set requires:
 * - Every internal node has exactly 0 or 2 children
 * - No duplicates
 * 
 * @param destinations Vector of destination path strings
 * @return true if valid, false otherwise
 */
bool validateDestinations(const std::vector<std::string>& destinations);

} // namespace TestUtils
