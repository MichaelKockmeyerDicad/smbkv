/**
 * @file SmbKvStore.cpp
 * @brief Implementation of the SMB-backed distributed Key-Value store.
 *
 * See include/smbkv/SmbKvStore.h for the full design description.
 *
 * WAL record format (plain-text, newline-terminated)
 * --------------------------------------------------
 * Each committed transaction is stored as one or more UTF-8 lines followed
 * by a blank line (empty record separator):
 *
 *   PUT <key_length> <key_bytes_hex> <value_length> <value_bytes_hex>\n
 *   PUT ...
 *   \n
 *
 * Field descriptions
 *   key_length   – decimal byte count of the raw key string.
 *   key_bytes_hex – the key bytes encoded as lowercase hexadecimal.
 *   value_length  – decimal byte count of the raw value string.
 *   value_bytes_hex – the value bytes encoded as lowercase hexadecimal.
 *
 * The hex encoding avoids any issues with keys or values that contain
 * whitespace, newlines, or other control characters.
 *
 * A blank line (i.e. "\n" on its own) acts as the transaction commit marker.
 * Records that are missing their terminating blank line (e.g. because a node
 * crashed mid-write) are silently discarded during WAL replay.
 */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "smbkv/SmbKvStore.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace smbkv {

// ---------------------------------------------------------------------------
// Utility helpers (file-local)
// ---------------------------------------------------------------------------
namespace {

/// Convert a raw byte string to lowercase hexadecimal.
std::string toHex(const std::string& s) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : s) {
        oss << std::setw(2) << static_cast<unsigned>(c);
    }
    return oss.str();
}

/// Decode a lowercase hexadecimal string back to raw bytes.
std::string fromHex(const std::string& hex) {
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

/// Get the current size of an open file as a 64-bit value.
/// Returns UINT64_MAX on failure.
uint64_t getFileSize64(HANDLE h) {
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz)) {
        return UINT64_MAX;
    }
    return static_cast<uint64_t>(sz.QuadPart);
}

