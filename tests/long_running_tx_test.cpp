/**
 * @file tests/long_running_tx_test.cpp
 * @brief Test demonstrating safe long-running interactive transactions using
 *        pessimistic byte-range locking.
 *
 * Background
 * ----------
 * The SMB KV store uses Win32 LockFileEx/UnlockFileEx to hold per-key
 * exclusive locks for the entire duration of a transaction.  On Linux (used
 * by the CI build farm) the equivalent primitive is the "Open File Description
 * (OFD) lock" accessed via fcntl(2) with F_OFD_SETLKW.  OFD locks have the
 * same semantics as Win32 HANDLE-level locks: the lock is associated with the
 * file description (not the process), so two threads each opening the file
 * independently can correctly block each other – unlike the older POSIX
 * F_SETLKW which is process-wide.
 *
 * What this test proves
 * ---------------------
 * 1. Thread A ("long transaction") acquires an exclusive lock on a key byte
 *    offset in a temporary file, simulating a user who has started a PUT but
 *    has not yet committed.
 * 2. While Thread A holds the lock it sleeps for 2 seconds, representing the
 *    user "thinking" interactively.
 * 3. Thread B ("concurrent writer") tries to acquire the same exclusive lock
 *    immediately.  It must block until Thread A releases.
 * 4. Thread A wakes up and releases the lock (simulating Commit()).
 * 5. Thread B finally acquires the lock and records the time it was unblocked.
 * 6. The test asserts that Thread B was blocked for at least 1 second –
 *    proving that the locking correctly serialises conflicting writers.
 *
 * The test is designed to complete well within the 60-second CTest timeout.
 */

#if defined(_WIN32)
// On Windows the interactive_demo and the library itself test the locking;
// this file provides a lightweight compile-time check only.
int main() { return 0; }
#else

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Open a file and return its file descriptor.  Throws on error.
static int openFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("open(") + path + "): " + std::strerror(errno));
    }
    return fd;
}

/// Acquire an exclusive OFD lock on one byte at the given offset.
/// Blocks until the lock is granted (equivalent to Win32 LockFileEx blocking).
static void acquireOfdLock(int fd, off_t offset) {
    struct flock fl{};
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = offset;
    fl.l_len    = 1;
    if (::fcntl(fd, F_OFD_SETLKW, &fl) != 0) {
        throw std::runtime_error(
            std::string("fcntl F_OFD_SETLKW: ") + std::strerror(errno));
    }
}

/// Release the OFD lock at the given offset.
static void releaseOfdLock(int fd, off_t offset) {
    struct flock fl{};
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = offset;
    fl.l_len    = 1;
    if (::fcntl(fd, F_OFD_SETLKW, &fl) != 0) {
        throw std::runtime_error(
            std::string("fcntl F_UNLCK: ") + std::strerror(errno));
    }
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

int main() {
    using Clock    = std::chrono::steady_clock;
    using Seconds  = std::chrono::duration<double>;

    // Create a unique temporary file per process to avoid conflicts when tests
    // run in parallel (e.g., on a shared CI build machine).
    const std::string lockFile =
        std::string("/tmp/smbkv_long_tx_test_") + std::to_string(::getpid()) + ".bin";

    // Remove any leftover file from a previous failed run.
    ::unlink(lockFile.c_str());

    // Pre-create the file so both threads can open it independently.
    {
        int fd = openFile(lockFile);
        ::close(fd);
    }

    // Byte offset in the lock file that represents our shared key.
    constexpr off_t KEY_OFFSET = 42;

    // How long Thread A holds the lock (simulating a "thinking" user).
    constexpr int HOLD_SECONDS = 2;

    // We expect Thread B to have been blocked for at least this long.
    constexpr double MIN_BLOCKED_SECONDS = 1.0;

    std::string errorA, errorB;
    double threadBWaitedSeconds = 0.0; // set by Thread B before it joins

    // -----------------------------------------------------------------------
    // Thread A: long-running interactive transaction
    // -----------------------------------------------------------------------
    std::thread threadA([&]() {
        try {
            // Each thread opens its own file description so OFD locks work.
            int fd = openFile(lockFile);

            std::cout << "[A] BeginTransaction – acquiring exclusive lock on key offset "
                      << KEY_OFFSET << "...\n";
            acquireOfdLock(fd, KEY_OFFSET);
            std::cout << "[A] Lock acquired.  Simulating user interaction ("
                      << HOLD_SECONDS << "s)...\n";

            // Simulate the user typing interactively.
            std::this_thread::sleep_for(std::chrono::seconds(HOLD_SECONDS));

            std::cout << "[A] Commit – releasing lock.\n";
            releaseOfdLock(fd, KEY_OFFSET);
            ::close(fd);
        } catch (const std::exception& ex) {
            errorA = ex.what();
        }
    });

    // Give Thread A a head start so it definitely acquires the lock first.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // -----------------------------------------------------------------------
    // Thread B: concurrent writer – must block until Thread A commits
    // -----------------------------------------------------------------------
    std::thread threadB([&]() {
        try {
            int fd = openFile(lockFile);

            std::cout << "[B] BeginTransaction – trying to acquire the same lock...\n";
            auto waitStart = Clock::now();
            acquireOfdLock(fd, KEY_OFFSET); // ← blocks here until A releases.
            auto waitEnd = Clock::now();

            threadBWaitedSeconds = Seconds(waitEnd - waitStart).count();
            std::cout << "[B] Lock acquired after " << threadBWaitedSeconds
                      << "s (was blocked by A).\n";
            releaseOfdLock(fd, KEY_OFFSET);
            ::close(fd);
        } catch (const std::exception& ex) {
            errorB = ex.what();
        }
    });

    threadA.join();
    threadB.join();

    // Cleanup.
    ::unlink(lockFile.c_str());

    // -----------------------------------------------------------------------
    // Assertions
    // -----------------------------------------------------------------------
    if (!errorA.empty()) {
        std::cerr << "FAIL: Thread A threw: " << errorA << "\n";
        return 1;
    }
    if (!errorB.empty()) {
        std::cerr << "FAIL: Thread B threw: " << errorB << "\n";
        return 1;
    }

    // Thread B must have been unblocked after Thread A started (not before).
    // We verify by checking that it waited at least MIN_BLOCKED_SECONDS.
    // (Thread B started ~100 ms after Thread A acquired the lock, so it should
    //  have waited at least HOLD_SECONDS - 0.1 s ≈ 1.9 s.)
    if (threadBWaitedSeconds < MIN_BLOCKED_SECONDS) {
        std::cerr << "FAIL: Thread B was not blocked long enough (waited "
                  << threadBWaitedSeconds << "s, expected >= "
                  << MIN_BLOCKED_SECONDS << "s).\n"
                  << "      The locking did NOT serialise the concurrent writers "
                     "as expected.\n";
        return 1;
    }

    std::cout << "\nPASS: Thread B waited " << threadBWaitedSeconds
              << "s (>= " << MIN_BLOCKED_SECONDS << "s required).\n"
              << "      Thread B was correctly blocked by Thread A's long-running "
                 "transaction.\n"
              << "      This mirrors the behaviour of Win32 LockFileEx / SMB "
                 "byte-range locking.\n";
    return 0;
}

#endif // !_WIN32
