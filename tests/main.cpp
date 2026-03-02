/**
 * @file main.cpp
 * @brief JUCE UnitTestRunner main entry point for dtcwpt_tests.
 * 
 * This executable runs all registered JUCE UnitTest suites and reports results.
 * Returns exit code 0 if all tests pass, 1 if any test fails.
 */

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>

int main(int argc, char* argv[])
{
    // Initialise the JUCE MessageManager on the main thread.
    // This is required by AudioProcessor (and other JUCE components) even in
    // headless/console applications. The MessageManager must be created before
    // any AudioProcessor is instantiated.
    juce::initialiseJuce_GUI();

    // Create unit test runner
    juce::UnitTestRunner runner;
    
    // Enable logging of passes for verbose output
    runner.setPassesAreLogged(true);

    // Parse command line for test name filtering
    juce::String testName = "*";
    for (int i = 0; i < argc - 1; ++i) {
        juce::String arg(argv[i]);
        if (arg == "--test" || arg == "-t") {
            testName = juce::String(argv[i + 1]);
            break;
        }
    }

    // Print header
    std::cout << "========================================" << std::endl;
    std::cout << "DT-CWPT Unit Test Runner" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Test filter: " << testName.toStdString() << std::endl;
    std::cout << std::endl;

    // Run tests
    if (testName == "*") {
        runner.runAllTests(42);  // Run all tests with fixed seed
    } else {
        runner.runTestsWithName(testName, 42);  // Run tests matching name
    }

    // Check results
    int totalPasses = 0;
    int totalFailures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i) {
        auto* result = runner.getResult(i);
        if (result != nullptr) {
            totalPasses += result->passes;
            totalFailures += result->failures;
            if (result->failures > 0) {
                std::cout << "[FAIL] " << result->unitTestName.toStdString()
                          << " / " << result->subcategoryName.toStdString()
                          << "  (passes=" << result->passes
                          << " failures=" << result->failures << ")" << std::endl;
                for (auto& msg : result->messages)
                    std::cout << "       " << msg.toStdString() << std::endl;
            }
        }
    }

    // Print summary
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Test Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Total assertions: " << (totalPasses + totalFailures) << std::endl;
    std::cout << "Passed:           " << totalPasses << std::endl;
    std::cout << "Failed:           " << totalFailures << std::endl;
    std::cout << std::endl;

    if (totalFailures == 0) {
        std::cout << "All tests passed!" << std::endl;
        juce::shutdownJuce_GUI();
        return 0;
    } else {
        std::cout << "Some tests failed!" << std::endl;
        juce::shutdownJuce_GUI();
        return 1;
    }
}