/// Seek an open file handle to the given 64-bit offset.
bool seekFile(HANDLE h, uint64_t offset) {
    LARGE_INTEGER li = {};
    li.QuadPart = static_cast<LONGLONG>(offset);
    LARGE_INTEGER newPos = {};
    return SetFilePointerEx(h, li, &newPos, FILE_BEGIN) != FALSE;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SmbKvStore::SmbKvStore(std::string sharePath)
    : sharePath_(std::move(sharePath))
    , walHandle_(INVALID_HANDLE_VALUE)
    , locksHandle_(INVALID_HANDLE_VALUE)
    , walOffset_(0)
    , inTransaction_(false)
    , snapshotOffset_(0)
{
    // Open wal.log with read+write access, shared with other nodes.
    walHandle_ = openShareFile(
        "wal.log",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        OPEN_ALWAYS);

    // Open locks.bin with read+write access (needed for LockFileEx), shared.
    locksHandle_ = openShareFile(
        "locks.bin",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        OPEN_ALWAYS);

    // Replay the full WAL to build the initial in-memory state.
    walOffset_ = replayWal(0, kvMap_);
}

SmbKvStore::~SmbKvStore() {
    // Best-effort rollback of any active transaction.
    if (inTransaction_) {
        Rollback();
    }

    if (walHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(walHandle_);
        walHandle_ = INVALID_HANDLE_VALUE;
    }
    if (locksHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(locksHandle_);
        locksHandle_ = INVALID_HANDLE_VALUE;
    }
}

// ---------------------------------------------------------------------------
// Transaction API
// ---------------------------------------------------------------------------

Status SmbKvStore::BeginTransaction() {
    if (inTransaction_) {
        return Status::AlreadyInTx;
    }

    // Replay any WAL entries written by other nodes since our last transaction.
    walOffset_ = replayWal(walOffset_, kvMap_);
    if (walOffset_ == UINT64_MAX) {
        return Status::IoError;
    }

    snapshotOffset_ = walOffset_;
    writeBuffer_.clear();
    heldLocks_.clear();
    inTransaction_ = true;
    return Status::Ok;
}

Status SmbKvStore::Get(const std::string& key, std::string& outValue) {
    if (!inTransaction_) {
        return Status::InvalidState;
    }

    // Check the write buffer first (read-your-writes semantics).
    auto bufIt = writeBuffer_.find(key);
    if (bufIt != writeBuffer_.end()) {
        outValue = bufIt->second;
        return Status::Ok;
    }

    // Fall back to the in-memory snapshot.
    auto mapIt = kvMap_.find(key);
    if (mapIt == kvMap_.end()) {
        return Status::NotFound;
    }
    outValue = mapIt->second;
    return Status::Ok;
}

Status SmbKvStore::Put(const std::string& key, const std::string& value) {
    if (!inTransaction_) {
        return Status::InvalidState;
    }

    uint64_t offset = keyToLockOffset(key);

    // Acquire the exclusive lock only if we don't already hold it.
    bool alreadyLocked = (std::find(heldLocks_.begin(), heldLocks_.end(), offset)
                          != heldLocks_.end());
    if (!alreadyLocked) {
        if (!acquireLock(locksHandle_, offset)) {
            return Status::LockFailed;
        }
        heldLocks_.push_back(offset);
    }

    writeBuffer_[key] = value;
    return Status::Ok;
}

Status SmbKvStore::Commit() {
    if (!inTransaction_) {
        return Status::InvalidState;
    }

    if (writeBuffer_.empty()) {
        // Nothing to commit; just end the transaction.
        inTransaction_ = false;
        return Status::Ok;
    }

    // --- 1. Lock the WAL ---
    if (!acquireLock(walHandle_, WAL_LOCK_OFFSET)) {
        return Status::LockFailed;
    }

    // --- 2. Serialise the transaction buffer ---
    std::string record;
    for (const auto& [k, v] : writeBuffer_) {
        std::string kHex = toHex(k);
        std::string vHex = toHex(v);
        record += "PUT ";
        record += std::to_string(k.size());
        record += ' ';
        record += kHex;
        record += ' ';
        record += std::to_string(v.size());
        record += ' ';
        record += vHex;
        record += '\n';
    }
    record += '\n'; // Commit marker (blank line).

    // --- 3. Seek to end of file and append ---
    uint64_t endOffset = getFileSize64(walHandle_);
    if (endOffset == UINT64_MAX) {
        releaseLock(walHandle_, WAL_LOCK_OFFSET);
        return Status::IoError;
    }
    if (!seekFile(walHandle_, endOffset)) {
        releaseLock(walHandle_, WAL_LOCK_OFFSET);
        return Status::IoError;
    }

    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(
        walHandle_,
        record.data(),
        static_cast<DWORD>(record.size()),
        &bytesWritten,
        nullptr);

    if (!writeOk || bytesWritten != static_cast<DWORD>(record.size())) {
        releaseLock(walHandle_, WAL_LOCK_OFFSET);
        return Status::IoError;
    }

    // --- 4. Flush to guarantee durability on the SMB server disk ---
    if (!FlushFileBuffers(walHandle_)) {
        releaseLock(walHandle_, WAL_LOCK_OFFSET);
        return Status::IoError;
    }

    // --- 5. Release WAL lock ---
    releaseLock(walHandle_, WAL_LOCK_OFFSET);

    // --- 6. Release all per-key locks ---
    for (uint64_t lockOffset : heldLocks_) {
        releaseLock(locksHandle_, lockOffset);
    }
    heldLocks_.clear();

    // --- 7. Apply write buffer to local in-memory state ---
    for (const auto& [k, v] : writeBuffer_) {
        kvMap_[k] = v;
    }
    walOffset_ = endOffset + static_cast<uint64_t>(record.size());
    writeBuffer_.clear();
    inTransaction_ = false;
    return Status::Ok;
}

Status SmbKvStore::Rollback() {
    if (!inTransaction_) {
        return Status::Ok; // Idempotent.
    }

    writeBuffer_.clear();

    for (uint64_t lockOffset : heldLocks_) {
        releaseLock(locksHandle_, lockOffset);
    }
    heldLocks_.clear();

    inTransaction_ = false;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

HANDLE SmbKvStore::openShareFile(const std::string& filename,
                                  DWORD desiredAccess,
                                  DWORD shareMode,
                                  DWORD creationDisposition) const {
    std::string fullPath = sharePath_ + '\\' + filename;
    HANDLE h = CreateFileA(
        fullPath.c_str(),
        desiredAccess,
        shareMode,
        nullptr,
        creationDisposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "SmbKvStore: cannot open '" + fullPath +
            "' (Win32 error " + std::to_string(GetLastError()) + ')');
    }
    return h;
}

uint64_t SmbKvStore::replayWal(
        uint64_t startOffset,
        std::unordered_map<std::string, std::string>& targetMap) {

    // Determine how far the file extends.
    uint64_t fileSize = getFileSize64(walHandle_);
    if (fileSize == UINT64_MAX) {
        return UINT64_MAX; // Signal error to caller.
    }
    if (fileSize <= startOffset) {
        return startOffset; // Nothing new to read.
    }

    // Read the new portion of the WAL into a buffer.
    uint64_t readLen = fileSize - startOffset;
    std::string buf(static_cast<size_t>(readLen), '\0');

    if (!seekFile(walHandle_, startOffset)) {
        return UINT64_MAX;
    }

    DWORD totalRead = 0;
    while (totalRead < static_cast<DWORD>(readLen)) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(
            walHandle_,
            buf.data() + totalRead,
            static_cast<DWORD>(readLen) - totalRead,
            &bytesRead,
            nullptr);
        if (!ok || bytesRead == 0) {
            break;
        }
        totalRead += bytesRead;
    }
    buf.resize(totalRead);

    // Parse WAL records.  A record consists of one or more PUT lines followed
    // by a blank line.  Partial records (missing the blank-line terminator)
    // are ignored so we stay consistent even if a writer crashed mid-commit.
    uint64_t committedUpTo = startOffset;
    size_t pos = 0;

    while (pos < buf.size()) {
        // Collect lines until a blank line (commit marker) or EOF.
        std::unordered_map<std::string, std::string> txBuffer;
        bool foundCommit = false;
        bool txMalformed = false;

        while (pos < buf.size()) {
            size_t lineEnd = buf.find('\n', pos);
            if (lineEnd == std::string::npos) {
                // Incomplete line; stop parsing.
                goto doneReplaying;
            }
            std::string line = buf.substr(pos, lineEnd - pos);
            pos = lineEnd + 1;

            if (line.empty()) {
                // Blank line = commit marker for this transaction.
                foundCommit = true;
                break;
            }

            // Parse: PUT <keyLen> <keyHex> <valLen> <valHex>
            std::istringstream ss(line);
            std::string op;
            size_t keyLen = 0, valLen = 0;
            std::string keyHex, valHex;

            if (!(ss >> op >> keyLen >> keyHex >> valLen >> valHex)) {
                // Malformed line: mark transaction as invalid and consume
                // lines until the commit marker so pos stays consistent.
                txMalformed = true;
                continue;
            }
            if (op != "PUT") {
                txMalformed = true;
                continue;
            }

            std::string key   = fromHex(keyHex);
            std::string value = fromHex(valHex);

            if (key.size() != keyLen || value.size() != valLen) {
                // Length mismatch: mark transaction as invalid.
                txMalformed = true;
                continue;
            }

            if (!txMalformed) {
                txBuffer[key] = value;
            }
        }

        if (foundCommit && !txMalformed) {
            for (auto& [k, v] : txBuffer) {
                targetMap[k] = v;
            }
            committedUpTo = startOffset + static_cast<uint64_t>(pos);
        }
    }

doneReplaying:
    return committedUpTo;
}

