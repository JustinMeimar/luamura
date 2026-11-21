#ifndef LM_CODEGEN_H
#define LM_CODEGEN_H
#include "lm_cfg.h"
#include "lm_lua.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>
#include <vector>

namespace luamura {

// The baseline compiler. Each function of the chunk becomes a native
// function that runs its instructions on the Lua stack, with lvm.c's
// function for each instruction.
std::unique_ptr<llvm::Module> compileModule(llvm::LLVMContext& context,
                                            const Proto& main,
                                            const std::string& chunk);

// What luaV_execute keeps while running a function.
struct Frame {
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    // the function's instructions, and the address of its pc
    llvm::Value* code = nullptr;
    llvm::Value* pc = nullptr;
};

// The Futamura projection of a block: luaV_execute specialized to the
// block's instructions. Each instruction calls lvm.c's function for it
// with the instruction as a constant, and LLVM inlines the call. What
// remains is the interpreter's work for that instruction alone, without
// fetching, decoding or dispatch.
class BlockCompiler {
public:
    BlockCompiler(llvm::Module& module, llvm::IRBuilder<>& builder,
                  const Proto& proto, const Frame& frame);

    // Returns what the block's last instruction returned: the next call
    // for luaV_execute to run, if any.
    llvm::Value* compile(const CFG::Block& block);

private:
    llvm::Module& module;
    llvm::IRBuilder<>& builder;
    llvm::FunctionType* op_handler_ty;
    const Proto& proto;
    Frame frame;
};

// Compiles a Lua function to a native function whose control flow
// mirrors its CFG: an LLVM block for each of the CFG's blocks, ending
// where the interpreter would continue.
class FunctionCompiler {
public:
    FunctionCompiler(llvm::Module& module, const Proto& proto);

    llvm::Function* compile(const llvm::Twine& name);

private:
    void enter();
    void terminate(const CFG::Block& block, llvm::Value* next);
    void toSuccessors(const CFG::Block& block);
    void afterCall(llvm::Value* next, llvm::BasicBlock* successor);
    void tailCall(llvm::Value* next);
    void returnIf(llvm::Value* condition, llvm::Value* value);
    llvm::BasicBlock* createBlock(const llvm::Twine& name = "");

    llvm::Module& module;
    llvm::LLVMContext& context;
    llvm::IRBuilder<> builder;
    llvm::PointerType* ptr_ty;
    llvm::IntegerType* i32_ty;
    llvm::IntegerType* instruction_ty;
    llvm::FunctionType* aot_function_ty;
    llvm::FunctionCallee lm_code;
    llvm::FunctionCallee lm_resume;
    llvm::FunctionCallee lm_compiled;

    const Proto& proto;
    CFG cfg;
    llvm::Function* function = nullptr;
    Frame frame;
    // the LLVM block for each of cfg's blocks
    std::vector<llvm::BasicBlock*> llvm_blocks;
    // returns to luaV_execute, to interpret from the saved pc
    llvm::BasicBlock* interpret = nullptr;
};

} // namespace luamura

#endif // LM_CODEGEN_H
