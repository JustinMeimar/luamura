#ifndef LUAMURA_H
#define LUAMURA_H

#include "lstate.h"

/*
 * [LMDOC] Execution model
 *
 * A compiled program is Lua plus native code. The compiler loads the
 * program with Lua, embeds its binary chunk, and compiles some of its
 * functions. At startup the runtime loads the chunk and sets the 'aot'
 * field of each compiled function's Proto to its implementation.
 *
 * Every function is an ordinary Lua function with an ordinary CallInfo,
 * registers on the Lua stack and a saved pc. luaV_execute runs a
 * compiled function by calling its implementation instead of
 * interpreting it, unless hooks are active:
 *
 *     CallInfo* f(lua_State* L, CallInfo* ci);
 *
 * which runs ci's function from ci->u.l.savedpc and returns the next
 * call for luaV_execute to run: a Lua function it called, its caller
 * after it returned, or NULL after returning from a fresh call. Calls
 * to Lua functions therefore return to luaV_execute, and the caller
 * later continues from its saved pc. An implementation resumes at the
 * function's start and after calls; it returns ci itself to have the
 * interpreter continue from any other pc, or when hooks have been set
 * meanwhile. A tail call to a compiled function enters its
 * implementation directly, as a tail call in C.
 *
 * Compiled code keeps registers on the Lua stack, and runs each
 * instruction with lvm.c's function for it, which LLVM inlines.
 *
 * Errors, the collector, debug information and coroutines behave as for
 * interpreted functions, because the state they inspect is the same.
 */

extern const char lm_chunk[];
extern const size_t lm_chunk_size;
extern const lua_AotFunction lm_functions[];
extern const int lm_nfunctions;

#endif // LUAMURA_H
