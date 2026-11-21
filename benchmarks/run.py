#!/usr/bin/env python3
"""Build and compare Lua and luamura on the pinned AWFY suite."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import time

from awfy import BENCHMARKS, SOURCE, bundle, driver

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build" / "benchmarks" / time.strftime(
                            "%Y%m%d-%H%M%S"))
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--seconds", type=float, default=0.3)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--benchmarks", nargs="+", choices=BENCHMARKS,
                        default=list(BENCHMARKS))
    args = parser.parse_args()
    if args.runs < 2 or args.seconds <= 0:
        parser.error("use at least two runs and a positive duration")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("LUA_INIT", "LUA_PATH", "LUA_CPATH"))}
    commands = []

    def run(command, *, capture=True, cwd=ROOT):
        command = [str(part) for part in command]
        commands.append({"argv": command, "cwd": str(cwd)})
        start = time.perf_counter()
        try:
            result = subprocess.run(command, cwd=cwd, env=env, check=True,
                                    stdout=subprocess.PIPE if capture else log,
                                    stderr=subprocess.PIPE if capture else log,
                                    timeout=300)
        except subprocess.CalledProcessError as error:
            if capture:
                sys.stderr.buffer.write(error.stdout or b"")
                sys.stderr.buffer.write(error.stderr or b"")
            else:
                print(f"Build failed; see {output / 'build.log'}", file=sys.stderr)
            raise
        return time.perf_counter() - start, result.stdout

    with (output / "build.log").open("w") as log:
        stock = output / "lua-src"
        stock.mkdir()
        for source in (ROOT / "src").iterdir():
            if source.suffix in {".c", ".h"} or source.name == "Makefile":
                shutil.copy2(source, stock / source.name)
        run(["make", "-C", stock, "-j", "4", "linux"], capture=False)
        build = output / "luamura"
        run(["make", "-C", ROOT / "src/luamura", "-j", "4",
             f"BUILD={build}", "all"], capture=False)

        metadata = {
            "platform": platform.platform(),
            "cpuinfo": Path("/proc/cpuinfo").read_text(),
            "affinity": sorted(os.sched_getaffinity(0)),
            "runs": args.runs, "target_seconds": args.seconds,
            "suite": json.loads((SOURCE / "upstream.json").read_text()),
            "git_head": run(["git", "rev-parse", "HEAD"])[1].decode().strip(),
            "git_status": run(["git", "status", "--short"])[1].decode(),
            "tools": {tool: run([tool, "--version"])[1].decode().splitlines()[0]
                      for tool in ["gcc", "g++", "clang", "llc", "python3"]},
        }
        (output / "source.diff").write_bytes(run(["git", "diff", "HEAD"])[1])
        shutil.copytree(SOURCE, output / "awfy")
        for filename in ["run.py", "awfy.py"]:
            shutil.copy2(ROOT / "benchmarks" / filename, output / filename)
        results = []
        rng = random.Random(0)
        print(f"Results: {output}", flush=True)
        print("workload       Lua (s)   AOT (s)   speedup", flush=True)
        for name in args.benchmarks:
            print(f"Preparing {name}...", flush=True)
            source = output / f"{name}.lua"
            source.write_text(bundle(name))
            original = output / f"{name}-upstream.lua"
            original.write_text(driver(name))
            _, upstream = run([stock / "lua", original, 1], cwd=output / "awfy")
            _, bundled = run([stock / "lua", source, 1])
            if upstream != bundled:
                raise RuntimeError(f"bundling changed output: {name}")
            ir, obj, exe = [output / f"{name}{suffix}"
                            for suffix in [".ll", ".o", ".bin"]]
            compile_seconds, _ = run([build / "luamura", source, ir],
                                     capture=False)
            backend_seconds, _ = run(["llc", ir, "-filetype=obj", "-o", obj,
                                      "--relocation-model=pic"], capture=False)
            link_seconds, _ = run(["gcc", obj, build / "lm_rt.o",
                                   build / "liblua.a", "-Wl,-E", "-ldl",
                                   "-lm", "-pthread", "-o", exe], capture=False)
            engines = {"lua": [stock / "lua", source], "aot": [exe]}
            n = 1
            for _ in range(12):
                elapsed, expected = run([*engines["lua"], n])
                if elapsed >= args.seconds:
                    break
                n *= max(2, min(10, int(args.seconds / max(elapsed, 1e-6))))
            else:
                raise RuntimeError(f"could not calibrate {name}")
            for command in engines.values():
                _, actual = run([*command, n])
                if actual != expected:
                    raise RuntimeError(f"output mismatch: {name}")
            samples = {engine: [] for engine in engines}
            first = rng.choice(list(engines))
            for trial in range(args.runs):
                order = [first, "aot" if first == "lua" else "lua"]
                if trial % 2:
                    order.reverse()
                for engine in order:
                    elapsed, actual = run([*engines[engine], n])
                    if actual != expected:
                        raise RuntimeError(f"output mismatch: {name}/{engine}")
                    samples[engine].append(elapsed)
            medians = {engine: statistics.median(times)
                       for engine, times in samples.items()}
            speedup = medians["lua"] / medians["aot"]
            results.append({
                "name": name, "iterations": n, "samples": samples,
                "inner_iterations": BENCHMARKS[name],
                "median_seconds": medians, "speedup": speedup,
                "stdout": expected.decode(),
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "compile_seconds": compile_seconds,
                "backend_seconds": backend_seconds, "link_seconds": link_seconds,
                "executable_bytes": exe.stat().st_size,
            })
            print(f"{name:14} {medians['lua']:8.4f} {medians['aot']:9.4f}"
                  f" {speedup:8.2f}x", flush=True)
            (output / "results.json").write_text(json.dumps(
                {"metadata": metadata, "results": results,
                 "commands": commands}, indent=2) + "\n")
        with (output / "samples.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["workload", "engine", "trial", "seconds"])
            for result in results:
                for engine, samples in result["samples"].items():
                    for trial, elapsed in enumerate(samples):
                        writer.writerow([result["name"], engine, trial, elapsed])


if __name__ == "__main__":
    main()
