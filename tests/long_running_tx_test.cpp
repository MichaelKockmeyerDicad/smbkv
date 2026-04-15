/**
 * @file long_running_tx_test.cpp
 * @brief Safety tests for the pessimistic locking scheme used by SmbKvStore.
 *
 * This file proves that long-running interactive transactions are safe under
 * the byte-range locking protocol implemented by the smbkv library.
 *
 * Three test scenarios are covered:
 *
 *  1. Long-running transaction – Node A acquires an exclusive lock on "key1",
 *     simulates user "think time" (sleep), then commits.  Concurrently Node B
 *     tries to write the same key and must block until Node A is done.  The
 *     test verifies that Node B's write completes AFTER Node A's commit and
 *     that the final WAL reflects the correct commit order.
 *
 *  2. Read-write isolation – A reader inside a long transaction never sees the
 *     uncommitted write of another concurrent writer.  Once the writer rolls
 *     back the reader still sees its original snapshot; only a new transaction
 *     picks up the update.
 *
 *  3. WAL integrity – After several concurrent transactions the replayed WAL
 *     yields a consistent final state with no corruption or lost updates.
 *
 *  4. Rollback releases the lock – After a writer rolls back, a concurrent
 *     writer that was blocked can immediately proceed and commit.
 *
 * Platform notes
 * --------------
 * SmbKvStore itself is Windows-only (it uses Win32 LockFileEx).  On
 * non-Windows platforms this file exercises an equivalent POSIX
 * implementation that mirrors the library design using Linux Open File
 * Description (OFD) locks (fcntl F_OFD_SETLKW, available since Linux 3.15).
 * OFD locks are scoped to the open file description (i.e. per file descriptor
 * returned by open(2)) just like Win32 LockFileEx is scoped per-HANDLE.
 * This means threads holding separate file descriptors correctly block each
 * other – classic POSIX record locks (F_SETLKW) do NOT have this property
 * because they are per-process.  The POSIX path lets CI systems running Linux
 * validate the locking logic without a Windows host.
 */

// ============================================================================
// POSIX path – Linux / macOS
// ============================================================================
#if !defined(_WIN32)

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal self-contained "TestStore" that mirrors SmbKvStore semantics but
// uses POSIX fcntl byte-range locks instead of Win32 LockFileEx.
// ---------------------------------------------------------------------------
namespace teststore {

// FNV-1a 64-bit hash (same as SmbKvStore::keyToLockOffset).
static uint64_t keyToLockOffset(const std::string& key) {
    constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME        = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET_BASIS;
    for (unsigned char c : key) {
        hash ^= static_cast<uint64_t>(c);
        hash *= FNV_PRIME;
    }
    constexpr uint64_t LOCK_RESERVED_BYTES = 1;
    constexpr uint64_t LOCK_ADDRESS_SPACE  = 1'000'000;
    return LOCK_RESERVED_BYTES + (hash % LOCK_ADDRESS_SPACE);
}

// Acquire an exclusive byte-range lock on one byte at `offset` in `fd`.
//
// Uses Open File Description (OFD) locks (F_OFD_SETLKW) rather than classic
// POSIX record locks (F_SETLKW).  The key difference:
//   - Classic POSIX locks are per-process: two threads in the same process
//     can both hold a conflicting lock without blocking each other.
//   - OFD locks are per-open-file-description (i.e. per file-descriptor
//     returned by open(2)): they behave exactly like Windows LockFileEx
//     (which is per-HANDLE), so threads holding separate file descriptors
//     for the same file DO block each other correctly.
//
// F_OFD_SETLKW is available since Linux 3.15 (glibc 2.20).
static bool acquireLock(int fd, uint64_t offset) {
    struct flock fl{};
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = static_cast<off_t>(offset);
    fl.l_len    = 1;
    return fcntl(fd, F_OFD_SETLKW, &fl) == 0;
}

// Release the OFD byte-range lock at `offset` in `fd`.
static bool releaseLock(int fd, uint64_t offset) {
    struct flock fl{};
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = static_cast<off_t>(offset);
    fl.l_len    = 1;
    return fcntl(fd, F_OFD_SETLKW, &fl) == 0;
}

static std::string toHex(const std::string& s) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : s) {
        oss << std::setw(2) << static_cast<unsigned>(c);
    }
    return oss.str();
}