uint64_t SmbKvStore::keyToLockOffset(const std::string& key) {
    // FNV-1a 64-bit hash.
    constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME        = 1099511628211ULL;

    uint64_t hash = FNV_OFFSET_BASIS;
    for (unsigned char c : key) {
        hash ^= static_cast<uint64_t>(c);
        hash *= FNV_PRIME;
    }

    // Map to [LOCK_RESERVED_BYTES, LOCK_RESERVED_BYTES + LOCK_ADDRESS_SPACE).
    return LOCK_RESERVED_BYTES + (hash % LOCK_ADDRESS_SPACE);
}

bool SmbKvStore::acquireLock(HANDLE fileHandle, uint64_t offset) {
    OVERLAPPED ov = {};
    ov.Offset     = static_cast<DWORD>(offset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

    // LOCKFILE_EXCLUSIVE_LOCK | 0 (blocking) – waits until the lock is granted.
    return LockFileEx(fileHandle,
                      LOCKFILE_EXCLUSIVE_LOCK,
                      0,       // reserved
                      1, 0,    // nNumberOfBytesToLockLow / High (1 byte)
                      &ov) != FALSE;
}

bool SmbKvStore::releaseLock(HANDLE fileHandle, uint64_t offset) {
    OVERLAPPED ov = {};
    ov.Offset     = static_cast<DWORD>(offset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

    return UnlockFileEx(fileHandle,
                        0,       // reserved
                        1, 0,    // nNumberOfBytesToUnlockLow / High
                        &ov) != FALSE;
}

} // namespace smbkv

#else  // !_WIN32

// ---------------------------------------------------------------------------
// Non-Windows stub – keeps the translation unit non-empty on Linux / macOS
// build farms and produces a clear compile-time message.
// ---------------------------------------------------------------------------
#include "smbkv/SmbKvStore.h"
#include <stdexcept>

namespace smbkv {

static void platformNotSupported() {
    throw std::runtime_error(
        "SmbKvStore is only supported on Windows (requires Win32 APIs).");
}

SmbKvStore::SmbKvStore(std::string /*sharePath*/) {
    platformNotSupported();
}
SmbKvStore::~SmbKvStore() {}
Status SmbKvStore::BeginTransaction()                                  { platformNotSupported(); return Status::IoError; }
Status SmbKvStore::Get(const std::string&, std::string&)               { platformNotSupported(); return Status::IoError; }
Status SmbKvStore::Put(const std::string&, const std::string&)         { platformNotSupported(); return Status::IoError; }
Status SmbKvStore::Commit()                                            { platformNotSupported(); return Status::IoError; }
Status SmbKvStore::Rollback()                                          { platformNotSupported(); return Status::IoError; }

HANDLE SmbKvStore::openShareFile(const std::string&, DWORD, DWORD, DWORD) const { return INVALID_HANDLE_VALUE; }
uint64_t SmbKvStore::replayWal(uint64_t, std::unordered_map<std::string,std::string>&) { return 0; }
uint64_t SmbKvStore::keyToLockOffset(const std::string&) { return 0; }
bool SmbKvStore::acquireLock(HANDLE, uint64_t) { return false; }
bool SmbKvStore::releaseLock(HANDLE, uint64_t) { return false; }

} // namespace smbkv

#endif // _WIN32
