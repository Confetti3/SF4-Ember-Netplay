#pragma once

// Shared by the standalone test executables. A failed CHECK names the file,
// line and expression, then ends the process with exit code 1.
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Check failed at %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (false)
