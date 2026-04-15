#pragma once

/**
 * @file SmbKvStore.h
 * @brief Distributed Key-Value store backed by a Windows network share (SMB).
 *
 * Architecture overview
 * ---------------------
 * A central SMB share hosts two files:
 *   wal.log   – append-only Write-Ahead Log; every committed transaction is
 *               appended here as a length-prefixed, newline-delimited record.
 *   locks.bin – a dummy file used exclusively for byte-range locking via the
 *               Win32 LockFileEx / UnlockFileEx APIs.
 *
 * Each client node keeps an in-memory map that is built by replaying wal.log
 * on startup and refreshed at the beginning of every new transaction.
 *
 * Concurrency control
 * -------------------
 * Keys are mapped to 64-bit byte offsets by a FNV-1a hash.  An exclusive
 * byte-range lock on that offset in locks.bin is acquired for every key that
 * is written inside a transaction and held until Commit() / Rollback().
 * The WAL itself is protected by a separate exclusive lock on byte offset 0
 * of wal.log during the commit critical section.
 *
 * If a client crashes while holding locks, the Windows SMB server
 * automatically releases them once the underlying TCP/SMB session times out,
 * so orphaned locks are not permanent.
 *
 * Supported platforms
 * -------------------
 * Windows only (requires Win32 APIs: CreateFile, LockFileEx, UnlockFileEx,
 * ReadFile, WriteFile, FlushFileBuffers, SetFilePointer).
 */

#ifndef SMBKV_SMBKVSTORE_H
#define SMBKV_SMBKVSTORE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare the Win32 HANDLE type so that the header can be parsed by
// tools / IDEs on non-Windows platforms without pulling in <windows.h>.
#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#else
// Stub types so the header compiles on non-Windows for documentation / IDE
// inspection purposes.  The actual implementation is Windows-only.
using HANDLE  = void*;
using DWORD   = unsigned long;
using BOOL    = int;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(-1)
#endif

namespace smbkv {

/**
 * @brief Status codes returned by SmbKvStore operations.
 */
enum class Status {
    Ok = 0,           ///< Operation succeeded.
    NotFound,         ///< Key does not exist in the store.
    LockFailed,       ///< Could not acquire a required file lock.
    IoError,          ///< A file I/O operation failed.
    InvalidState,     ///< Operation is illegal in the current state
                      ///< (e.g. calling Get() before BeginTransaction()).
    AlreadyInTx,      ///< BeginTransaction() called while a transaction is
                      ///< already active.
};

/**
 * @brief Distributed Key-Value store backed by an SMB share.
 *
 * All public methods are **not** thread-safe.  If multiple threads share one
 * SmbKvStore instance, the caller is responsible for external synchronisation.
 * The typical usage pattern is one SmbKvStore per thread / per logical client.
 *
 * Typical usage
 * -------------
 * @code
 *   smbkv::SmbKvStore store(R"(\\fileserver\kvstore)");
 *   store.BeginTransaction();
 *
 *   std::string val;
 *   if (store.Get("counter", val) == smbkv::Status::Ok) {
 *       int n = std::stoi(val);
 *       store.Put("counter", std::to_string(n + 1));
 *   } else {
 *       store.Put("counter", "1");
 *   }
 *
 *   store.Commit();
 * @endcode
 */
class SmbKvStore {
public:
    /**
     * @brief Construct an SmbKvStore pointing at the given SMB share path.
     *
     * @param sharePath  UNC path to the share directory, e.g.
     *                   `R"(\\fileserver\kvstore)"`.
     *                   The directory and the two control files (wal.log and
     *                   locks.bin) must already exist and be accessible.
     *
     * The constructor opens the two control files and replays wal.log to
     * build the initial in-memory state.  It throws std::runtime_error if
     * the files cannot be opened.
     */
    explicit SmbKvStore(std::string sharePath);

    /**
     * @brief Destructor.  Rolls back any active transaction and closes all
     *        file handles.
     */
    ~SmbKvStore();

    // Non-copyable, non-movable (owns OS handles)
    SmbKvStore(const SmbKvStore&)            = delete;
    SmbKvStore& operator=(const SmbKvStore&) = delete;
    SmbKvStore(SmbKvStore&&)                 = delete;
    SmbKvStore& operator=(SmbKvStore&&)      = delete;

    // -----------------------------------------------------------------------
    // Transaction API
    // -----------------------------------------------------------------------

    /**
     * @brief Begin a new interactive transaction.
     *
     * Reads any new entries that have been appended to wal.log since the last
     * transaction and updates the in-memory state accordingly.  Records the
     * current WAL size as the "snapshot offset" to provide snapshot isolation
     * for the duration of this transaction.
     *
     * @return Status::Ok          on success.
     * @return Status::AlreadyInTx if a transaction is already active.
     * @return Status::IoError     if replaying the WAL failed.
     */
    Status BeginTransaction();

