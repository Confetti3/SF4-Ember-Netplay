cmake_minimum_required(VERSION 3.20)
project(EmberDegradedConnectionTests LANGUAGES CXX)
include(CTest)

# Standalone on purpose: no Windows, game, Rust, or vcpkg dependencies.
# The coarse pacing controller runs as PacingController in the core-only tests.
add_executable(DegradedConnectionPolicies policies_test.cxx)
target_compile_features(DegradedConnectionPolicies PRIVATE cxx_std_11)
if(MSVC)
    target_compile_options(DegradedConnectionPolicies PRIVATE /W4 /WX)
else()
    target_compile_options(DegradedConnectionPolicies PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME DegradedConnectionPolicies COMMAND DegradedConnectionPolicies)
set_tests_properties(DegradedConnectionPolicies PROPERTIES TIMEOUT 15)