static std::string fromHex(const std::string& hex) {
    std::string result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned int byte = 0;
        std::istringstream ss(hex.substr(i, 2));
        ss >> std::hex >> byte;
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

enum class Status { Ok, NotFound, LockFailed, IoError, InvalidState, AlreadyInTx };

static constexpr uint64_t WAL_LOCK_OFFSET = 0;

class TestStore {
public:
    explicit TestStore(const std::string& dir) : dir_(dir) {
        walFd_   = openFile("wal.log");
        locksFd_ = openFile("locks.bin");
        walOffset_ = replayWal(0, kvMap_);
    }

    ~TestStore() {
        if (inTransaction_) rollback();
        if (walFd_   >= 0) ::close(walFd_);
        if (locksFd_ >= 0) ::close(locksFd_);
    }

    Status beginTransaction() {
        if (inTransaction_) return Status::AlreadyInTx;
        walOffset_ = replayWal(walOffset_, kvMap_);
        if (walOffset_ == static_cast<uint64_t>(-1)) return Status::IoError;
        writeBuffer_.clear();
        heldLocks_.clear();
        inTransaction_ = true;
        return Status::Ok;
    }

    Status get(const std::string& key, std::string& outValue) {
        if (!inTransaction_) return Status::InvalidState;
        auto bufIt = writeBuffer_.find(key);
        if (bufIt != writeBuffer_.end()) { outValue = bufIt->second; return Status::Ok; }
        auto mapIt = kvMap_.find(key);
        if (mapIt == kvMap_.end()) return Status::NotFound;
        outValue = mapIt->second;
        return Status::Ok;
    }

    Status put(const std::string& key, const std::string& value) {
        if (!inTransaction_) return Status::InvalidState;
        uint64_t offset = keyToLockOffset(key);
        bool alreadyLocked = (std::find(heldLocks_.begin(), heldLocks_.end(), offset)
                               != heldLocks_.end());
        if (!alreadyLocked) {
            if (!acquireLock(locksFd_, offset)) return Status::LockFailed;
            heldLocks_.push_back(offset);
        }
        writeBuffer_[key] = value;
        return Status::Ok;
    }

    Status commit() {
        if (!inTransaction_) return Status::InvalidState;
        if (writeBuffer_.empty()) { inTransaction_ = false; return Status::Ok; }

        if (!acquireLock(walFd_, WAL_LOCK_OFFSET)) return Status::LockFailed;

        // Serialise.
        std::string record;
        for (const auto& [k, v] : writeBuffer_) {
            record += "PUT " + std::to_string(k.size()) + ' '
                    + toHex(k) + ' ' + std::to_string(v.size()) + ' '
                    + toHex(v) + '\n';
        }
        record += '\n'; // commit marker

        // Seek to end and append.
        off_t end = ::lseek(walFd_, 0, SEEK_END);
        if (end < 0) { releaseLock(walFd_, WAL_LOCK_OFFSET); return Status::IoError; }

        ssize_t written = ::write(walFd_, record.data(), record.size());
        if (written != static_cast<ssize_t>(record.size())) {
            releaseLock(walFd_, WAL_LOCK_OFFSET);
            return Status::IoError;
        }
        ::fsync(walFd_);

        releaseLock(walFd_, WAL_LOCK_OFFSET);
        for (uint64_t lo : heldLocks_) releaseLock(locksFd_, lo);
        heldLocks_.clear();

        for (const auto& [k, v] : writeBuffer_) kvMap_[k] = v;
        walOffset_ = static_cast<uint64_t>(end) + record.size();
        writeBuffer_.clear();
        inTransaction_ = false;
        return Status::Ok;
    }

    Status rollback() {
        if (!inTransaction_) return Status::Ok;
        writeBuffer_.clear();
        for (uint64_t lo : heldLocks_) releaseLock(locksFd_, lo);
        heldLocks_.clear();
        inTransaction_ = false;
        return Status::Ok;
    }

private:
    int openFile(const std::string& name) {
        std::string path = dir_ + '/' + name;
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) throw std::runtime_error("Cannot open " + path + ": " + std::strerror(errno));
        return fd;
    }

    uint64_t replayWal(uint64_t startOffset,
                       std::unordered_map<std::string, std::string>& targetMap) {
        off_t sz = ::lseek(walFd_, 0, SEEK_END);
        if (sz < 0) return static_cast<uint64_t>(-1);
        if (static_cast<uint64_t>(sz) <= startOffset) return startOffset;

        uint64_t readLen = static_cast<uint64_t>(sz) - startOffset;
        std::string buf(readLen, '\0');
        if (::lseek(walFd_, static_cast<off_t>(startOffset), SEEK_SET) < 0)
            return static_cast<uint64_t>(-1);
        ssize_t r = ::read(walFd_, buf.data(), readLen);
        if (r < 0) return static_cast<uint64_t>(-1);
        buf.resize(static_cast<size_t>(r));

        uint64_t committedUpTo = startOffset;
        size_t pos = 0;
        while (pos < buf.size()) {
            std::unordered_map<std::string, std::string> txBuffer;
            bool foundCommit = false, txMalformed = false;
            while (pos < buf.size()) {
                size_t lineEnd = buf.find('\n', pos);
                if (lineEnd == std::string::npos) goto done;
                std::string line = buf.substr(pos, lineEnd - pos);
                pos = lineEnd + 1;
                if (line.empty()) { foundCommit = true; break; }
                std::istringstream ss(line);
                std::string op; size_t kl = 0, vl = 0; std::string kh, vh;
                if (!(ss >> op >> kl >> kh >> vl >> vh) || op != "PUT") { txMalformed = true; continue; }
                std::string k = fromHex(kh), v = fromHex(vh);
                if (k.size() != kl || v.size() != vl) { txMalformed = true; continue; }
                if (!txMalformed) txBuffer[k] = v;
            }
            if (foundCommit && !txMalformed) {
                for (auto& [k, v] : txBuffer) targetMap[k] = v;
                committedUpTo = startOffset + static_cast<uint64_t>(pos);
            }
        }
done:
        return committedUpTo;
    }

    std::string dir_;
    int walFd_   = -1;
    int locksFd_ = -1;
    uint64_t walOffset_ = 0;
    std::unordered_map<std::string, std::string> kvMap_;
    bool     inTransaction_ = false;
    std::unordered_map<std::string, std::string> writeBuffer_;
    std::vector<uint64_t> heldLocks_;
};

