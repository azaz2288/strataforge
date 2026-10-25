# Engineering rules
Read docs/DESIGN.md and docs/ROADMAP.md first. Rebuild current C++ before tests.
Do not claim power-loss safety from process-crash tests. Preserve existing databases.
Reject malformed committed files; only an incomplete final active-WAL frame may be truncated.
Update tests/docs for semantic changes; keep strict compiler warnings and independent Python oracle.
