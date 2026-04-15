cmake_minimum_required(VERSION 3.16)
project(smbkv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(smbkv
    src/SmbKvStore.cpp
)

target_include_directories(smbkv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Windows-specific: link against kernel32 (provides LockFileEx etc.)
if(WIN32)
    target_link_libraries(smbkv PUBLIC kernel32)
endif()

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(smbkv_example examples/main.cpp)
target_link_libraries(smbkv_example PRIVATE smbkv)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(long_running_tx_test tests/long_running_tx_test.cpp)

# On Windows the test uses SmbKvStore directly; on non-Windows it is
# self-contained (POSIX implementation) but still links smbkv so the build
# validates that the library compiles correctly on all platforms.
target_link_libraries(long_running_tx_test PRIVATE smbkv)

if(UNIX)
    target_link_libraries(long_running_tx_test PRIVATE pthread)
endif()

add_test(NAME long_running_tx_test COMMAND long_running_tx_test)
# Allow up to 60 s for the long-running transaction tests (they sleep 2 s
# per scenario by design).
set_tests_properties(long_running_tx_test PROPERTIES TIMEOUT 60)

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS smbkv
    EXPORT smbkvTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT smbkvTargets
    FILE smbkvTargets.cmake
    NAMESPACE smbkv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/smbkv
)