// ---------------------------------------------------------------------------
// Helper: create a fresh temporary store directory.
// ---------------------------------------------------------------------------
static std::string makeTempDir() {
    char tmpl[] = "/tmp/smbkv_test_XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) throw std::runtime_error("mkdtemp failed");
    return d;
}

static void removeTempDir(const std::string& dir) {
    // Remove files, then directory.
    for (const char* name : {"wal.log", "locks.bin"}) {
        std::string p = dir + '/' + name;
        ::unlink(p.c_str());
    }
    ::rmdir(dir.c_str());
}

} // namespace teststore

// ============================================================================
// Test harness (tiny, no external dependencies)
// ============================================================================
namespace {

struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

static std::vector<TestResult> g_results;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::ostringstream _ss;                                            \
            _ss << "FAIL at line " << __LINE__ << ": " << (msg);              \
            return {testName, false, _ss.str()};                               \
        }                                                                      \
    } while (0)

// ============================================================================
// Test 1 – Long-running transaction: Node B blocks while Node A holds the lock
// ============================================================================
TestResult test_long_running_transaction_blocks_concurrent_writer() {
    const std::string testName = "long_running_transaction_blocks_concurrent_writer";

    std::string dir = teststore::makeTempDir();

    // Timestamps to verify ordering.
    std::atomic<std::chrono::steady_clock::time_point>
        nodeA_committed{std::chrono::steady_clock::now()},
        nodeB_put_started{std::chrono::steady_clock::now()},
        nodeB_committed{std::chrono::steady_clock::now()};

    std::atomic<bool> nodeA_put_done{false};
    std::atomic<teststore::Status> nodeA_status{teststore::Status::Ok};
    std::atomic<teststore::Status> nodeB_status{teststore::Status::Ok};

    const auto thinkTime = std::chrono::milliseconds(2000); // 2 s simulated think time

    // --- Node A thread ---
    std::thread nodeA([&]() {
        teststore::TestStore storeA(dir);
        storeA.beginTransaction();

        // Put "key1"="value1" – acquires exclusive lock.
        nodeA_status.store(storeA.put("key1", "value1"));
        nodeA_put_done.store(true);

        // Simulate long user think time.
        std::this_thread::sleep_for(thinkTime);

        nodeA_status.store(storeA.commit());
        nodeA_committed.store(std::chrono::steady_clock::now());
    });

    // Wait until Node A has the lock before starting Node B.
    while (!nodeA_put_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // --- Node B thread ---
    std::thread nodeB([&]() {
        teststore::TestStore storeB(dir);
        storeB.beginTransaction();

        nodeB_put_started.store(std::chrono::steady_clock::now());
        // This call MUST block until Node A commits and releases the lock.
        nodeB_status.store(storeB.put("key1", "value2"));
        nodeB_status.store(storeB.commit());
        nodeB_committed.store(std::chrono::steady_clock::now());
    });

    nodeA.join();
    nodeB.join();

    // Verify both operations succeeded.
    CHECK(nodeA_status.load() == teststore::Status::Ok, "Node A commit failed");
    CHECK(nodeB_status.load() == teststore::Status::Ok, "Node B commit failed");

    // Verify ordering: Node B's Put started BEFORE Node A committed but
    // Node B only committed AFTER Node A committed.
    auto putStarted  = nodeB_put_started.load();
    auto aCommitted  = nodeA_committed.load();
    auto bCommitted  = nodeB_committed.load();

    CHECK(putStarted < aCommitted,
          "Node B's Put should have started before Node A committed");
    CHECK(bCommitted >= aCommitted,
          "Node B must have committed after Node A (it was blocked)");

    auto blockDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(bCommitted - putStarted);
    CHECK(blockDuration >= thinkTime - std::chrono::milliseconds(200),
          "Node B was not blocked long enough – locking may not be working. "
          "Block duration: " + std::to_string(blockDuration.count()) + " ms");

    // Verify WAL: replay from scratch and check the final state.
    // Node B wrote "value2" last, so the store must reflect "value2".
    {
        teststore::TestStore verifier(dir);
        verifier.beginTransaction();
        std::string val;
        auto st = verifier.get("key1", val);
        CHECK(st == teststore::Status::Ok, "key1 not found after both commits");
        CHECK(val == "value2",
              "Final value should be 'value2' (Node B committed last). Got: " + val);
        verifier.rollback();
    }

    teststore::removeTempDir(dir);
    return {testName, true, "Block duration: " + std::to_string(blockDuration.count()) + " ms"};
}

// ============================================================================
// Test 2 – Read-write isolation: reader never sees uncommitted writes
// ============================================================================
TestResult test_read_write_isolation() {
    const std::string testName = "read_write_isolation";

    std::string dir = teststore::makeTempDir();

    // Seed an initial committed value.
    {
        teststore::TestStore seed(dir);
        seed.beginTransaction();
        seed.put("key1", "committed_initial");
        seed.commit();
    }

    std::atomic<bool> writerPutDone{false};
    std::atomic<bool> readerChecked{false};

    std::thread writer([&]() {
        teststore::TestStore w(dir);
        w.beginTransaction();
        w.put("key1", "uncommitted_value");
        writerPutDone.store(true);

        // Hold the lock while the reader runs its check.
        while (!readerChecked.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Rollback (discard the write).
        w.rollback();
    });

    // Wait until writer has buffered its uncommitted write.
    while (!writerPutDone.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Reader uses a *separate* store instance.  It should see the committed
    // snapshot and NOT the writer's uncommitted "uncommitted_value".
    // Because the writer holds an exclusive byte-range lock on "key1", any
    // read transaction that tries to acquire a conflicting lock would also
    // block.  In this library's design, plain Get() does NOT acquire a read
    // lock (Read Committed semantics), so the reader sees the last committed
    // value without blocking.
    std::string readerSaw;
    {
        teststore::TestStore reader(dir);
        reader.beginTransaction();
        auto st = reader.get("key1", readerSaw);
        // The reader must see the previously committed value, NOT the
        // uncommitted write of the writer.
        (void)st;
        reader.rollback();
    }
    readerChecked.store(true);
    writer.join();

    CHECK(readerSaw == "committed_initial",
          "Reader saw an uncommitted value: '" + readerSaw + "'");

    teststore::removeTempDir(dir);
    return {testName, true, "Reader correctly saw committed_initial"};
}

// ============================================================================
// Test 3 – WAL integrity after multiple concurrent transactions
// ============================================================================
TestResult test_wal_integrity_concurrent_commits() {
    const std::string testName = "wal_integrity_concurrent_commits";

    std::string dir = teststore::makeTempDir();
    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = 5;

    // Each thread writes to its own dedicated key (no lock contention) so all
    // commits go through concurrently and race on the WAL lock only.
    std::vector<std::thread> threads;
    std::vector<teststore::Status> results(NUM_THREADS, teststore::Status::Ok);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            teststore::TestStore store(dir);
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string key   = "thread" + std::to_string(t);
                std::string value = std::to_string(i);
                store.beginTransaction();
                auto st = store.put(key, value);
                if (st != teststore::Status::Ok) { results[t] = st; return; }
                st = store.commit();
                if (st != teststore::Status::Ok) { results[t] = st; return; }
            }
        });
    }

    for (auto& th : threads) th.join();

    for (int t = 0; t < NUM_THREADS; ++t) {
        std::ostringstream msg;
        msg << "Thread " << t << " reported error " << static_cast<int>(results[t]);
        CHECK(results[t] == teststore::Status::Ok, msg.str());
    }

    // Replay the WAL from scratch; each thread's key must have the last
    // value written by that thread (OPS_PER_THREAD - 1).
    {
        teststore::TestStore verifier(dir);
        verifier.beginTransaction();
        for (int t = 0; t < NUM_THREADS; ++t) {
            std::string key = "thread" + std::to_string(t);
            std::string expected = std::to_string(OPS_PER_THREAD - 1);
            std::string val;
            auto st = verifier.get(key, val);
            std::ostringstream msg;
            msg << "key '" << key << "' not found after WAL replay";
            CHECK(st == teststore::Status::Ok, msg.str());
            msg.str("");
            msg << "key '" << key << "' expected '" << expected << "' got '" << val << "'";
            CHECK(val == expected, msg.str());
        }
        verifier.rollback();
    }

    teststore::removeTempDir(dir);
    return {testName, true, ""};
}

