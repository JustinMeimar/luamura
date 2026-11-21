#ifndef LM_LUA_H
#define LM_LUA_H

// Lua defines a function-like macro 'cast', which LLVM declares as a
// function. Declaring LLVM's first leaves only its calls, cast<T>(x),
// which the macro does not match.
#include <llvm/Support/Casting.h>

extern "C" {
#include "lauxlib.h"
#include "lfunc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lua.h"
}

#endif // LM_LUA_H
