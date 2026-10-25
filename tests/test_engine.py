"""Separate Python map/version oracle; never import C++ serialization logic."""

import os
import random
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

BINARY = Path(
    os.environ.get(
        "STRATA_BINARY",
        "build/strataforge.exe" if os.name == "nt" else "build/strataforge",
    )
).resolve()


def hx(value):
    return value.hex() or "-"


class Client:
    def __init__(self, path, **env):
        self.process = subprocess.Popen(
            [str(BINARY), str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=dict(os.environ, **env),
        )
        self.ready = self.process.stdout.readline().strip()
        self.closed = False
        self.stderr = ""

    def call(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.process.stdout.readline().strip()

    def close(self, kill=False):
        if self.closed:
            return self.stderr
        if self.process.poll() is None and not kill:
            try:
                self.call("quit")
            except (BrokenPipeError, OSError):
                pass
        elif self.process.poll() is None and kill:
            self.process.kill()
        self.process.wait(timeout=15)
        self.stderr = self.process.stderr.read()
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            try:
                stream.close()
            except OSError:
                pass
        self.closed = True
        return self.stderr


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "db"
        self.clients = []
        self.addCleanup(self.close_clients)

    def close_clients(self):
        for client in reversed(self.clients):
            client.close()

    def client(self, **env):
        client = Client(self.path, **env)
        self.clients.append(client)
        return client

    def test_atomic_batches_empty_values_binary_keys_and_snapshots(self):
        c = self.client()
        self.assertEqual(c.call("batch 00ff - 61 00fe"), "OK 1")
        self.assertEqual(c.call("batch 61 ~ 62 32"), "OK 2")
        self.assertEqual(c.call("get 1 61"), "VALUE 00fe")
        self.assertEqual(c.call("get 2 61"), "MISSING")
        self.assertEqual(c.call("get 2 00ff"), "VALUE -")
        self.assertEqual(c.call("scan 1 - ffff"), "ROWS 2 00ff - 61 00fe")

    def test_seeded_differential_restarts_flush_compact_and_old_snapshots(self):
        for seed in range(12):
            with self.subTest(seed=seed):
                c = self.client()
                oracle = [{}]
                rng = random.Random(seed)
                for step in range(90):
                    changes = {
                        bytes([rng.randrange(12)]): None
                        if rng.random() < 0.3
                        else rng.randbytes(rng.randrange(8))
                        for _ in range(1 + rng.randrange(4))
                    }
                    latest = dict(oracle[-1])
                    for key, value in changes.items():
                        if value is None:
                            latest.pop(key, None)
                        else:
                            latest[key] = value
                    command = "batch " + " ".join(
                        hx(k) + " " + ("~" if v is None else hx(v)) for k, v in changes.items()
                    )
                    self.assertEqual(c.call(command), f"OK {len(oracle)}")
                    oracle.append(latest)
                    if step % 7 == 0:
                        self.assertTrue(c.call("flush").startswith("OK"))
                    if step % 13 == 0:
                        self.assertTrue(c.call("compact").startswith("OK"))
                    if step % 19 == 0:
                        self.assertEqual(c.close(), "")
                        c = self.client()
                    at = rng.randrange(len(oracle))
                    lo, hi = sorted([bytes([rng.randrange(13)]), bytes([rng.randrange(13)])])
                    rows = [(k, v) for k, v in sorted(oracle[at].items()) if lo <= k < hi]
                    expected = f"ROWS {len(rows)}" + "".join(
                        " " + hx(k) + " " + hx(v) for k, v in rows
                    )
                    self.assertEqual(c.call(f"scan {at} {hx(lo)} {hx(hi)}"), expected)
                c.close()
                # Distinct store for the next seed; only synthetic owned directory.
                self.path = Path(self.temp.name) / f"db-{seed}"

    def test_real_process_kill_preserves_acknowledged_batch(self):
        c = self.client()
        self.assertEqual(c.call("batch 61 31 62 32"), "OK 1")
        c.close(kill=True)
        reopened = self.client()
        self.assertEqual(reopened.call("scan 1 - ff"), "ROWS 2 61 31 62 32")

    def test_lock_excludes_second_process_and_is_released_by_death(self):
        c = self.client()
        other = Client(self.path)
        self.assertEqual(other.ready, "")
        self.assertIn("locked", other.close())
        c.close(kill=True)
        self.assertEqual(self.client().ready, "READY 0")

    def test_every_truncated_tail_position_discards_only_uncommitted_batch(self):
        c = self.client()
        c.call("batch 61 31")
        c.close()
        wal = next(self.path.glob("*.wal"))
        valid = wal.read_bytes()
        c = self.client()
        c.call("batch 62 32 63 33")
        c.close()
        second = wal.read_bytes()[len(valid) :]
        for cut in range(1, len(second)):
            wal.write_bytes(valid + second[:cut])
            c = self.client()
            self.assertEqual(c.ready, "READY 1", cut)
            self.assertEqual(c.call("get 1 61"), "VALUE 31")
            c.close()
            self.assertEqual(wal.read_bytes(), valid)

    def test_complete_crc_damage_fails_closed_and_does_not_truncate(self):
        c = self.client()
        c.call("batch 61 31")
        c.close()
        wal = next(self.path.glob("*.wal"))
        data = bytearray(wal.read_bytes())
        data[-1] ^= 1
        wal.write_bytes(data)
        broken = Client(self.path)
        self.assertEqual(broken.ready, "")
        self.assertIn("CRC mismatch", broken.close())
        self.assertEqual(wal.read_bytes(), bytes(data))

    def test_flush_crash_cuts_recover_entire_acknowledged_state(self):
        for cut in (
            "segment_synced",
            "new_wal_synced",
            "manifest_synced",
            "manifest_published",
        ):
            with self.subTest(cut=cut):
                self.path = Path(self.temp.name) / cut
                c = self.client()
                c.call("batch 61 31 62 32")
                c.close()
                crashed = self.client(STRATA_FAILPOINT=cut)
                self.assertEqual(crashed.call("flush"), "")
                crashed.close()
                self.assertEqual(crashed.process.returncode, 86)
                c = self.client()
                self.assertEqual(c.ready, "READY 1")
                self.assertEqual(c.call("scan 1 - ff"), "ROWS 2 61 31 62 32")
                c.call("batch 61 33")
                c.call("flush")
                c.close()
                c = self.client()
                self.assertEqual(c.call("get 2 61"), "VALUE 33")
                c.close()

    def test_compaction_crash_cuts_keep_active_wal_and_snapshots(self):
        for cut in ("segment_synced", "manifest_synced", "manifest_published"):
            self.path = Path(self.temp.name) / cut
            c = self.client()
            c.call("batch 61 31")
            c.call("flush")
            c.call("batch 61 ~")
            c.call("flush")
            c.call("batch 62 32")
            c.close()
            c = self.client(STRATA_FAILPOINT=cut)
            self.assertEqual(c.call("compact"), "")
            c.close()
            self.assertEqual(c.process.returncode, 86)
            c = self.client()
            self.assertEqual(c.ready, "READY 3")
            self.assertEqual(c.call("get 1 61"), "VALUE 31")
            self.assertEqual(c.call("get 3 61"), "MISSING")
            self.assertEqual(c.call("get 3 62"), "VALUE 32")
            c.close()

    def test_unacknowledged_synced_batch_may_survive_atomically(self):
        self.client().close()
        c = self.client(STRATA_FAILPOINT="wal_synced")
        self.assertEqual(c.call("batch 61 31 62 32"), "")
        c.close()
        self.assertEqual(c.process.returncode, 86)
        c = self.client()
        self.assertEqual(c.ready, "READY 1")
        self.assertEqual(c.call("scan 1 - ff"), "ROWS 2 61 31 62 32")

    def test_missing_segment_manifest_crc_and_sorted_record_corruption(self):
        c = self.client()
        c.call("batch 62 32 61 31")
        c.call("flush")
        c.close()
        sst = next(self.path.glob("*.sst"))
        original = sst.read_bytes()
        # Independent framing verifies key order, without the engine's decoder.
        records = []
        offset = 0
        while offset < len(original):
            (size,) = struct.unpack_from("<I", original, offset)
            payload = original[offset + 4 : offset + 4 + size]
            self.assertEqual(
                zlib.crc32(payload),
                struct.unpack_from("<I", original, offset + 4 + size)[0],
            )
            records.append(original[offset : offset + size + 8])
            offset += size + 8
        sst.write_bytes(b"".join(reversed(records)))
        broken = Client(self.path)
        self.assertIn("order", broken.close())
        sst.write_bytes(original[:-1])
        broken = Client(self.path)
        self.assertIn("torn segment", broken.close())
        sst.unlink()
        broken = Client(self.path)
        self.assertIn("missing referenced", broken.close())

    def test_invalid_input_does_not_commit(self):
        for command in (
            "batch 61 31 61 32",
            "batch",
            "batch gg 31",
            "get 2 61",
            "scan 0 ff 00",
        ):
            c = self.client()
            response = c.call(command)
            error = c.close()
            self.assertTrue(
                response.startswith("ERROR") or not response, (command, response, error)
            )
            self.assertIn("ERROR", response + error, (command, response, error))
            self.assertEqual(self.client().ready, "READY 0")
            self.clients[-1].close()

    def test_unicode_directory_and_maximum_value_size(self):
        self.path = Path(self.temp.name) / "合成-store"
        c = self.client()
        self.assertEqual(c.ready, "READY 0")
        value = b"\x00\xff" * (1024 * 512)
        self.assertEqual(c.call("batch - " + value.hex()), "OK 1")
        self.assertEqual(c.call("get 1 -"), "VALUE " + value.hex())
        c.call("flush")
        c.close()
        self.assertTrue((self.path / "MANIFEST").is_file())
        c = self.client()
        self.assertEqual(c.call("get 1 -"), "VALUE " + value.hex())

    def test_manifest_crc_schema_and_missing_active_wal_fail_closed(self):
        self.client().close()
        manifest = self.path / "MANIFEST"
        original = manifest.read_bytes()
        damaged = bytearray(original)
        damaged[-1] ^= 1
        manifest.write_bytes(damaged)
        broken = Client(self.path)
        self.assertIn("CRC mismatch", broken.close())
        payload = b"BAD!" + original[8:-4]
        manifest.write_bytes(
            struct.pack("<I", len(payload)) + payload + struct.pack("<I", zlib.crc32(payload))
        )
        broken = Client(self.path)
        self.assertIn("unsupported manifest", broken.close())
        manifest.write_bytes(original)
        next(self.path.glob("*.wal")).unlink()
        broken = Client(self.path)
        self.assertIn("missing referenced", broken.close())

    def test_valid_crc_bad_wal_schema_and_oversized_header_are_not_torn_tail(self):
        self.client().close()
        wal = next(self.path.glob("*.wal"))
        # Fully checksummed, structurally wrong batch, not a truncation.
        payload = b"B1" + struct.pack("<QI", 1, 1) + struct.pack("<II", 70000, 0)
        invalid = struct.pack("<I", len(payload)) + payload + struct.pack("<I", zlib.crc32(payload))
        wal.write_bytes(invalid)
        broken = Client(self.path)
        self.assertIn("invalid key/value size", broken.close())
        self.assertEqual(wal.read_bytes(), invalid)
        invalid = struct.pack("<I", 16 * 1024 * 1024 + 1)
        wal.write_bytes(invalid)
        broken = Client(self.path)
        self.assertIn("invalid frame length", broken.close())
        self.assertEqual(wal.read_bytes(), invalid)


if __name__ == "__main__":
    unittest.main()