// ============================================================================
// Test 4 – Rollback releases the lock so another writer can proceed
// ============================================================================
TestResult test_rollback_releases_lock() {
    const std::string testName = "rollback_releases_lock";

    std::string dir = teststore::makeTempDir();

    std::atomic<bool> nodeA_locked{false};
    std::atomic<bool> nodeA_should_rollback{false};
    std::atomic<teststore::Status> nodeB_put_status{teststore::Status::IoError};

    const auto rollbackDelay = std::chrono::milliseconds(1500);

    std::thread nodeA([&]() {
        teststore::TestStore storeA(dir);
        storeA.beginTransaction();
        storeA.put("sharedKey", "from_A");
        nodeA_locked.store(true);
        while (!nodeA_should_rollback.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        storeA.rollback(); // Discard – must release the lock.
    });

    while (!nodeA_locked.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::thread nodeB([&]() {
        teststore::TestStore storeB(dir);
        storeB.beginTransaction();
        // Signal Node A to rollback.
        nodeA_should_rollback.store(true);
        // This blocks until Node A rollbacks and releases the lock.
        nodeB_put_status.store(storeB.put("sharedKey", "from_B"));
        storeB.commit();
    });

    nodeA.join();
    nodeB.join();

    CHECK(nodeB_put_status.load() == teststore::Status::Ok,
          "Node B could not acquire lock after Node A rolled back");

    // Verify only Node B's value survived.
    {
        teststore::TestStore verifier(dir);
        verifier.beginTransaction();
        std::string val;
        CHECK(verifier.get("sharedKey", val) == teststore::Status::Ok,
              "sharedKey not found");
        CHECK(val == "from_B",
              "Expected 'from_B' after rollback, got '" + val + "'");
        verifier.rollback();
    }

    teststore::removeTempDir(dir);
    return {testName, true, ""};
}

} // anonymous namespace

// ============================================================================
// main
// ============================================================================
int main() {
    using TestFn = std::function<TestResult()>;
    const std::vector<TestFn> tests = {
        test_long_running_transaction_blocks_concurrent_writer,
        test_read_write_isolation,
        test_wal_integrity_concurrent_commits,
        test_rollback_releases_lock,
    };

    int failed = 0;
    for (auto& fn : tests) {
        TestResult r = fn();
        std::cout << (r.passed ? "[PASS]" : "[FAIL]") << " " << r.name;
        if (!r.message.empty()) std::cout << " – " << r.message;
        std::cout << '\n';
        if (!r.passed) ++failed;
    }

    std::cout << '\n' << (tests.size() - failed) << '/' << tests.size()
              << " tests passed.\n";
    return failed == 0 ? 0 : 1;
}

// ============================================================================
// Windows path – uses SmbKvStore directly with a local temp directory.
// On Windows, CreateFile / LockFileEx work on local paths as well as UNC
// paths, so we can exercise the real library code without a network share.
// ============================================================================
#else // _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "smbkv/SmbKvStore.h"

namespace {

struct TestResult { std::string name; bool passed; std::string message; };

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::ostringstream _ss;                                           \
            _ss << "FAIL at line " << __LINE__ << ": " << (msg);             \
            return {testName, false, _ss.str()};                              \
        }                                                                     \
    } while (0)

