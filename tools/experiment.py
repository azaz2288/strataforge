"""Synthetic flush/compaction/restart experiment; timings are this machine only."""

import argparse
import hashlib
import json
import os
import platform
import random
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def run(binary, batches=250):
    root = Path(__file__).resolve().parents[1]
    latencies = []
    with tempfile.TemporaryDirectory(prefix="strataforge-synthetic-") as folder:
        database = Path(folder) / "db"
        process = subprocess.Popen(
            [str(binary), str(database)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        def call(command):
            process.stdin.write(command + "\n")
            process.stdin.flush()
            response = process.stdout.readline().strip()
            if not response:
                raise RuntimeError(process.stderr.read())
            return response

        rng = random.Random(7931)
        expected = {}
        old = None
        old_at = None
        sequence = 0
        try:
            assert process.stdout.readline().strip() == "READY 0"
            for i in range(batches):
                key = f"key-{rng.randrange(80):03}".encode()
                value = rng.randbytes(80)
                started = time.perf_counter_ns()
                assert call(f"batch {key.hex()} {value.hex()}") == f"OK {i + 1}"
                latencies.append((time.perf_counter_ns() - started) / 1e6)
                expected[key] = value
                sequence = i + 1
                if i == batches // 2:
                    old = dict(expected)
                    old_at = sequence
                if i % 25 == 24:
                    assert call("flush").startswith("OK ")
            before = sum(p.stat().st_size for p in database.glob("*.sst"))
            started = time.perf_counter_ns()
            assert call("compact").startswith("OK ")
            compact_ms = (time.perf_counter_ns() - started) / 1e6
            after = sum(p.stat().st_size for p in database.glob("*.sst"))
            for snapshot, model in ((sequence, expected), (old_at, old)):
                rows = call(f"scan {snapshot} - ff").split()
                actual = {
                    bytes.fromhex(rows[i]): bytes.fromhex(rows[i + 1])
                    for i in range(2, len(rows), 2)
                }
                assert actual == model
            process.kill()
            process.wait(timeout=10)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()
        started = time.perf_counter_ns()
        reopened = subprocess.run(
            [str(binary), str(database)],
            input=f"scan {sequence} - ff\nquit\n",
            capture_output=True,
            text=True,
            check=True,
        )
        reopen_ms = (time.perf_counter_ns() - started) / 1e6
        rows = reopened.stdout.splitlines()[1].split()
        assert {
            bytes.fromhex(rows[i]): bytes.fromhex(rows[i + 1]) for i in range(2, len(rows), 2)
        } == expected
    source_digest = hashlib.sha256(
        b"".join((root / path).read_bytes() for path in ("src/engine.hpp", "src/main.cpp"))
    ).hexdigest()
    return {
        "schema": 1,
        "synthetic": True,
        "seed": 7931,
        "platform": platform.platform(),
        "python": platform.python_version(),
        "source_sha256": source_digest,
        "batches": batches,
        "mutations_per_batch": 1,
        "max_key_space": 80,
        "value_bytes": 80,
        "ack_latency_ms": {
            "median": statistics.median(latencies),
            "p95": sorted(latencies)[int(0.95 * (len(latencies) - 1))],
            "max": max(latencies),
        },
        "all_ack_latencies_ms": latencies,
        "compaction_ms": compact_ms,
        "segment_bytes_before": before,
        "segment_bytes_after": after,
        "restart_including_process_ms": reopen_ms,
        "oracle_and_old_snapshot_and_kill_reopen": "passed",
        "limits": [
            "not device-only fsync latency",
            "IPC and interpreter included",
            "all versions retained",
            "no power-loss or production-scale claim",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    report = run(root / "build" / ("strataforge.exe" if os.name == "nt" else "strataforge"))
    data = json.dumps(report, indent=2)
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(data + "\n")
    print(data)
