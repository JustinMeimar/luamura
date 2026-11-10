#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include "lm_codegen.h"
#include "lm_lua.h"

#include <cstdint>
#include <iostream>
#include <string>

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

namespace luamura {

#define LUAMURA_PRECOMP alignas(4) static const uint8_t

LUAMURA_PRECOMP lvm_bitcode[] = {
#embed "lvm.bc"
};

LUAMURA_PRECOMP ltable_bitcode[] = {
#embed "ltable.bc"
};

static bool linkBitcode(llvm::Module& module,
                        llvm::ArrayRef<uint8_t> bitcode,
                        llvm::StringRef name) {
    llvm::MemoryBufferRef buffer(
        llvm::StringRef(reinterpret_cast<const char*>(bitcode.data()),
                        bitcode.size()),
        name);
    auto linked = llvm::parseBitcodeFile(buffer, module.getContext());
    if (!linked) {
        llvm::logAllUnhandledErrors(linked.takeError(), llvm::errs(),
                                    "luamura: ");
        return false;
    }
    for (llvm::Function& f : **linked) {
        // llc chooses the target, and inlining needs them to agree
        f.removeFnAttr("target-cpu");
        f.removeFnAttr("target-features");
        f.removeFnAttr("tune-cpu");
        if (!f.isDeclaration() && !f.hasLocalLinkage())
            f.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
    }
    return !llvm::Linker::linkModules(module, std::move(*linked),
                                      llvm::Linker::LinkOnlyNeeded);
}

// The compiled code calls lvm.c's function for each instruction, and
// they call ltable.c's. Linking their bodies in lets LLVM inline them;
// liblua defines the rest. lvm.c comes first, so that only the table
// functions it needs are linked.
static bool linkLua(llvm::Module& module) {
    return linkBitcode(module, lvm_bitcode, "lvm.bc") &&
           linkBitcode(module, ltable_bitcode, "ltable.bc");
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

} // namespace luamura

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
    luamura::GuestBytecode bytecode;
    lua_dump(
        L,
        [](lua_State*, const void* data, size_t size, void* out) {
            auto* bytes = static_cast<const uint8_t*>(data);
            auto* bytecode = static_cast<luamura::GuestBytecode*>(out);
            bytecode->insert(bytecode->end(), bytes, bytes + size);
            return 0;
        },
        &bytecode, 0);

    llvm::LLVMContext context;
    auto module = luamura::compileModule(context, *main, bytecode);
    lua_close(L);
    if (llvm::verifyModule(*module, &llvm::errs()) ||
        !luamura::linkLua(*module))
        return 1;
    luamura::optimize(*module);

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