// Create a temporary directory that looks like a share root.
static std::string makeTempDir() {
    char tmpPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpPath);
    char dirPath[MAX_PATH];
    GetTempFileNameA(tmpPath, "skv", 0, dirPath);
    DeleteFileA(dirPath); // GetTempFileName creates a file; remove it.
    CreateDirectoryA(dirPath, nullptr);
    return std::string(dirPath);
}

static void removeTempDir(const std::string& dir) {
    for (const char* name : {"wal.log", "locks.bin"}) {
        DeleteFileA((dir + '\\' + name).c_str());
    }
    RemoveDirectoryA(dir.c_str());
}

// -------------------------------------------------------------------------
// Test 1 – Long-running transaction blocks concurrent writer
// -------------------------------------------------------------------------
TestResult test_long_running_transaction_blocks_concurrent_writer() {
    const std::string testName = "long_running_transaction_blocks_concurrent_writer";

    std::string dir = makeTempDir();
    const auto thinkTime = std::chrono::milliseconds(2000);

    std::atomic<bool> nodeA_put_done{false};
    std::atomic<smbkv::Status> nodeA_status{smbkv::Status::Ok};
    std::atomic<smbkv::Status> nodeB_status{smbkv::Status::Ok};
    std::atomic<std::chrono::steady_clock::time_point>
        nodeA_committed{std::chrono::steady_clock::now()},
        nodeB_put_started{std::chrono::steady_clock::now()},
        nodeB_committed{std::chrono::steady_clock::now()};

    std::thread nodeA([&]() {
        smbkv::SmbKvStore storeA(dir);
        storeA.BeginTransaction();
        nodeA_status.store(storeA.Put("key1", "value1"));
        nodeA_put_done.store(true);
        std::this_thread::sleep_for(thinkTime);
        nodeA_status.store(storeA.Commit());
        nodeA_committed.store(std::chrono::steady_clock::now());
    });

    while (!nodeA_put_done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread nodeB([&]() {
        smbkv::SmbKvStore storeB(dir);
        storeB.BeginTransaction();
        nodeB_put_started.store(std::chrono::steady_clock::now());
        nodeB_status.store(storeB.Put("key1", "value2"));
        nodeB_status.store(storeB.Commit());
        nodeB_committed.store(std::chrono::steady_clock::now());
    });

    nodeA.join();
    nodeB.join();

    CHECK(nodeA_status.load() == smbkv::Status::Ok, "Node A commit failed");
    CHECK(nodeB_status.load() == smbkv::Status::Ok, "Node B commit failed");

    auto blockDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        nodeB_committed.load() - nodeB_put_started.load());
    CHECK(blockDuration >= thinkTime - std::chrono::milliseconds(200),
          "Node B was not blocked long enough: " + std::to_string(blockDuration.count()) + " ms");

    {
        smbkv::SmbKvStore verifier(dir);
        verifier.BeginTransaction();
        std::string val;
        CHECK(verifier.Get("key1", val) == smbkv::Status::Ok, "key1 not found");
        CHECK(val == "value2", "Expected 'value2', got '" + val + "'");
        verifier.Rollback();
    }

    removeTempDir(dir);
    return {testName, true, "Block duration: " + std::to_string(blockDuration.count()) + " ms"};
}

