# Are We Fast Yet? on Lua and luamura

Run all 14 workloads from the Linux development environment:

```sh
python3 benchmarks/run.py
```

The suite is [Are We Fast Yet?](https://github.com/smarr/are-we-fast-yet),
vendored at revision `74306fec151070fd07157cefeacf19e7e0bcdc89`. The
`awfy/` directory contains unchanged upstream Lua sources, license and
attribution files, the upstream configuration, and `upstream.json` with
provenance. No download or third-party Python package is needed to run it.

The workloads are Bounce, CD, DeltaBlue, Havlak, Json, List, Mandelbrot,
NBody, Permute, Queens, Richards, Sieve, Storage, and Towers. They replace
our original five ad hoc microbenchmarks. AWFY covers programming
abstractions and algorithms; it is not a representative Lua application
corpus.

## Compilation and correctness

luamura currently compiles a single chunk and its nested functions. Simply
compiling AWFY's harness would leave modules loaded by `require` interpreted.
`awfy.py` therefore generates one compilation unit per workload:

- Each required Lua module becomes a `package.preload` function, preserving
  module scope, return values, and normal `require` caching.
- Five fixed bitwise helper definitions in `som.lua` and `richards.lua`
  become literal functions instead of functions created through `load`.
  Their bodies are unchanged. The original vendored files remain intact.
- File and C module searchers are disabled, as are `load`, `loadfile`, and
  `dofile`. Unexpected dynamic loading fails instead of introducing
  interpreted benchmark code. LuaJIT's optional `table.new` is unavailable.
- A small driver calls AWFY's `inner_benchmark_loop` and asserts its result.
  This retains upstream correctness checks while replacing the timing
  harness and its optional LuaSocket dependency.

Both engines receive exactly the same generated source. Before compilation,
plain Lua runs both the original modules and the bundle through the same
checking driver. Every warmup, calibration, and timed invocation also runs
AWFY's checks. Timed stdout must match the expected success message.

Problem sizes (inner iterations) are taken from upstream `rebench.conf`.
Calibration changes only the number of complete outer repetitions, so
verification sizes remain valid. Original module execution is checked at
one outer repetition; timed repetitions are checked by the bundle.

## Measurement

The runner makes fresh builds in a new directory under `build/benchmarks`.
The baseline is this tree's Lua 5.5.1 interpreter with LUAMURA disabled and
normal computed-goto dispatch. Compiled programs use the existing luamura
O2 pipeline and statically link its Lua runtime. Both runtime builds use
GCC at O2; opcode bitcode uses Clang at O2.

Each workload is calibrated to take at least 0.3 seconds in the interpreter.
The fixed upstream problem size may already take longer. Both engines then
run once untimed and seven times measured, alternating which runs first.
Each sample is a fresh process. The table reports median wall time and
interpreter time divided by compiled time; above 1 means luamura is faster.
Startup, parsing/loading, checks, and shutdown are included. Compilation is
excluded and recorded separately. This is not upstream's steady-state
measurement protocol; do not directly compare timings with its reports.

For a subset or longer measurements on an otherwise idle machine:

```sh
python3 benchmarks/run.py --benchmarks Richards DeltaBlue Json
python3 benchmarks/run.py --runs 15 --seconds 1 --cpu 2
```

Select a CPU allowed by your environment. Pinning does not isolate it from
other work or control frequency scaling. Inspect sample variability before
drawing conclusions; repeat experiments to assess stability. The default
duration may be too short when compiled code is much faster than Lua.

The output directory retains bundles, original sources, runner scripts, IR,
objects, executables, build logs, raw samples, compilation times, tool
versions, CPU data, Git state, and commands. `results.json` contains results;
`samples.csv` is suitable for further analysis. The Git diff excludes
untracked files, but the workload and runner copies preserve the experiment.

This compares only Lua and luamura. The generated bundles require Lua 5.3+
syntax and are not suitable for LuaJIT without a separate compatibility
adaptation. No compiler or runtime semantics are changed for the suite.
