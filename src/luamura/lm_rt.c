#include "lstate.h"

#include "lauxlib.h"
#include "ldebug.h"
#include "lfunc.h"
#include "lualib.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// What compiled programs define.
extern const char lm_chunk[];
extern const size_t lm_chunk_size;
extern const lua_AotFunction lm_functions[];
extern const uint32_t lm_nfunctions;

static const char* progname = "luamura";

const Instruction* lm_code(CallInfo* ci) { return ci_func(ci)->p->code; }

int32_t lm_resume(CallInfo* ci) {
    return (int32_t)(ci->u.l.savedpc - ci_func(ci)->p->code);
}

// The implementation to run ci with, or NULL to interpret it.
lua_AotFunction lm_compiled(lua_State* L, CallInfo* ci) {
    return L->hookmask ? NULL : ci_func(ci)->p->aot;
}

// Attaches the functions from 'next' on to p and its nested Protos, in
// preorder, as the compiler emitted them.
static bool attach(Proto* p, uint32_t* next) {
    if (*next >= lm_nfunctions)
        return false;
    p->aot = lm_functions[(*next)++];
    for (int i = 0; i < p->sizep; i++)
        if (!attach(p->p[i], next))
            return false;
    return true;
}

static void report(lua_State* L, int status) {
    if (status == LUA_OK)
        return;
    fprintf(stderr, "%s: %s\n", progname, lua_tostring(L, -1));
    fflush(stderr);
    lua_pop(L, 1);
}

// As the message handler of lua.c.
static int message(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") &&
            lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)",
                              luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// As lua.c running a script: arg[0] is the script, arg[-1] the program.
static int run(lua_State* L) {
    int argc = (int)lua_tointeger(L, 1);
    char** argv = (char**)lua_touserdata(L, 2);
    luaL_checkversion(L);
    luaL_openlibs(L);
    lua_gc(L, LUA_GCRESTART);
    lua_gc(L, LUA_GCGEN);

    int status =
        luaL_loadbufferx(L, lm_chunk, lm_chunk_size, progname, "b");
    if (status != LUA_OK) {
        report(L, status);
        return 0;
    }
    Proto* main = clLvalue(s2v(L->top.p - 1))->p;
    uint32_t attached = 0;
    if (!attach(main, &attached) || attached != lm_nfunctions)
        return luaL_error(L, "compiled functions do not match the chunk");

    const char* script = getstr(main->source);
    if (*script == '@' || *script == '=')
        script++;
    lua_createtable(L, argc - 1, 2);
    lua_pushstring(L, argv[0]);
    lua_rawseti(L, -2, -1);
    lua_pushstring(L, script);
    lua_rawseti(L, -2, 0);
    for (int i = 1; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");

    int base = lua_gettop(L);
    lua_pushcfunction(L, message);
    lua_insert(L, base);
    luaL_checkstack(L, argc, "too many arguments to script");
    for (int i = 1; i < argc; i++)
        lua_pushstring(L, argv[i]);
    status = lua_pcall(L, argc - 1, LUA_MULTRET, base);
    report(L, status);
    lua_pushboolean(L, status == LUA_OK);
    return 1;
}

int main(int argc, char** argv) {
    lua_State* L = luaL_newstate();
    if (L == NULL) {
        fprintf(stderr, "%s: cannot create state: not enough memory\n",
                progname);
        return 1;
    }
    lua_gc(L, LUA_GCSTOP);
    lua_pushcfunction(L, run);
    lua_pushinteger(L, argc);
    lua_pushlightuserdata(L, argv);
    int status = lua_pcall(L, 2, 1, 0);
    int result = lua_toboolean(L, -1);
    report(L, status);
    lua_close(L);
    return result && status == LUA_OK ? 0 : 1;
}