    /**
     * @brief Read the current value associated with @p key.
     *
     * Looks up the key in the local in-memory snapshot.  If the key was
     * written by the current transaction (i.e. it is in the local write
     * buffer), the buffered value is returned.
     *
     * @param[in]  key        Key to look up.
     * @param[out] outValue   Receives the value when Status::Ok is returned.
     *
     * @return Status::Ok          if the key was found.
     * @return Status::NotFound    if the key does not exist.
     * @return Status::InvalidState if no transaction is active.
     */
    Status Get(const std::string& key, std::string& outValue);

    /**
     * @brief Write @p value for @p key, holding an exclusive lock until
     *        Commit() or Rollback().
     *
     * Hashes @p key to a byte offset and acquires an exclusive byte-range
     * lock on that offset in locks.bin.  If another node already holds the
     * lock, this call **blocks** until the lock becomes available.
     *
     * @param key    Key to write.
     * @param value  Value to associate with the key.
     *
     * @return Status::Ok          on success.
     * @return Status::LockFailed  if the lock could not be acquired.
     * @return Status::InvalidState if no transaction is active.
     */
    Status Put(const std::string& key, const std::string& value);

    /**
     * @brief Atomically append the transaction buffer to wal.log and release
     *        all held locks.
     *
     * Steps performed:
     *   1. Acquire an exclusive byte-range lock on wal.log (WAL lock).
     *   2. Seek to end and append the serialised transaction record.
     *   3. Call FlushFileBuffers to guarantee durability on the SMB server.
     *   4. Release the WAL lock.
     *   5. Release all per-key locks in locks.bin.
     *   6. Apply the write buffer to the local in-memory map.
     *
     * @return Status::Ok       on success.
     * @return Status::IoError  if appending to the WAL failed.
     * @return Status::LockFailed if the WAL lock could not be acquired.
     * @return Status::InvalidState if no transaction is active.
     */
    Status Commit();

    /**
     * @brief Discard all buffered writes and release all held locks.
     *
     * @return Status::Ok          always (even if no transaction was active).
     */
    Status Rollback();

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /**
     * @brief Open (or create) a file on the share and return its HANDLE.
     *        Throws std::runtime_error on failure.
     */
    HANDLE openShareFile(const std::string& filename,
                         DWORD desiredAccess,
                         DWORD shareMode,
                         DWORD creationDisposition) const;

    /**
     * @brief Replay all WAL records from @p startOffset up to the current
     *        end of the file and apply them to @p targetMap.
     *
     * @param startOffset  Byte offset in wal.log at which to begin reading.
     * @param targetMap    Map to be updated with committed key-value pairs.
     * @return The byte offset just past the last successfully parsed record.
     */
    uint64_t replayWal(uint64_t startOffset,
                       std::unordered_map<std::string, std::string>& targetMap);

    /**
     * @brief FNV-1a 64-bit hash of @p key, clamped to the lock address space.
     *
     * The lock address space is [LOCK_RESERVED_BYTES, LOCK_RESERVED_BYTES +
     * LOCK_ADDRESS_SPACE) so that byte 0 of locks.bin can be reserved for a
     * potential global lock without colliding with per-key offsets.
     */
    static uint64_t keyToLockOffset(const std::string& key);

    /**
     * @brief Acquire an exclusive byte-range lock on one byte at @p offset
     *        in @p fileHandle.  Blocks until the lock is granted.
     */
    static bool acquireLock(HANDLE fileHandle, uint64_t offset);

    /**
     * @brief Release the byte-range lock at @p offset in @p fileHandle.
     */
    static bool releaseLock(HANDLE fileHandle, uint64_t offset);

    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------
    static constexpr uint64_t WAL_LOCK_OFFSET    = 0;
    static constexpr uint64_t LOCK_RESERVED_BYTES = 1;
    static constexpr uint64_t LOCK_ADDRESS_SPACE  = 1'000'000;

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    std::string sharePath_;        ///< UNC path to the share directory.

    HANDLE walHandle_;             ///< Handle to wal.log.
    HANDLE locksHandle_;           ///< Handle to locks.bin.

    uint64_t walOffset_;           ///< Byte offset up to which the WAL has
                                   ///< already been replayed into kvMap_.

    std::unordered_map<std::string, std::string> kvMap_;  ///< In-memory state.

    // Per-transaction state
    bool inTransaction_;           ///< True while a transaction is active.
    uint64_t snapshotOffset_;      ///< WAL size at BeginTransaction() time.

    std::unordered_map<std::string, std::string> writeBuffer_; ///< Pending writes.
    std::vector<uint64_t> heldLocks_;  ///< Lock offsets currently held.
};

} // namespace smbkv

#endif // SMBKV_SMBKVSTORE_H
