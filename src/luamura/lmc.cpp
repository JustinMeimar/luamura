#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include "lmemit.h"
#include "lmlua.h"
#include "lmssa.h"

#include <cstring>
#include <iostream>
#include <string>

static int append(lua_State*, const void* data, size_t size, void* out) {
    static_cast<std::string*>(out)->append(static_cast<const char*>(data),
                                           size);
    return 0;
}

static void print_ssa(const Proto* p) {
    ssa::print(*ssa::lift(p), std::cout);
    for (int i = 0; i < p->sizep; i++)
        print_ssa(p->p[i]);
}

static void optimize(llvm::Module& module) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2)
        .run(module, mam);
}

int main(int argc, char* argv[]) {
    bool ssa = argc == 3 && std::strcmp(argv[1], "--ssa") == 0;
    if (argc != 3) {
        std::cerr << "usage: luamura <input.lua> <output.ll>\n"
                     "       luamura --ssa <input.lua>\n";
        return 1;
    }
    const char* input = ssa ? argv[2] : argv[1];
    lua_State* L = luaL_newstate();
    if (luaL_loadfile(L, input) != LUA_OK) {
        std::cerr << "luamura: " << lua_tostring(L, -1) << "\n";
        lua_close(L);
        return 1;
    }
    const Proto* main = clLvalue(s2v(L->top.p - 1))->p;
    if (ssa) {
        print_ssa(main);
        lua_close(L);
        return 0;
    }
    std::string chunk;
    lua_dump(L, append, &chunk, 0);

    llvm::LLVMContext context;
    auto module = emit_module(context, main, chunk);
    lua_close(L);
    if (llvm::verifyModule(*module, &llvm::errs()))
        return 1;
    optimize(*module);

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
