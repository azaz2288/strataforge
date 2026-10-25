# Acceptance roadmap
- [x] v0.1 local core: checksummed batch WAL, strict recovery, snapshot reads/scans, sorted segments/manifest/compaction, process locking, independent oracle, crash tests and direct C++ I/O-failure test.
- Dual-platform CI workflow is included; actual publication status must be checked for the current full commit SHA, never inferred from this checklist.
- [ ] Block-indexed immutable segments, independently measured cold/warm reads and RSS.
- [ ] Safe version reclamation with persisted snapshot/reader lifetime protocol.
- [ ] Background compaction with concurrent readers, cancellation and bounded write stalls.
- [ ] Filesystem-specific durability experiments and format migration tooling.

An unchecked future milestone is not implied by v0.1.
