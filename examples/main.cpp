/**
 * @file examples/main.cpp
 * @brief Simple demonstration of the SmbKvStore library.
 *
 * Usage (Windows, from the build directory):
 *   smbkv_example.exe \\\\fileserver\\kvstore
 *
 * The first argument is the UNC path to the SMB share that contains (or will
 * contain) the wal.log and locks.bin control files.
 */

#include "smbkv/SmbKvStore.h"

#include <iostream>
#include <string>

static const char* statusToString(smbkv::Status s) {
    switch (s) {
        case smbkv::Status::Ok:           return "Ok";
        case smbkv::Status::NotFound:     return "NotFound";
        case smbkv::Status::LockFailed:   return "LockFailed";
        case smbkv::Status::IoError:      return "IoError";
        case smbkv::Status::InvalidState: return "InvalidState";
        case smbkv::Status::AlreadyInTx:  return "AlreadyInTx";
        default:                          return "Unknown";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <smb-share-path>\n"
                  << "Example: " << argv[0] << R"( \\fileserver\kvstore)" << "\n";
        return 1;
    }

    std::string sharePath = argv[1];

    try {
        smbkv::SmbKvStore store(sharePath);
        std::cout << "Connected to share: " << sharePath << "\n\n";

        // ----------------------------------------------------------------
        // Transaction 1: write a key
        // ----------------------------------------------------------------
        std::cout << "=== Transaction 1: Put 'hello' = 'world' ===\n";
        {
            auto st = store.BeginTransaction();
            std::cout << "BeginTransaction: " << statusToString(st) << "\n";

            st = store.Put("hello", "world");
            std::cout << "Put(hello, world): " << statusToString(st) << "\n";

            st = store.Commit();
            std::cout << "Commit: " << statusToString(st) << "\n";
        }

        // ----------------------------------------------------------------
        // Transaction 2: read the key back and update it
        // ----------------------------------------------------------------
        std::cout << "\n=== Transaction 2: Read-Modify-Write 'hello' ===\n";
        {
            auto st = store.BeginTransaction();
            std::cout << "BeginTransaction: " << statusToString(st) << "\n";

            std::string value;
            st = store.Get("hello", value);
            std::cout << "Get(hello): " << statusToString(st);
            if (st == smbkv::Status::Ok) {
                std::cout << " -> '" << value << "'";
            }
            std::cout << "\n";

            st = store.Put("hello", value + "_updated");
            std::cout << "Put(hello, " << value << "_updated): "
                      << statusToString(st) << "\n";

            // Read-your-writes: the updated value should be visible.
            std::string updated;
            st = store.Get("hello", updated);
            std::cout << "Get(hello) [read-your-writes]: " << statusToString(st);
            if (st == smbkv::Status::Ok) {
                std::cout << " -> '" << updated << "'";
            }
            std::cout << "\n";

            st = store.Commit();
            std::cout << "Commit: " << statusToString(st) << "\n";
        }

        // ----------------------------------------------------------------
        // Transaction 3: demonstrate rollback
        // ----------------------------------------------------------------
        std::cout << "\n=== Transaction 3: Rollback demonstration ===\n";
        {
            auto st = store.BeginTransaction();
            std::cout << "BeginTransaction: " << statusToString(st) << "\n";

            st = store.Put("will_be_discarded", "temporary_value");
            std::cout << "Put(will_be_discarded, temporary_value): "
                      << statusToString(st) << "\n";

            st = store.Rollback();
            std::cout << "Rollback: " << statusToString(st) << "\n";
        }

        // Verify the rolled-back key is not present.
        std::cout << "\n=== Verification after rollback ===\n";
        {
            auto st = store.BeginTransaction();
            std::string val;
            st = store.Get("will_be_discarded", val);
            std::cout << "Get(will_be_discarded): " << statusToString(st) << "\n";
            store.Rollback();
        }

        std::cout << "\nDone.\n";
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }

    return 0;
}
