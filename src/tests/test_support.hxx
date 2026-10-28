#pragma once

// Shared by the standalone test executables. Each macro is named for what a
// failure does, so a reader of a test can tell whether it stops there:
//
//   CHECK(c)                 names file, line and expression on stderr, then
//                            ends the process with exit code 1.
//   CHECK_MSG(c, fmt, ...)   as CHECK, with a printf-style detail after the
//                            expression (give it at least one argument).
//   CHECK_OR_THROW(c)        throws std::runtime_error naming file, line and
//                            expression, for tests that catch it in main.
//   EXPECT(c)                prints "FAIL file:line: expression" on stdout,
//                            counts the failure and goes on.
//   EXPECT_MSG(c, fmt, ...)  as EXPECT, with the detail in parentheses.
//
// A test that uses EXPECT reports through TEST_FAILURES() (the count so far)
// or TEST_EXIT_CODE() (0 when nothing failed, otherwise 1).
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace sf4e_test {
inline int& FailureCount() { static int count = 0; return count; }
}

#define TEST_FAILURES() (::sf4e_test::FailureCount())
#define TEST_EXIT_CODE() (TEST_FAILURES() ? 1 : 0)

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Check failed at %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (false)
#define CHECK_MSG(c, fmt, ...) do { if (!(c)) { std::fprintf(stderr, "Check failed at %s:%d: %s: " fmt "\n", __FILE__, __LINE__, #c, __VA_ARGS__); std::exit(1); } } while (false)
#define CHECK_OR_THROW(c) do { if (!(c)) { throw std::runtime_error(std::string("Check failed at ") + __FILE__ + ":" + std::to_string(__LINE__) + ": " + #c); } } while (false)
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++TEST_FAILURES(); } } while (false)
#define EXPECT_MSG(c, fmt, ...) do { if (!(c)) { std::printf("FAIL %s:%d: %s (" fmt ")\n", __FILE__, __LINE__, #c, __VA_ARGS__); ++TEST_FAILURES(); } } while (false)
