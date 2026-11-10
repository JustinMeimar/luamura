#ifndef LM_CODEGEN_H
#define LM_CODEGEN_H

#include "lm_ir.h"
#include "lm_lower.h"
#include "lm_lua.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <vector>

namespace luamura {

// Compiles a chunk's functions with the baseline backend.
std::unique_ptr<llvm::Module> compileModule(llvm::LLVMContext& context,
                                            const Proto& main,
                                            const GuestBytecode& bytecode);

// What luaV_execute keeps while running a function.
struct Frame {
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    // the function's instructions, and the address of its pc
    llvm::Value* code = nullptr;
    llvm::Value* pc = nullptr;
};

// The baseline backend. Each function becomes a native function whose
// control flow mirrors its IR, running each bytecode with lvm.c's
// function for it.
class BaselineLowerer : public LmIRLowerer {
public:
    BaselineLowerer(llvm::Module& module, const GuestBytecode& bytecode);

    using LmIRLowerer::lower;
    // Also defines what the runtime loads.
    void lower(const LmModule& module) override;
    void lower(const LmIR& ir) override;
    void lower(const Block& block) override;

private:
    void lowerFrameRead(const Node& node) override;
    void lowerPhi(const Node& node) override;
    void lowerBytecode(const Node& node) override;
    void lowerGuard(const Node& node) override;
    void lowerTerminator(const Block& block) override;

    bool isIntegerAdd(const Node& node) const;
    void lowerIntegerAdd(const Node& node);
    llvm::Value* projectOpHandler(const Node& node);
    llvm::Value* slotAddress(Slot slot, size_t field);
    llvm::Value* loadInteger(Slot slot);
    void enter();
    void resume(const Block& block);
    void toSuccessors(const Block& block);
    void afterCall(llvm::BasicBlock* successor);
    void tailCall();
    void returnIf(llvm::Value* condition, llvm::Value* value);
    llvm::BasicBlock* createBlock(const llvm::Twine& name = "");
    void define(const char* name, llvm::Constant* value);

    llvm::Module& module;
    llvm::LLVMContext& context;
    llvm::IRBuilder<> builder;
    const GuestBytecode& bytecode;

    llvm::PointerType* ptr_ty;
    llvm::IntegerType* i8_ty;
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

    std::vector<llvm::Constant*> functions;

    // the function being lowered
    const Proto* proto = nullptr;
    llvm::Function* function = nullptr;
    Frame frame;
    // Indexed by IR block ID; the entry is the resume dispatch.
    std::vector<llvm::BasicBlock*> llvm_blocks;
    // returns to luaV_execute, to interpret from the saved pc
    llvm::BasicBlock* interpret = nullptr;

    // the block being lowered, and what its last node returned
    const Block* block = nullptr;
    llvm::Value* next = nullptr;
};

} // namespace luamura

#endif // LM_CODEGEN_H
