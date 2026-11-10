#ifndef LM_LUA_H
#define LM_LUA_H

// Lua defines function-like macros 'cast' and 'G', names LLVM uses.
// Including LLVM's headers first leaves only uses the macros do not
// match, such as cast<T>(x).
#include <llvm/Support/Casting.h>
#include <llvm/Support/GraphWriter.h>

#include <cstdint>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lua.h"
}

namespace luamura {

// The guest program's bytecode, as lua_dump writes it, which compiled
// programs embed for the runtime to load.
using GuestBytecode = std::vector<uint8_t>;

} // namespace luamura

#define LUAMURA_SWITCH(i) switch (GET_OPCODE(i))
#define LUAMURA_CASE(op) case OP_##op

#endif // LM_LUA_H
