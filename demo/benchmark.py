#!/usr/bin/env python3

import argparse
import json
import pathlib
import re
import shutil
import statistics
import subprocess
import tempfile


SOURCE = pathlib.Path(__file__).resolve().parent
EXPECTED_SYMBOLS = 16755
EXPECTED_MENUS = 2622
EXPECTED_EXPORTS = 16111


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path, help="path to a Release zxconfig binary")
    parser.add_argument("--repeat", type=int, default=3, help="samples per case (default: 3)")
    parser.add_argument("--timeout", type=float, default=60, help="seconds per invocation")
    parser.add_argument("--json", type=pathlib.Path, help="optional machine-readable results")
    args = parser.parse_args()
    if args.repeat < 1 or args.timeout <= 0:
        parser.error("--repeat and --timeout must be positive")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary does not exist: {binary}")
    timer = shutil.which("time", path="/usr/bin:/bin")
    if not timer:
        parser.error("GNU time is required for per-process RSS measurements")

    samples = {}

    def invoke(root, name, options, expected=0, diagnostic=None):
        timing = root / "timing.txt"
        command = [timer, "-f", "%e %U %S %M", "-o", str(timing), str(binary), *options]
        result = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=args.timeout)
        output = result.stdout + result.stderr
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}\n{output}")
        if diagnostic and diagnostic not in output:
            raise RuntimeError(f"{name}: missing diagnostic {diagnostic!r}\n{output}")
        # GNU time prepends an exit-status line for expected nonzero cases.
        wall, user, system, rss = timing.read_text().splitlines()[-1].split()
        samples.setdefault(name, []).append({
            "wall_seconds": float(wall), "user_seconds": float(user),
            "system_seconds": float(system), "peak_rss_kib": int(rss),
        })
        if expected == 0 and ("error:" in output or "error[" in output):
            raise RuntimeError(f"{name}: unexpected error diagnostic\n{output}")
        return output

    with tempfile.TemporaryDirectory(prefix="zxconfig-bench-") as temporary:
        for repetition in range(args.repeat):
            root = pathlib.Path(temporary) / str(repetition)
            shutil.copytree(SOURCE, root, ignore=shutil.ignore_patterns(
                "out", ".zxconfig.cache*", "__pycache__", "timing.txt"))
            output = invoke(root, "startup", ["--parse-only", "ZXConfig"])
            match = re.search(r"ok: (\d+) symbol\(s\), (\d+) menu\(s\)", output)
            if not match or tuple(map(int, match.groups())) != (EXPECTED_SYMBOLS, EXPECTED_MENUS):
                raise RuntimeError(f"workload size changed; update benchmark expectations\n{output}")

            invoke(root, "generate", ["--headless", "--generate", "ZXConfig"])
            symbols = json.loads((root / "out/config.json").read_text())["symbols"]
            assert len(symbols) == EXPECTED_EXPORTS
            assert symbols["Chain0256"]["value"] == 256
            assert symbols["BenchFailedProbe"]["value"] == "expected-fallback"
            assert 'BenchTarget_s390x' in (root / "out/config.h").read_text()
            assert not any("Arm64" in name for name in symbols)
            for name in ("config.h", "config.mk", "config.json", "config.cmake", "linker.ld"):
                assert (root / "out" / name).stat().st_size > 0, name

            invoke(root, "expert-preset", ["--headless", "--preset", "expert", "ZXConfig"])
            invoke(root, "architecture-edits", [
                "--headless", "--set", "arch.BenchTarget=arm64",
                "--set", "arch.BenchTarget=s390x", "--set", "arch.BenchTarget=ppc64le",
                "--set", "arch.BenchTarget=riscv64", "--set", "arch.BenchTarget=amd64", "ZXConfig",
            ])
            invoke(root, "dependency-edits", [
                "--headless", "--set", "BenchFastPath=false", "--set", "BenchFastPath=true",
                "--set", "arch.BenchCpuCount=64", "--set", "BenchProject=renamed-stress", "ZXConfig",
            ])
            invoke(root, "save", ["--headless", "--preset", "ci", "--save", "--file", "ZXConfig"])
            invoke(root, "cache-reload", ["--headless", "ZXConfig"], diagnostic="cache clean:")
            with (root / "arch.zx").open("a") as included:
                included.write("\n# benchmark staleness mutation\n")
            invoke(root, "stale-refusal", ["--headless", "ZXConfig"], diagnostic="cache was not applied")
            invoke(root, "stale-rescue", ["--headless", "--cache-force", "ZXConfig"], diagnostic="cache stale:")
            # Drop the isolated cache before preset/negative cases.
            (root / ".zxconfig.cache").unlink()
            invoke(root, "warnings", ["--headless", "--preset", "warnings", "ZXConfig"],
                   diagnostic="Stress warning")
            invoke(root, "invalid-validation", ["--headless", "--preset", "invalid", "--generate", "ZXConfig"],
                   expected=1, diagnostic="validation errors block")
            invoke(root, "recursive-limit", ["--parse-only", "negative/recursive.zx"],
                   expected=1, diagnostic="macro expansion nested deeper")
            invoke(root, "value-cycle", ["--parse-only", "negative/value-cycle.zx"],
                   expected=1, diagnostic="value cycle")
            print(f"Completed sample {repetition + 1}/{args.repeat}", flush=True)

    print(f"\n{EXPECTED_SYMBOLS:,} symbols; {EXPECTED_MENUS:,} menus (implicit root included)")
    print(f"{'case':24} {'median s':>10} {'max RSS MiB':>13}")
    for name, values in samples.items():
        seconds = statistics.median(row["wall_seconds"] for row in values)
        memory = max(row["peak_rss_kib"] for row in values) / 1024
        print(f"{name:24} {seconds:10.3f} {memory:13.1f}")
    if args.json:
        args.json.write_text(json.dumps({
            "binary": str(binary), "symbols": EXPECTED_SYMBOLS, "menus": EXPECTED_MENUS,
            "samples": samples,
        }, indent=2) + "\n")


if __name__ == "__main__":
    main()
