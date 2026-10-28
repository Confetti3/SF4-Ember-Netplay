# Build the exact bridge used by this CMake tree. Missing tools and failed
# integration prerequisites fail this required gate instead of skipping tests.
function(require_success)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
        WORKING_DIRECTORY "${SOURCE}/sdk/typescript")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "SDK validation failed (${result}): ${ARGV}")
    endif()
endfunction()
# The first line of a tool's output, for the stamp of the installed tree.
function(tool_version out_var)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0 OR version STREQUAL "")
        message(FATAL_ERROR "SDK validation failed (${result}): ${ARGN}")
    endif()
    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()
set(BRIDGE "${EMBER_TARGET}/debug/ember-bridge.exe")
# The protocol contract the SDK test compares its tables with (the ember-protocol
# example contract), built here beside the bridge so the test finds it without
# cargo on its PATH.
set(CONTRACT "${EMBER_TARGET}/debug/examples/contract.exe")
# On the processors CTest reserves for this test (SF4E_CARGO_TEST_JOBS).
if(NOT JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "SDK validation needs JOBS, a positive integer")
endif()
if(NOT NODE OR NOT NPM)
    message(FATAL_ERROR "SDK validation needs NODE and NPM, the node and npm programs")
endif()
require_success("${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${EMBER_TARGET}"
    "${CARGO}" build --locked -j ${JOBS} --manifest-path "${SOURCE}/server/ember/Cargo.toml" -p ember-bridge)
if(NOT EXISTS "${BRIDGE}")
    message(FATAL_ERROR "SDK bridge prerequisite is missing: ${BRIDGE}")
endif()
require_success("${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${EMBER_TARGET}"
    "${CARGO}" build --locked -j ${JOBS} --manifest-path "${SOURCE}/server/ember/Cargo.toml" -p ember-protocol --example contract)
if(NOT EXISTS "${CONTRACT}")
    message(FATAL_ERROR "SDK protocol contract prerequisite is missing: ${CONTRACT}")
endif()
# npm ci deletes node_modules and installs it again from the lockfile. It runs
# only when the tree is not the one it last installed: the stamp, written after
# a successful install, holds the lockfile's hash, the node and npm versions and
# the hash of the tree's own record (node_modules/.package-lock.json), so a
# changed lockfile or tool, a removed or reinstalled tree, or no stamp installs
# again. Delete the stamp to install again by hand. The tree is in the source
# folder and the stamp in the build folder, so a second build folder over the
# same source installs once for its own stamp, and finds the tree the same.
set(SDK_STAMP "${EMBER_TARGET}/sdk-npm.stamp")
set(SDK_INSTALLED "${SOURCE}/sdk/typescript/node_modules/.package-lock.json")
tool_version(node_version "${NODE}" --version)
tool_version(npm_version "${NPM}" --version)
file(SHA256 "${SOURCE}/sdk/typescript/package-lock.json" lock_hash)
set(install_needed TRUE)
if(EXISTS "${SDK_STAMP}" AND EXISTS "${SDK_INSTALLED}")
    file(SHA256 "${SDK_INSTALLED}" installed_hash)
    file(READ "${SDK_STAMP}" recorded)
    if(recorded STREQUAL "lock ${lock_hash}\nnode ${node_version}\nnpm ${npm_version}\ninstalled ${installed_hash}\n")
        set(install_needed FALSE)
    endif()
endif()
if(install_needed)
    file(REMOVE "${SDK_STAMP}")
    require_success("${NPM}" ci --ignore-scripts)
    if(NOT EXISTS "${SDK_INSTALLED}")
        message(FATAL_ERROR "SDK validation failed: npm ci left no ${SDK_INSTALLED}")
    endif()
    file(SHA256 "${SDK_INSTALLED}" installed_hash)
    file(WRITE "${SDK_STAMP}" "lock ${lock_hash}\nnode ${node_version}\nnpm ${npm_version}\ninstalled ${installed_hash}\n")
endif()
require_success("${NPM}" run build)
require_success("${CMAKE_COMMAND}" -E env "EMBER_BRIDGE_TEST_BINARY=${BRIDGE}" "${NPM}" test)
