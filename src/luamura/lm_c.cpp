#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include "lm_codegen.h"
#include "lm_lua.h"

#include <iostream>
#include <string>

static int append(lua_State*, const void* data, size_t size, void* out) {
    static_cast<std::string*>(out)->append(static_cast<const char*>(data),
                                           size);
    return 0;
}

alignas(4) static const unsigned char lvm_bitcode[] = {
#embed "lvm.bc"
};

// The compiled code calls lvm.c's function for each instruction. Linking
// their bodies in lets LLVM inline them; liblua defines the rest.
static bool link_lvm(llvm::Module& module) {
    llvm::MemoryBufferRef buffer(
        llvm::StringRef(reinterpret_cast<const char*>(lvm_bitcode),
                        sizeof lvm_bitcode),
        "lvm.bc");
    auto lvm = llvm::parseBitcodeFile(buffer, module.getContext());
    if (!lvm) {
        llvm::logAllUnhandledErrors(lvm.takeError(), llvm::errs(),
                                    "luamura: ");
        return false;
    }
    for (llvm::Function& f : **lvm) {
        // llc chooses the target, and inlining needs them to agree
        f.removeFnAttr("target-cpu");
        f.removeFnAttr("target-features");
        f.removeFnAttr("tune-cpu");
        if (!f.isDeclaration() && !f.hasLocalLinkage())
            f.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
    }
    return !llvm::Linker::linkModules(module, std::move(*lvm),
                                      llvm::Linker::LinkOnlyNeeded);
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
    const Proto* main = clLvalue(s2v(L->top.p - 1))->p;
    std::string chunk;
    lua_dump(L, append, &chunk, 0);

    llvm::LLVMContext context;
    auto module = compile_module(context, main, chunk);
    lua_close(L);
    if (llvm::verifyModule(*module, &llvm::errs()) || !link_lvm(*module))
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
