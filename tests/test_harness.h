#pragma once

#include <initializer_list>
#include <iostream>

namespace wolfie::tests {

struct TestCase {
    const char* name = nullptr;
    bool (*run)() = nullptr;
};

inline int runTestCases(std::initializer_list<TestCase> testCases) {
    bool allPassed = true;
    for (const TestCase& testCase : testCases) {
        if (testCase.run == nullptr) {
            std::cerr << "test case '" << (testCase.name != nullptr ? testCase.name : "<unnamed>")
                      << "' has no runner\n";
            allPassed = false;
            continue;
        }

        const bool passed = testCase.run();
        if (!passed) {
            std::cerr << "test failed: " << (testCase.name != nullptr ? testCase.name : "<unnamed>") << "\n";
            allPassed = false;
        }
    }

    return allPassed ? 0 : 1;
}

}  // namespace wolfie::tests
