
                    .---===~ Luamura ~===---.

            A Data Driven Approach to Interpretation.

This tree is Lua 5.5.1 (README, doc/, src/) with luamura in src/luamura:
a compiler from Lua bytecode to LLVM IR, and the runtime its programs
link. src/luamura/runtime/luamura.h describes the execution model.

    cmake -S . -B build && cmake --build build
    ctest --test-dir build -L e2e

    build/luamura app.lua app.ll
    llc app.ll -filetype=obj --relocation-model=pic -o app.o
    cc app.o build/libluamura_rt.a build/liblua.a -Wl,-E -lm -ldl -o app

|=------------------------------------------------------------------=|


--[ Table of Contents

F - Problem Statement
0 - Introduction
1 - The Art of Interpretation
2 - Challenges Imposed by WASM
3 - Design
    - Erasure through AOT Baseline Compilation
    - A Bounded AOT IC Corpus
    - Optimizations without Static Types
    - Parametric NaN Boxing for Cheap IC Guards

--[ Problem Statement

Demand for JITless execution of dynamic languages is growing due to PaaS who
chose WASM as a compilation target for it's hardened sandbox. My goal is to
make an AOT Lua compiler which demonstrates we don't need a JIT. We can get
good performance by representing the program in some IR, performing compiler
optimizations like LICM, GVN, CSE, ... <Find others>, which don't care if
the values don't have types, then perform a Baseline compilation which
contains IC dispatch for any generic opcode which could be type specialized.
There is no interpreter overhead for a Baseline compiled program. To let the
program specialize to types dynamically, we pre-compile the set of all
likely IC stubs, conditioned by some hybrid type analysis which returns a
set of likely Shapes used in the program. We design the IC system to use an
extremely RISC-esque IR to canonicalize property access etc, and exhibits
a strict separation of code and data, making AOT compilation conceivable.
Prior work in AmberMonkey has shown that AOT IC corpi can be formulated.

--[ Introduction

Dynamic language implementations have had their performance bolstered
through Just-in-Time (JIT) compilation. JITs enable classical compiler
optimizations to be applied in a dynamic setting through type-profiling:
Once a programs dynamic types are realized, a speculative compilation can
provide a fast-native implementation of that region, be it a trace, method,
or specialized op-code. Any speculative optimization of the source, however,
must include bail-outs, or guards, to return to generic interpretation when
type assumptions fail.

This paradigm of dynamic language execution was first established by
TraceMonkey in 2007, and has established its effectiveness through adoption
in various languages, such as Lua, Python and Ruby. While JIT compilation
can be expected to remain the predominant technique in it's current most
established areas: Browsers, heavy server-side workloads, etc, the field of
Platform-as-a-Serivce (PaaS) has been growing in recent years, and imposes a
unique set of constraints for dynamic language execution.

First, PaaS providers are highly concerned about security. JIT compilation
requires on dynamic executable memory, which increases the executable
footprint of a program, during runtime, by mapping JIT compiled data buffers
from the heap into executable code. This practice makes offline security
techniques such as Control-Flow-Integrity (CFI), address sanitization, etc
difficult, complicated by the high engineering complexity of a JIT, which
must maintain semantically correct program states across a varienty of
representations in a tiered JIT, and amidst the high complexity of
JavaScript as a specification. Altogether, executing untrusted JIT code from

PasS gravitate towards Sandboxes. One platform, doubling as a compilation
target, which included a hardened sandbox from it's inception is WebAssmebly
(WASM). The Sanbox of WASM comes at a cost. All executable memory must be
declared up front. The WASM platform presents an immediate challenge
languages relying on JIT compilation to achieve native performance.

--[ The Art of Interpretation

- Why we like interpreters: Portability, Debug-ability, Simple to
  profile for performance.

- Interpreter Dispatch Overhead. A naive switch statement interpreter is
  slow, with one switch-case per op-code, dispatching to the respective
  handler. We might hope the compiler optimizes the long switch into a jump
  table, which it can do if the switch-argument is a POD, however, there is
  still significant overhead in _paying the C++ calling convention overhead_
  to push a new frame, call into the handler, then return out. A threaded
  dispatch interpreter manages a jump table itself, and it quite fast. All
  of the interpreter dispatch overhead reduces to N indirect jumps, one
  at the bottom of each op-code handler.

--[ Challenges Imposed by WASM

All executable memory must be declared up front. The obivious JIT tier we
can apply offline is Baseline compilation, since it merely stitches together
the CFG of the guest program with generic op-code implementations in native
code. We are constrained to do type-independent optimizations. In a JITless
setting with relaxed compilation time, Baseline compilation is extended to
do all forms of type-independent optimizations. LICM, GVN, CSE, etc, should
all be applicable on an IR in which values may annotated with a type, set of
plausible types, or unknown type.

--[ Design

The preliminary question we ask ourselves is: Can all op-local type
specializations be captured with a finite corpus? The corpus may be
generated at compile time, so would be allowed to use static hybrid type
inference. How many Shapes/Hidden-Classes/Maps do we need to provide almost
full coverage for an average program? 64, 128, 256, 512?

- Any program can generate a eager attachment map. A sequence of hints
  formed as (ICSite, ICStub). AOT Baseline compiler can setup this data
  before program even runs.

- Can we use Parametric Nan Boxing, with a custom key for every single
  Shape observed during compilation, and or additional likely shapes from
  an offline prior, to enable extremely fast IC guard lookups, and hence
  arbitrary megamorphism?

  Suppose each IC site was a lookup table instead of a chain, wherein the
  Shape is the key, and we simply see if we have a stub for this operator
  shape combination. It is O(1) lookup to see if there is a stub at a 
  particular IC site for the dynamic object(s) flowing through.


