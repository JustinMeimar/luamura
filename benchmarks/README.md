# AWFY

From the repository root:

```sh
python3 benchmarks/run.py --revision ssa --record
(cd benchmarks && python3 -m http.server 8765 --bind 127.0.0.1)
```

Open http://localhost:8765/ to plot `history.json`. Each run records its
commit, UTC time, machine, tool versions, and samples. Revision builds use
a committed snapshot, excluding working-tree edits.

The history starts with Lua 5.5.1 (t=0), main, then ssa. To seed a new
database, use `--revision main --record --baseline --database path.json`.

All 14 vendored AWFY workloads run correctness checks, warm up, then take
seven wall-time samples. Plots show median time per outer repetition;
bars show the sample range. Compilation is excluded. Use
`--benchmarks Bounce NBody`, `--runs 15`, or `--cpu 2` to adjust a run.
Compare timings on an idle machine with matching settings.

The metric dropdown switches to native code size (.text, including Lua).
Code and full binary sizes are saved with every run.

Builds and detailed results stay in `build/benchmarks/`. The suite's
revision and license are in `awfy/upstream.json` and `awfy/LICENSE.md`.
