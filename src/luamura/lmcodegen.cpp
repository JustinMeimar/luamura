#include "lmcodegen.h"
#include "lmcfg.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <vector>

extern "C" {
#include "lopnames.h"
}

namespace {

// The LLVM types compiled code works with.
struct Types {
    explicit Types(llvm::LLVMContext& context)
        : ptrTy(llvm::PointerType::getUnqual(context)),
          i32Ty(llvm::Type::getInt32Ty(context)),
          i64Ty(llvm::Type::getInt64Ty(context)) {}

    llvm::PointerType* ptrTy;
    llvm::IntegerType* i32Ty;
    llvm::IntegerType* i64Ty;
};

class FunctionCompiler {
public:
    FunctionCompiler(llvm::Module& module, const Types& types,
                     const Proto* proto)
        : module(module), types(types), proto(proto), cfg(proto),
          builder(module.getContext()) {}

    llvm::Function* compile(const std::string& name);

private:
    llvm::Value* run(int pc);
    void terminate(const CFG::Block& block, llvm::Value* next);
    void to_successors(const CFG::Block& block);
    void after_call(llvm::Value* next, llvm::BasicBlock* successor);
    void tail_call(llvm::Value* next);
    llvm::BasicBlock* new_block(const std::string& name) {
        return llvm::BasicBlock::Create(module.getContext(), name,
                                        function);
    }
    llvm::FunctionCallee runtime(const std::string& name,
                                 llvm::Type* result,
                                 llvm::ArrayRef<llvm::Type*> params) {
        return module.getOrInsertFunction(
            name, llvm::FunctionType::get(result, params, false));
    }

    llvm::Module& module;
    const Types& types;
    const Proto* proto;
    CFG cfg;
    llvm::IRBuilder<> builder;
    llvm::Function* function = nullptr;
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    llvm::Value* code = nullptr;
    llvm::Value* pc_address = nullptr;
    llvm::BasicBlock* interpret = nullptr;
    // one for each of cfg's blocks
    std::vector<llvm::BasicBlock*> blocks;
};

llvm::Function* FunctionCompiler::compile(const std::string& name) {
    auto* ptrTy = types.ptrTy;
    function = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false),
        llvm::GlobalValue::InternalLinkage, name, module);
    L = function->getArg(0);
    ci = function->getArg(1);

    auto* entry = new_block("entry");
    interpret = new_block("interpret");
    for (const CFG::Block& b : cfg.blocks)
        blocks.push_back(new_block("pc" + std::to_string(b.start)));

    builder.SetInsertPoint(interpret);
    builder.CreateRet(ci);

    builder.SetInsertPoint(entry);
    pc_address = builder.CreateAlloca(ptrTy, nullptr, "pc");
    code = builder.CreateCall(runtime("lm_code", ptrTy, {ptrTy}), {ci});
    auto* saved = builder.CreateCall(
        runtime("lm_resume", types.i32Ty, {ptrTy}), {ci});
    auto* resume = builder.CreateSwitch(saved, interpret);
    for (int e : cfg.entries)
        resume->addCase(builder.getInt32(cfg.blocks[e].start), blocks[e]);

    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        builder.SetInsertPoint(blocks[b]);
        llvm::Value* next = nullptr;
        for (int pc = cfg.blocks[b].start; pc < cfg.blocks[b].end; pc++)
            next = run(pc);
        terminate(cfg.blocks[b], next);
    }
    return function;
}

// Calls lvm.c's function for the instruction, which LLVM inlines. With
// the instruction a constant, what remains is the interpreter's work for
// this instruction alone: the dispatch and decoding fold away.
llvm::Value* FunctionCompiler::run(int pc) {
    Instruction i = proto->code[pc];
    auto* ptrTy = types.ptrTy;
    // the interpreter's pc after fetching i
    builder.CreateStore(
        builder.CreateConstInBoundsGEP1_32(types.i32Ty, code, pc + 1),
        pc_address);
    auto* next = builder.CreateCall(
        runtime(std::string("luaV_OP_") + opnames[GET_OPCODE(i)], ptrTy,
                {ptrTy, ptrTy, ptrTy, types.i32Ty}),
        {L, ci, pc_address, builder.getInt32(i)});
    next->addFnAttr(llvm::Attribute::AlwaysInline);
    return next;
}

