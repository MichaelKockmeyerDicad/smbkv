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
# Example executables
# ---------------------------------------------------------------------------
add_executable(smbkv_example examples/main.cpp)
target_link_libraries(smbkv_example PRIVATE smbkv)

# Interactive console demo: demonstrates long-running interactive transactions
# where the user can PUT/GET keys while locks are held, then type 'end' to
# commit and auto-verify all written keys.
add_executable(smbkv_interactive examples/interactive_demo.cpp)
target_link_libraries(smbkv_interactive PRIVATE smbkv)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

# Long-running transaction test: demonstrates that a concurrent writer is
# correctly blocked while another transaction holds the lock.  Uses Linux OFD
# locks (fcntl F_OFD_SETLKW) as the Linux equivalent of Win32 LockFileEx.
add_executable(long_running_tx_test tests/long_running_tx_test.cpp)
target_link_libraries(long_running_tx_test PRIVATE smbkv)

add_test(NAME long_running_tx_test COMMAND long_running_tx_test)
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
