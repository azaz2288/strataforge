# v0.1.0 · 2026-10-25 (America/New_York)

## Implemented and locally accepted
- Original C++20 engine, no SQLite/RocksDB wrapper: atomic CRC32 WAL batches, actual (key,sequence)-sorted segment records, atomic manifest publication, historical point/range queries, version-preserving full compaction, cooperating-process exclusion.
- Windows Unicode directory support via wmain, byte keys/empty values/tombstones, key/value/frame/sequence limits, CRC/schema/sortedness/missing-target rejection. Only incomplete active-WAL suffix is durably truncated. A real write-open failure poisons the engine until reopened.
- Current-source Zig0.13 strict C++20 rebuild:14 Python test methods8.692s plus separate direct C++ API fault test passed; full compile+API+Python command22.715s on this Windows11 machine. Twelve deterministic seeds ×90 atomic batches independently checked against Python map/snapshot states through flush/compaction/restarts. All incomplete positions of a final frame tested; complete CRC damage rejected. Process kill after ACK and7 flush/compaction publication cut scenarios tested. Maximum1MiB value and Unicode path roundtrip passed.
- Python compile and Ruff checks/format passed. Windows/Linux workflow rebuilds current source; Linux additionally rebuilds/runs ASan+UBSan. Actual same-SHA remote results are publication evidence, not asserted in this pre-publication source record.

## Real synthetic measurement (not a scale claim)
`tools/experiment.py`:seed7931,250 single-mutation acknowledged batches,80-key maximum key space,80-byte values. ACK includes IPC/interpreter/fsync:median2.19935ms,p953.7592ms,max4.6695ms. Compaction9.4687ms; restart including process9.5168ms. Segment29250B before and after:all history is intentionally retained, so no claimed space reduction. Current snapshot/old snapshot/map oracle and kill/reopen matched. Raw250 samples and source SHA256 are saved in the workspace frontier-lab report; regenerate locally for your machine.

## Failures retained and fixed
First Ubuntu GCC CI rejected a misleadingly indented compaction-cleanup loop under -Werror. Added explicit braces/separate statements and repository clang-format policy/check; strict warnings remain enabled. Review also caught empty-range scan skipping get's poison guard, so scan now checks poison directly and the C++ real-I/O-failure test covers that path. This is a real failed first CI, not reported as all-green history; the repaired current commit must pass both platforms separately.
Initial harness had non-idempotent cleanup/Windows exited-pipe race, causing cascading test errors; fixed client lifecycle. Real protocol bug found:ostream wrote `OK` before batch evaluation could throw/exit; changed to finish operation before writing a full acknowledgement. Tests still require no false crash acknowledgement. No test assertions were relaxed to accept partial ACK. The independent C++ API test separately injects an actual filesystem open failure and checks read/write/flush/compact refusal until reopen.

## Not done
Full in-memory version index and stop-the-world compaction; no block index/cache or version garbage collection, bounded RSS, background concurrency, encryption or replication. Process termination is not physical power-cut validation. Interrupted first initialization without a manifest deliberately fails closed. Do not use v0.1 for irreplaceable data.
