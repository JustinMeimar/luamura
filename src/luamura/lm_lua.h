#ifndef LM_LUA_H
#define LM_LUA_H

// Lua defines function-like macros 'cast' and 'G', names LLVM uses.
// Including LLVM's headers first leaves only uses the macros do not
// match, such as cast<T>(x).
#include <llvm/Support/Casting.h>
#include <llvm/Support/GraphWriter.h>

extern "C" {
#include "lauxlib.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lua.h"
}

#endif // LM_LUA_H