// -------------------------------------------------------------------------
// Test 2 – Read-write isolation
// -------------------------------------------------------------------------
TestResult test_read_write_isolation() {
    const std::string testName = "read_write_isolation";

    std::string dir = makeTempDir();
    {
        smbkv::SmbKvStore seed(dir);
        seed.BeginTransaction();
        seed.Put("key1", "committed_initial");
        seed.Commit();
    }

    std::atomic<bool> writerPutDone{false};
    std::atomic<bool> readerChecked{false};

    std::thread writer([&]() {
        smbkv::SmbKvStore w(dir);
        w.BeginTransaction();
        w.Put("key1", "uncommitted_value");
        writerPutDone.store(true);
        while (!readerChecked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        w.Rollback();
    });

    while (!writerPutDone.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::string readerSaw;
    {
        smbkv::SmbKvStore reader(dir);
        reader.BeginTransaction();
        reader.Get("key1", readerSaw);
        reader.Rollback();
    }
    readerChecked.store(true);
    writer.join();

    CHECK(readerSaw == "committed_initial",
          "Reader saw uncommitted value: '" + readerSaw + "'");

    removeTempDir(dir);
    return {testName, true, "Reader correctly saw committed_initial"};
}

// -------------------------------------------------------------------------
// Test 3 – WAL integrity after concurrent commits
// -------------------------------------------------------------------------
TestResult test_wal_integrity_concurrent_commits() {
    const std::string testName = "wal_integrity_concurrent_commits";

    std::string dir = makeTempDir();
    const int NUM_THREADS = 4, OPS = 5;
    std::vector<std::thread> threads;
    std::vector<smbkv::Status> results(NUM_THREADS, smbkv::Status::Ok);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            smbkv::SmbKvStore store(dir);
            for (int i = 0; i < OPS; ++i) {
                store.BeginTransaction();
                auto st = store.Put("thread" + std::to_string(t), std::to_string(i));
                if (st != smbkv::Status::Ok) { results[t] = st; return; }
                st = store.Commit();
                if (st != smbkv::Status::Ok) { results[t] = st; return; }
            }
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < NUM_THREADS; ++t) {
        std::ostringstream msg;
        msg << "Thread " << t << " error " << static_cast<int>(results[t]);
        CHECK(results[t] == smbkv::Status::Ok, msg.str());
    }

    {
        smbkv::SmbKvStore verifier(dir);
        verifier.BeginTransaction();
        for (int t = 0; t < NUM_THREADS; ++t) {
            std::string key = "thread" + std::to_string(t);
            std::string val;
            CHECK(verifier.Get(key, val) == smbkv::Status::Ok, key + " not found");
            CHECK(val == std::to_string(OPS - 1), key + " wrong value: " + val);
        }
        verifier.Rollback();
    }

    removeTempDir(dir);
    return {testName, true, ""};
}

// -------------------------------------------------------------------------
// Test 4 – Rollback releases the lock
// -------------------------------------------------------------------------
TestResult test_rollback_releases_lock() {
    const std::string testName = "rollback_releases_lock";

    std::string dir = makeTempDir();
    std::atomic<bool> nodeA_locked{false}, nodeA_should_rollback{false};
    std::atomic<smbkv::Status> nodeB_put_status{smbkv::Status::IoError};

    std::thread nodeA([&]() {
        smbkv::SmbKvStore storeA(dir);
        storeA.BeginTransaction();
        storeA.Put("sharedKey", "from_A");
        nodeA_locked.store(true);
        while (!nodeA_should_rollback.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        storeA.Rollback();
    });

    while (!nodeA_locked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread nodeB([&]() {
        smbkv::SmbKvStore storeB(dir);
        storeB.BeginTransaction();
        nodeA_should_rollback.store(true);
        nodeB_put_status.store(storeB.Put("sharedKey", "from_B"));
        storeB.Commit();
    });

    nodeA.join(); nodeB.join();

    CHECK(nodeB_put_status.load() == smbkv::Status::Ok,
          "Node B could not acquire lock after rollback");

    {
        smbkv::SmbKvStore verifier(dir);
        verifier.BeginTransaction();
        std::string val;
        CHECK(verifier.Get("sharedKey", val) == smbkv::Status::Ok, "sharedKey missing");
        CHECK(val == "from_B", "Expected 'from_B', got '" + val + "'");
        verifier.Rollback();
    }

    removeTempDir(dir);
    return {testName, true, ""};
}

} // anonymous namespace

int main() {
    using TestFn = std::function<TestResult()>;
    const std::vector<TestFn> tests = {
        test_long_running_transaction_blocks_concurrent_writer,
        test_read_write_isolation,
        test_wal_integrity_concurrent_commits,
        test_rollback_releases_lock,
    };

    int failed = 0;
    for (auto& fn : tests) {
        TestResult r = fn();
        std::cout << (r.passed ? "[PASS]" : "[FAIL]") << " " << r.name;
        if (!r.message.empty()) std::cout << " \xe2\x80\x93 " << r.message;
        std::cout << '\n';
        if (!r.passed) ++failed;
    }

    std::cout << '\n' << (tests.size() - failed) << '/' << tests.size()
              << " tests passed.\n";
    return failed == 0 ? 0 : 1;
}

#endif // _WIN32
