#pragma once
// No external test framework dependency (kept minimal deliberately -- the
// project's whole ethos is "no unnecessary bloat"). Each test_*.cpp file
// exposes a `void run_<name>_tests()` function that uses CHRONOS_CHECK.
// test_main.cpp calls each one; a failed CHECK prints and sets exit code 1.

#include <iostream>
#include <string>

inline int g_failures = 0;

#define CHRONOS_CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "CHECK FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            ++g_failures; \
        } \
    } while (0)
