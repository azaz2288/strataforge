#include "../src/engine.hpp"
#include <iostream>

int main(int argc, char **argv) {
    try {
        strata::require(argc == 2, "test needs new synthetic directory");
        strata::fs::path path(argv[1]);
        {
            strata::Engine db(path);
            strata::require(db.batch({{"a", "first"}}) == 1, "initial batch");
            try {
                db.batch({{"a", "x"}, {"a", "y"}});
                throw std::logic_error("duplicate accepted");
            } catch (const std::runtime_error &) {
            }
            strata::require(db.sequence() == 1, "input rejection must not poison engine");
            // Deterministic actual open failure, not a mocked engine exception.
            strata::fs::rename(path / "1.wal", path / "saved-wal");
            strata::fs::create_directory(path / "1.wal");
            bool failed = false;
            try {
                db.batch({{"b", "second"}});
            } catch (const std::runtime_error &) {
                failed = true;
            }
            strata::require(failed, "write error required");
            for (int action = 0; action < 5; ++action) {
                failed = false;
                try {
                    if (action == 0)
                        (void)db.get("a", 1);
                    if (action == 1)
                        (void)db.batch({{"c", "third"}});
                    if (action == 2)
                        db.flush();
                    if (action == 3)
                        db.compact();
                    if (action == 4)
                        (void)db.scan("z", "z", 1);
                } catch (const std::runtime_error &) {
                    failed = true;
                }
                strata::require(failed, "poisoned engine continued");
            }
            strata::fs::remove(path / "1.wal");
            strata::fs::rename(path / "saved-wal", path / "1.wal");
        }
        strata::Engine reopened(path);
        strata::require(reopened.sequence() == 1 && reopened.get("a", 1) == "first",
                        "reopen after real I/O failure");
        std::cout << "C++ API input rejection and real I/O poisoning: passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
