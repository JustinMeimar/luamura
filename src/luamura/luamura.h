#ifndef LUAMURA_H
#define LUAMURA_H

#include "lstate.h"
#include <stdint.h>

/* Compiled functions reuse Lua's opcode handlers and statically link
 * liblua. Calls, hooks and coroutine resumption share the VM's state.
 * The interpreter retains its normal dispatch loop.
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
