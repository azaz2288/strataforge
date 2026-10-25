"""Always compile current sources, then run independent protocol/recovery tests."""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build(sanitize=False):
    (ROOT / "build").mkdir(exist_ok=True)
    output = ROOT / "build" / ("strataforge.exe" if os.name == "nt" else "strataforge")
    native = shutil.which("g++") or shutil.which("clang++")
    zig = os.environ.get("STRATA_ZIG") or shutil.which("zig")
    fallback = (
        ROOT.parent.parent / "maintenance/career-lab/toolchain/zig-windows-x86_64-0.13.0/zig.exe"
    )
    if os.environ.get("STRATA_ZIG"):
        compiler = [os.environ["STRATA_ZIG"], "c++"]
    elif native:
        compiler = [native]
    elif zig or fallback.is_file():
        compiler = [zig or str(fallback), "c++"]
    else:
        raise SystemExit("Need GCC/Clang C++20 or STRATA_ZIG pointing to Zig")
    flags = [
        "-std=c++20",
        "-O1" if sanitize else "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    ]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if os.name == "nt":
        flags += ["-municode"]
    subprocess.run([*compiler, *flags, "src/main.cpp", "-o", str(output)], cwd=ROOT, check=True)
    api_output = output.with_name("api-test.exe" if os.name == "nt" else "api-test")
    api_flags = [flag for flag in flags if flag != "-municode"]
    subprocess.run(
        [*compiler, *api_flags, "tests/api.cpp", "-o", str(api_output)],
        cwd=ROOT,
        check=True,
    )
    with tempfile.TemporaryDirectory(prefix="strata-api-") as folder:
        subprocess.run([str(api_output), str(Path(folder) / "db")], check=True)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    binary = build(args.sanitize)
    environment = dict(os.environ, STRATA_BINARY=str(binary))
    subprocess.run(
        [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v"],
        cwd=ROOT,
        env=environment,
        check=True,
    )
