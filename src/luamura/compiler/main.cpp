#include "emit.h"
#include "lua_headers.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <string>

static int append(lua_State*, const void* data, size_t size, void* out) {
    static_cast<std::string*>(out)->append(static_cast<const char*>(data),
                                           size);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: luamura <input.lua> <output.ll>\n";
        return 1;
    }
    lua_State* L = luaL_newstate();
    if (luaL_loadfile(L, argv[1]) != LUA_OK) {
        std::cerr << "luamura: " << lua_tostring(L, -1) << "\n";
        lua_close(L);
        return 1;
    }
    std::string chunk;
    lua_dump(L, append, &chunk, 0);
    const Proto* main = clLvalue(s2v(L->top.p - 1))->p;

    llvm::LLVMContext context;
    auto module = emit_module(context, main, chunk);
    lua_close(L);
    if (llvm::verifyModule(*module, &llvm::errs()))
        return 1;

    std::error_code ec;
    llvm::raw_fd_ostream out(argv[2], ec);
    if (ec) {
        std::cerr << "luamura: cannot write " << argv[2] << ": "
                  << ec.message() << "\n";
        return 1;
    }
    module->print(out, nullptr);
    return 0;
}
