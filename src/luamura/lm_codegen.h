#ifndef LM_CODEGEN_H
#define LM_CODEGEN_H

#include "lm_ir.h"
#include "lm_pass.h"
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

// What every compiler shares: the module, the builder inserting into
// it, and the LLVM types compiled code uses.
class BaselineCompiler {
public:
    BaselineCompiler(llvm::Module& module, llvm::IRBuilder<>& builder);

protected:
    llvm::Module& module;
    llvm::LLVMContext& context;
    llvm::IRBuilder<>& builder;

    llvm::PointerType* ptr_ty;
    llvm::IntegerType* i32_ty;
    llvm::IntegerType* i64_ty;
    llvm::IntegerType* instruction_ty;
    // lua_AotFunction
    llvm::FunctionType* aot_function_ty;
    // lvm.c's function for an instruction
    llvm::FunctionType* op_handler_ty;

    // the runtime functions compiled code calls
    llvm::FunctionCallee lm_code;
    llvm::FunctionCallee lm_resume;
    llvm::FunctionCallee lm_compiled;
};

// Compiles a Lua function to a native function whose control flow
// mirrors its IR: an LLVM block for each bytecode block, ending
// where the interpreter would continue.
class FunctionCompiler : BaselineCompiler {
public:
    FunctionCompiler(const BaselineCompiler& compiler, const LmIR& ir);

    llvm::Function* compile(const llvm::Twine& name);

private:
    llvm::Value* lowerBlock(const Block& block);
    llvm::Value* projectOpHandler(const Node& node);
    void enter();
    void resume(const Block& block);
    void terminate(const Block& block, llvm::Value* next);
    void toSuccessors(const Block& block);
    void afterCall(llvm::Value* next, llvm::BasicBlock* successor);
    void tailCall(llvm::Value* next);
    void returnIf(llvm::Value* condition, llvm::Value* value);
    llvm::BasicBlock* createBlock(const llvm::Twine& name = "");

    const Proto& proto;
    const LmIR& ir;
    llvm::Function* function = nullptr;
    Frame frame;
    // Indexed by IR block ID; the entry is the resume dispatch.
    std::vector<llvm::BasicBlock*> llvm_blocks;
    // returns to luaV_execute, to interpret from the saved pc
    llvm::BasicBlock* interpret = nullptr;
};

// Compiles a chunk's functions, and defines what the runtime loads.
class ModuleCompiler : BaselineCompiler {
public:
    using BaselineCompiler::BaselineCompiler;

    void compile(const Proto& main, const std::string& chunk);

private:
    void compileFunction(const Proto& proto);
    void define(const char* name, llvm::Constant* value);

    PassManager passes;
    std::vector<llvm::Constant*> functions;
};

} // namespace luamura

#endif // LM_CODEGEN_H
