"""A small, append-only JSON database of completed AWFY runs."""

import fcntl
import json
from pathlib import Path
import statistics
import subprocess


def code_bytes(executable):
    sections = subprocess.check_output(
        ["size", "-A", str(executable)], text=True)
    for line in sections.splitlines():
        fields = line.split()
        if fields and fields[0] == ".text":
            return int(fields[1])
    raise ValueError(f"no .text section in {executable}")


def record(path, metadata, results, *, baseline=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_suffix(".lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        history = json.loads(path.read_text()) if path.exists() else []
        if baseline and history:
            raise ValueError("--baseline requires an empty history database")
        common = {key: metadata[key] for key in (
            "started_at", "finished_at", "git_head", "commit_date",
            "hostname", "platform", "affinity", "runs", "target_seconds",
            "suite", "tools", "runner_sha256")}
        common["cpu"] = next(
            (line.split(":", 1)[1].strip()
             for line in metadata["cpuinfo"].splitlines()
             if line.startswith("model name")), "unknown")
        for engine in (["lua", "aot"] if baseline else ["aot"]):
            entry = dict(common, engine=engine,
                         label="Lua 5.5.1" if engine == "lua"
                         else metadata["revision"])
            entry["benchmarks"] = {
                result["name"]: {
                    "seconds": statistics.median(result["samples"][engine])
                    / result["iterations"],
                    "samples": [t / result["iterations"]
                                for t in result["samples"][engine]],
                    "code_bytes": result["code_bytes"][engine],
                    "binary_bytes": result["binary_bytes"][engine],
                    "iterations": result["iterations"],
                    "inner_iterations": result["inner_iterations"],
                    "source_sha256": result["source_sha256"],
                } for result in results}
            history.append(entry)
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(history, separators=(",", ":")) + "\n")
        temporary.replace(path)
