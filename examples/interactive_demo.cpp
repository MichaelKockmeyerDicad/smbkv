/**
 * @file examples/interactive_demo.cpp
 * @brief Interactive console demo for the SmbKvStore.
 *
 * Demonstrates long-running interactive transactions where locks are held for
 * the entire duration of user interaction (simulating a user who "thinks"
 * between writes).
 *
 * Usage (Windows, from the build directory):
 *   smbkv_interactive.exe \\\\fileserver\\kvstore
 *
 * Commands accepted at the prompt:
 *   PUT <key> <value>   Write a key (acquires an exclusive lock, held until commit)
 *   GET <key>           Read the current value of a key
 *   end                 Commit the transaction, then verify all written keys
 */

#include "smbkv/SmbKvStore.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

    const std::string sharePath = argv[1];

    try {
        smbkv::SmbKvStore store(sharePath);
        std::cout << "Connected to share: " << sharePath << "\n";

        // ----------------------------------------------------------------
        // Begin the first interactive transaction.
        // ----------------------------------------------------------------
        auto st = store.BeginTransaction();
        if (st != smbkv::Status::Ok) {
            std::cerr << "BeginTransaction failed: " << statusToString(st) << "\n";
            return 2;
        }

        std::cout << "\nTransaction started.\n"
                  << "Exclusive locks are acquired on PUT and held until you type 'end'.\n"
                  << "Other nodes trying to write the same keys will block until then.\n\n"
                  << "Commands:\n"
                  << "  PUT <key> <value>  - write a key (acquires and holds the lock)\n"
                  << "  GET <key>          - read the current value of a key\n"
                  << "  end                - commit the transaction and verify written keys\n"
                  << "\n";

        // Track which keys were written so we can verify after commit.
        std::vector<std::string> writtenKeys;

        // ----------------------------------------------------------------
        // Interactive command loop – runs until the user types "end".
        // ----------------------------------------------------------------
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "end") {
                break;
            }

            std::istringstream iss(line);
            std::string cmd;
            if (!(iss >> cmd)) {
                continue; // Empty line – ignore.
            }

            if (cmd == "PUT") {
                std::string key, value;
                if (!(iss >> key >> value)) {
                    std::cerr << "  Usage: PUT <key> <value>\n";
                    continue;
                }
                st = store.Put(key, value);
                std::cout << "  PUT " << key << " = \"" << value << "\""
                          << "  [" << statusToString(st) << "]\n";
                if (st == smbkv::Status::Ok) {
                    // Remember this key for post-commit verification.
                    if (std::find(writtenKeys.begin(), writtenKeys.end(), key)
                            == writtenKeys.end()) {
                        writtenKeys.push_back(key);
                    }
                }
            } else if (cmd == "GET") {
                std::string key;
                if (!(iss >> key)) {
                    std::cerr << "  Usage: GET <key>\n";
                    continue;
                }
                std::string value;
                st = store.Get(key, value);
                if (st == smbkv::Status::Ok) {
                    std::cout << "  GET " << key << " = \"" << value << "\"\n";
                } else {
                    std::cout << "  GET " << key << "  [" << statusToString(st) << "]\n";
                }
            } else {
                std::cerr << "  Unknown command '" << cmd << "'.\n"
                          << "  Use PUT, GET, or end.\n";
            }
        }

        // ----------------------------------------------------------------
        // Commit the transaction.
        // ----------------------------------------------------------------
        std::cout << "\nCommitting transaction...\n";
        st = store.Commit();
        std::cout << "Commit: " << statusToString(st) << "\n";

        if (st != smbkv::Status::Ok) {
            std::cerr << "Commit failed – your changes were not persisted.\n";
            return 3;
        }

        // ----------------------------------------------------------------
        // Verification: open a new transaction and read back every key
        // that was written, to confirm the data was correctly persisted.
        // ----------------------------------------------------------------
        if (!writtenKeys.empty()) {
            std::cout << "\n=== Verification: reading back all written keys in a new transaction ===\n";

            st = store.BeginTransaction();
            if (st != smbkv::Status::Ok) {
                std::cerr << "BeginTransaction (verify) failed: " << statusToString(st) << "\n";
                return 4;
            }

            bool allOk = true;
            for (const auto& key : writtenKeys) {
                std::string value;
                st = store.Get(key, value);
                if (st == smbkv::Status::Ok) {
                    std::cout << "  " << key << " = \"" << value << "\"  [OK]\n";
                } else {
                    std::cout << "  " << key << "  [" << statusToString(st) << " – FAIL]\n";
                    allOk = false;
                }
            }

            store.Rollback(); // End the read-only verification transaction.

            if (allOk) {
                std::cout << "\nAll " << writtenKeys.size()
                          << " key(s) verified successfully.\n";
            } else {
                std::cerr << "\nSome keys could not be verified!\n";
                return 5;
            }
        } else {
            std::cout << "\nNo keys were written.\n";
        }

        std::cout << "\nDone.\n";
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }

    return 0;
}
