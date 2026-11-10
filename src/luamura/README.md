# LmIR

`LmIR` represents one Lua function. It owns blocks, bytecode nodes, and
SSA values. Blocks own predecessor/successor lists and ordered node IDs;
there is no separate CFG. Blocks have stable addresses. Nodes and values
have append-only IDs, so references into their vectors must not survive
insertion. The source `Proto` must outlive its LmIR.

Construction has four steps:

- Build reachable blocks and native entry edges.
- Build bytecode nodes with register inputs, outputs, and effects.
- Insert phis using register liveness and LLVM's iterated dominance frontier.
- Rename register uses and definitions along the dominator tree.

`buildBytecodeNode` decodes an opcode's register accesses directly into its
node. Moves, loads and boolean tests have explicit accesses. Operations
whose effects are not yet modeled conservatively read and redefine the
fixed frame, including slots reachable through open upvalues. The handlers
still own variable results, stack-top changes, errors and yields.

Block zero represents native entry dispatch, including resumption after
calls. Those edges prevent definitions before a call from incorrectly
dominating a native continuation. GraphTraits exposes forward and reverse
traversal to LLVM's graph algorithms.

## Passes

The local `PassManager` is an ordered list of functions taking `LmIR&`.
`add` appends a pass; `run` invokes each pass in order. There is no registry,
analysis cache, invalidation protocol, or verifier. Passes compute the
analyses they need. The default pipeline is empty.

LLVM's own optimization pipeline still runs on the lowered LLVM module.

## Lowering

Baseline lowering takes a whole LmIR, then emits its blocks and bytecode
handlers. SSA currently describes the values in Lua's frame; lowering does
not yet keep them in native registers. Before adding operand-rewriting
passes, lowering must honor those operands and materialize them in the
handler's input slots. An entry value for an unused slot must not become an
unconditional load of an uninitialized Lua value.

To inspect LmIR before lowering:

```sh
LUAMURA_DUMP_IR=1 build/release/luamura input.lua /tmp/output.ll
```

The dump goes to stderr. Run the existing Lua suites with:

```sh
make -C src/luamura -j6 check
```
