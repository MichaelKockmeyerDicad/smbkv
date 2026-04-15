# smbkv

A C++ library implementing a distributed Key-Value store that uses a Windows
network share (SMB) as its communication and persistence layer.

## Architecture

```
\\fileserver\kvstore\
  ├── wal.log     – append-only Write-Ahead Log (committed transactions)
  └── locks.bin   – dummy file used exclusively for byte-range locking
```

- **Shared WAL**: every committed transaction is appended to `wal.log`.  Each
  client replays the log on startup and at the beginning of every new
  transaction to keep its in-memory state up to date.
- **Pessimistic locking**: keys are hashed (FNV-1a 64-bit) to byte offsets in
  `locks.bin`.  Exclusive byte-range locks (`LockFileEx`) are held from
  `Put()` until `Commit()` / `Rollback()`, blocking other nodes that want to
  write the same key.
- **Crash safety**: if a client crashes the Windows SMB server releases its
  byte-range locks automatically when the TCP/SMB session times out.

## Requirements

- Windows (Win32 APIs: `CreateFile`, `LockFileEx`, `UnlockFileEx`,
  `ReadFile`, `WriteFile`, `FlushFileBuffers`, `SetFilePointerEx`)
- C++17 compiler (MSVC, Clang-cl, or MinGW)
- CMake ≥ 3.16

## Building

```powershell
cmake -B build -S .
cmake --build build --config Release
```

The build produces:
- `smbkv.lib` / `smbkv.dll` – the library
- `smbkv_example.exe` – a simple demonstration program

## Quick start

```powershell
# Create the control files on the share (once, by any node):
New-Item -ItemType File "\\fileserver\kvstore\wal.log"
New-Item -ItemType File "\\fileserver\kvstore\locks.bin"

# Run the example:
.\build\Release\smbkv_example.exe \\fileserver\kvstore
```

## API

```cpp
#include <smbkv/SmbKvStore.h>

smbkv::SmbKvStore store(R"(\\fileserver\kvstore)");

store.BeginTransaction();

std::string val;
if (store.Get("counter", val) == smbkv::Status::Ok) {
    store.Put("counter", std::to_string(std::stoi(val) + 1));
} else {
    store.Put("counter", "1");
}

store.Commit();   // or store.Rollback();
```

| Method               | Description                                              |
|----------------------|----------------------------------------------------------|
| `BeginTransaction()` | Sync WAL, snapshot current state, open a transaction.    |
| `Get(key, value)`    | Read from the in-memory snapshot (read-your-writes).     |
| `Put(key, value)`    | Acquire exclusive lock on the key; buffer the write.     |
| `Commit()`           | Append to WAL, flush, release all locks.                 |
| `Rollback()`         | Discard buffer, release all locks.                       |

All methods return a `smbkv::Status` enum value (`Ok`, `NotFound`,
`LockFailed`, `IoError`, `InvalidState`, `AlreadyInTx`).

## WAL record format

Each committed transaction is stored as one UTF-8 text block terminated by a
blank line:

```
PUT <key_len> <key_hex> <val_len> <val_hex>
PUT <key_len> <key_hex> <val_len> <val_hex>

```

Keys and values are hex-encoded so that arbitrary byte sequences (including
whitespace and newlines) are supported safely.

## Limitations

- Windows only (the implementation uses Win32 file-locking APIs).
- Performance is intentionally not a design goal; the primary use case is
  academic / proof-of-concept.
- Hash collisions in the lock address space (1 000 000 slots) will cause
  false-positive lock contention but never data corruption.
