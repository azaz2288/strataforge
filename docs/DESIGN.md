# StrataForge v0.1.0 design

C++20 single-writer, versioned byte-string key/value engine. A checksummed atomic batch WAL feeds a memtable; flush publishes an immutable sorted segment and a new WAL through one atomic manifest replacement. Full compaction preserves every version, including tombstones, so pinned sequence snapshots remain readable. Range scans use [lower, upper) byte order and a fixed sequence. No network, SQL or external database library implements the engine.

## Commit protocol
Every acknowledged batch writes one length-delimited CRC32 frame, then fsync/FlushFileBuffers, then exposes all mutations under one sequence. A failed write poisons the process (CLI exits); caller must reopen before retrying. An unacknowledged batch may survive. No cross-process client retry deduplication is promised.

Flush: write/sync fresh segment → write/sync fresh empty WAL → atomically replace/sync manifest → switch active state → remove old WAL. Compaction: write/sync merged segment → publish manifest referencing same active WAL → remove replaced segments. Old files are never deleted before publication; orphan files are ignored and IDs never reused. POSIX fsyncs the directory; Windows uses MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) and file FlushFileBuffers, but does not claim tested filesystem/power-failure guarantees.

Manifest and segments are CRC32 framed. WAL recovery accepts only a truncated final frame and durably truncates to the last valid batch. Any complete bad CRC, invalid schema, sequence regression, missing manifest target, malformed segment or unsupported version fails closed. Limits: 16MiB frame/64KiB key/1MiB value/100000 batch mutations. Empty values differ from tombstones.

OS advisory/exclusive lock on LOCK prevents cooperating processes from opening one directory simultaneously; the OS releases it on death. Not a security boundary against arbitrary local filesystem mutation/symlinks. Never run on user data during tests.

## Deliberate limits
Segments are sorted version records but fully loaded into memory on open; point/range queries use an ordered in-memory index, not on-disk block search. Compaction retains all historical versions and is stop-the-world, so no bounded storage/RSS or production throughput claim. Snapshot IDs last only within a CLI process; sequence values remain queryable after restart. No concurrent threads inside an engine, transactions across batches, compression, encryption, replication or online repair. Deterministic failpoints and real kill tests cover process crashes, not physical power loss.
