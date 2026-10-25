# StrataForge

**A C++20 versioned storage engine built around failure semantics, not a database wrapper.** Checksummed atomic batch WAL, byte-sorted immutable segments, memtable flush, atomic manifest publication, full version-preserving compaction, historical point/range reads and exclusive process ownership.

## Build and verify
```powershell
python tools/verify.py
```
This **recompiles current source** with GCC/Clang or Zig, strict warnings, then runs the independent Python model/crash suite. On Windows set `$env:STRATA_ZIG='C:\path\to\zig.exe'` if Zig is not in PATH. The developer workspace's isolated Zig is auto-detected; end users need their own compiler. Linux may run `python tools/verify.py --sanitize` with GCC/Clang for ASan/UBSan. No external runtime libraries.

## Interactive protocol
```text
build/strataforge.exe NEW_DATABASE_DIRECTORY
READY 0
batch 616c706861 31 62657461 32
OK 1
batch 616c706861 ~
OK 2
get 1 616c706861
VALUE 31
get 2 616c706861
MISSING
flush
OK 2
compact
OK 2
scan 1 - ff
ROWS 2 616c706861 31 62657461 32
quit
```
Use `build/strataforge` on Linux. Lowercase hex encodes arbitrary bytes, `-` means empty bytes, `~` means tombstone. `batch` accepts distinct key/value pairs and commits one sequence atomically; `snapshot` returns current sequence; scans are `[lower,upper)` at a supplied sequence. Empty values are not deletions. Protocol errors terminate with exit2; only a **complete `OK N` line** acknowledges a write. See `python tools/experiment.py` for a self-cleaning synthetic demonstration and measured latency summary.

## Engineering acceptance
Independent Python map/version oracle compares random batches, deletes, scans and old snapshots through restarts/flush/compaction. Real child-process kill tests verify acknowledged writes; deterministic process exit at WAL sync, segment sync, new-WAL sync, manifest sync and manifest publication tests recovery cuts. Tail truncation walks every incomplete frame position. Complete CRC damage, missing targets, invalid sort order and competing open fail closed. No test opens your existing database or real filesystem inventories.

## Boundaries
Single writer, one engine thread; cooperating processes excluded by OS file lock. All versions and segments are loaded into a sorted memory index; this is **not a block-cached, bounded-RSS production database**. Compaction preserves all old versions and tombstones (no GC), and is stop-the-world. Segment/WAL formats use CRC32 for damage detection, not authenticity. Only an incomplete final active-WAL frame is truncated, never a complete bad-CRC record. Interrupted first-ever initialization without a manifest fails closed; no automatic repair.

Fsync/FlushFileBuffers precedes ACK; POSIX syncs directory publication, Windows uses write-through replacement. **Process crash tests are not physical power-cut tests or guarantees for every filesystem.** No encryption, distributed replication, retry IDs or production workload claim. [Design](docs/DESIGN.md), [roadmap](docs/ROADMAP.md), [evidence](docs/PROGRESS.md).