// Control goes where the interpreter would go after the block's last
// instruction, whose function returned next.
void FunctionCompiler::terminate(const CFG::Block& block,
                                 llvm::Value* next) {
    switch (GET_OPCODE(proto->code[block.last()])) {
    case OP_CALL:
        return after_call(next, blocks[block.successors.front()]);
    case OP_TAILCALL:
        return tail_call(next);
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
        builder.CreateRet(next);
        return;
    default:
        return to_successors(block);
    }
}

// The instruction left in pc which successor to continue at.
void FunctionCompiler::to_successors(const CFG::Block& block) {
    const std::vector<int>& targets = block.successors;
    if (targets.size() == 1) {
        builder.CreateBr(blocks[targets.front()]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        types.i32Ty, builder.CreateLoad(types.ptrTy, pc_address), code);
    auto* to = builder.CreateSwitch(moved, blocks[targets.front()]);
    for (int t : llvm::drop_begin(targets))
        to->addCase(builder.getInt64(cfg.blocks[t].start), blocks[t]);
}

// A Lua callee is left for luaV_execute to run. A C function returned
// here, and may have set a hook, such as with debug.sethook, which only
// the interpreter runs.
void FunctionCompiler::after_call(llvm::Value* next,
                                  llvm::BasicBlock* successor) {
    auto* returned = new_block("returned");
    auto* called = new_block("called");
    builder.CreateCondBr(builder.CreateIsNull(next), returned, called);
    builder.SetInsertPoint(called);
    builder.CreateRet(next);
    builder.SetInsertPoint(returned);

    // volatile, as signal handlers set it
    auto* hookmask = builder.CreateLoad(
        builder.getIntNTy(8 * sizeof(l_signalT)),
        builder.CreateInBoundsPtrAdd(
            L, builder.getInt64(offsetof(lua_State, hookmask))),
        true);
    builder.CreateCondBr(builder.CreateIsNotNull(hookmask), interpret,
                         successor);
}

// A Lua callee replaces this function in ci. Entering its native code
// as a tail call keeps the C stack from growing.
void FunctionCompiler::tail_call(llvm::Value* next) {
    auto* ptrTy = types.ptrTy;
    auto* callee = new_block("callee");
    auto* compiled = new_block("compiled");
    auto* done = new_block("done");
    builder.CreateCondBr(builder.CreateICmpEQ(next, ci), callee, done);
    builder.SetInsertPoint(done);
    builder.CreateRet(next);

    builder.SetInsertPoint(callee);
    auto* native = builder.CreateCall(
        runtime("lm_compiled", ptrTy, {ptrTy, ptrTy}), {L, ci});
    builder.CreateCondBr(builder.CreateIsNull(native), done, compiled);
    builder.SetInsertPoint(compiled);
    auto* call =
        builder.CreateCall(function->getFunctionType(), native, {L, ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
void compile_function(llvm::Module& module, const Types& types,
                      const Proto* p,
                      std::vector<llvm::Constant*>& functions) {
    std::string name = "lm_function_" + std::to_string(functions.size());
    functions.push_back(FunctionCompiler(module, types, p).compile(name));
    for (int i = 0; i < p->sizep; i++)
        compile_function(module, types, p->p[i], functions);
}

} // namespace

std::unique_ptr<llvm::Module> compile_module(llvm::LLVMContext& context,
                                             const Proto* main,
                                             const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    Types types(context);
    std::vector<llvm::Constant*> functions;
    compile_function(*module, types, main, functions);

    auto define = [&](llvm::Constant* value, const char* name) {
        new llvm::GlobalVariable(*module, value->getType(), true,
                                 llvm::GlobalValue::ExternalLinkage, value,
                                 name);
    };
    define(llvm::ConstantDataArray::getString(context, chunk, false),
           "lm_chunk");
    define(llvm::ConstantInt::get(types.i64Ty, chunk.size()),
           "lm_chunk_size");
    define(
        llvm::ConstantArray::get(
            llvm::ArrayType::get(types.ptrTy, functions.size()), functions),
        "lm_functions");
    define(llvm::ConstantInt::get(types.i32Ty, functions.size()),
           "lm_nfunctions");
    return module;
}
