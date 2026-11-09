#ifndef LUAMURA_H
#define LUAMURA_H

#include "lstate.h"

#include <stdint.h>

/*
 * [LMDOC] Luamura
 * 
 * Luamura is an AOT Baseline compiler for lua. The primary goal is to
 * eliminate the interpreters dispatch overhead. Luamura achieves this by
 * applying the first Futamura projection, deriving a compiled binary by
 * specializing the interpreter to a fixed bytecode.
 *
 * A set of native, dispatch erased functions are emited into a binary which
 * links liblua. Native functions can, and often do, bail out or dispatch to
 * VM routines for garbage collection, debug handling, co-routine scheduling,
 * etc. This is what allows Luamura to be a compliant implementation with
 * relatively little effort. It is also limits the Baseline performance. All
 * dispatches into the VM are opaque, and make analysis imprecise. Moreover,
 * any VM call can mutate the stack, PC, etc, which forces native-compiled
 * code to be defensive around each call.
 *
 * To natively compile Lua, each function is a module is lexed, parsed and
 * lowered into bytecode. A control-flow-graph (CFG) is constructed from the
 * instructions in each function. The Baseline compiler receives the CFG and
 * iterates over its blocks. For each bytecode instruction within a block, the
 * IR builder emits a call to the respective op-handler function, marked with
 * the always-inline attribute. A straight-line sequence of Lua bytecode then
 * compiles down into each op-handler stitched together, with no dispatch
 * overhead.
 *
 * In order to make each op-handler visible, the interpreter is modified to
 * expose each op-handler as a function. This slows down the interpreter by
 * trading the computed-goto (threaded) dispatch for a slow, function call
 * between each operation.
 *
 *
 */

// What compiled code calls.
const Instruction* lm_code(CallInfo* ci);
int32_t lm_resume(CallInfo* ci);
lua_AotFunction lm_compiled(lua_State* L, CallInfo* ci);

// What compiled programs define.
extern const char lm_chunk[];
extern const size_t lm_chunk_size;
extern const lua_AotFunction lm_functions[];
extern const uint32_t lm_nfunctions;

#endif // LUAMURA_H
