#include "lmcodegen.h"
#include "lmcfg.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <vector>

extern "C" {
#include "lopnames.h"
}

namespace {

class FunctionCompiler {
public:
    FunctionCompiler(llvm::Module& module, const Proto* proto)
        : module(module), proto(proto), builder(module.getContext()) {}

    llvm::Function* compile(const std::string& name);

private:
    void instruction(int pc);
    llvm::Value* run(int pc);
    void to_successors(int pc);
    void after_call(int pc, llvm::Value* next);
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
    const Proto* proto;
    llvm::IRBuilder<> builder;
    llvm::Function* function = nullptr;
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    llvm::Value* code = nullptr;
    llvm::Value* pc_address = nullptr;
    llvm::BasicBlock* interpret = nullptr;
    // the block starting at each pc, or null
    std::vector<llvm::BasicBlock*> blocks;
};

llvm::Function* FunctionCompiler::compile(const std::string& name) {
    auto* ptr = builder.getPtrTy();
    function = llvm::Function::Create(
        llvm::FunctionType::get(ptr, {ptr, ptr}, false),
        llvm::GlobalValue::InternalLinkage, name, module);
    L = function->getArg(0);
    ci = function->getArg(1);

    auto* entry = new_block("entry");
    interpret = new_block("interpret");
    std::vector<bool> leaders = cfg::leaders(proto);
    blocks.assign(leaders.size(), nullptr);
    for (size_t pc = 0; pc < leaders.size(); pc++)
        if (leaders[pc])
            blocks[pc] = new_block("pc" + std::to_string(pc));

    builder.SetInsertPoint(interpret);
    builder.CreateRet(ci);

    builder.SetInsertPoint(entry);
    pc_address = builder.CreateAlloca(ptr, nullptr, "pc");
    code = builder.CreateCall(runtime("lm_code", ptr, {ptr}), {ci});
    auto* saved = builder.CreateCall(
        runtime("lm_resume", builder.getInt32Ty(), {ptr}), {ci});
    std::vector<int> entries = cfg::entries(proto);
    auto* dispatch = builder.CreateSwitch(saved, interpret, entries.size());
    for (int pc : entries)
        dispatch->addCase(builder.getInt32(pc), blocks[pc]);

    for (int pc = 0; pc < proto->sizecode; pc++) {
        if (blocks[pc] != nullptr) {
            if (builder.GetInsertBlock()->getTerminator() == nullptr)
                builder.CreateBr(blocks[pc]);
            builder.SetInsertPoint(blocks[pc]);
        }
        instruction(pc);
    }
    return function;
}

// Each instruction runs as the interpreter would run it, then control
// goes where the interpreter would go next.
void FunctionCompiler::instruction(int pc) {
    OpCode op = GET_OPCODE(proto->code[pc]);
    // an operand of the instruction before it, never run
    if (op == OP_EXTRAARG) {
        builder.CreateUnreachable();
        return;
    }

    llvm::Value* next = run(pc);

    switch (op) {
    case OP_CALL:
        return after_call(pc, next);
    case OP_TAILCALL:
        return tail_call(next);
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
        builder.CreateRet(next);
        return;
    default:
        return to_successors(pc);
    }
}

// Calls lvm.c's function for the instruction, which LLVM inlines. With
// the instruction a constant, what remains is the interpreter's work for
// this instruction alone: the dispatch and decoding fold away.
llvm::Value* FunctionCompiler::run(int pc) {
    Instruction i = proto->code[pc];
    auto* ptr = builder.getPtrTy();
    auto* i32 = builder.getInt32Ty();
    // the interpreter's pc after fetching i
    builder.CreateStore(
        builder.CreateConstInBoundsGEP1_32(i32, code, pc + 1), pc_address);
    auto* next = builder.CreateCall(
        runtime(std::string("luaV_OP_") + opnames[GET_OPCODE(i)], ptr,
                {ptr, ptr, ptr, i32}),
        {L, ci, pc_address, builder.getInt32(i)});
    next->addFnAttr(llvm::Attribute::AlwaysInline);
    return next;
}

// The instruction left in pc where to continue. Where it has a single
// successor, pc is not needed.
void FunctionCompiler::to_successors(int pc) {
    std::vector<int> targets = cfg::successors(proto, pc);
    if (targets.size() == 1) {
        if (targets[0] != pc + 1)
            builder.CreateBr(blocks[targets[0]]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        builder.getInt32Ty(),
        builder.CreateLoad(builder.getPtrTy(), pc_address), code);
    auto* to = builder.CreateSwitch(moved, blocks[targets[0]]);
    for (size_t n = 1; n < targets.size(); n++)
        to->addCase(builder.getInt64(targets[n]), blocks[targets[n]]);
}

// A Lua callee is left for luaV_execute to run. A C function returned
// here, and may have set a hook, such as with debug.sethook, which only
// the interpreter runs.
void FunctionCompiler::after_call(int pc, llvm::Value* next) {
    auto* returned = new_block("returned");
    auto* called = new_block("called");
    builder.CreateCondBr(builder.CreateIsNull(next), returned, called);
    builder.SetInsertPoint(called);
    builder.CreateRet(next);
    builder.SetInsertPoint(returned);
    // volatile, as signal handlers set it
    auto* hookmask = builder.CreateLoad(
        builder.getIntNTy(8 * sizeof(l_signalT)),
        builder.CreateConstInBoundsGEP1_64(builder.getInt8Ty(), L,
                                           offsetof(lua_State, hookmask)),
        true);
    builder.CreateCondBr(builder.CreateIsNotNull(hookmask), interpret,
                         blocks[pc + 1]);
}

// A Lua callee replaces this function in ci. Entering its native code
// as a tail call keeps the C stack from growing.
void FunctionCompiler::tail_call(llvm::Value* next) {
    auto* ptr = builder.getPtrTy();
    auto* callee = new_block("callee");
    auto* compiled = new_block("compiled");
    auto* done = new_block("done");
    builder.CreateCondBr(builder.CreateICmpEQ(next, ci), callee, done);
    builder.SetInsertPoint(done);
    builder.CreateRet(next);

    builder.SetInsertPoint(callee);
    auto* native = builder.CreateCall(
        runtime("lm_compiled", ptr, {ptr, ptr}), {L, ci});
    builder.CreateCondBr(builder.CreateIsNull(native), done, compiled);
    builder.SetInsertPoint(compiled);
    auto* call =
        builder.CreateCall(function->getFunctionType(), native, {L, ci});
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    builder.CreateRet(call);
}

// The runtime attaches the functions to the chunk's Protos in preorder.
void compile_function(llvm::Module& module, const Proto* p,
                      std::vector<llvm::Constant*>& functions) {
    std::string name = "lm_function_" + std::to_string(functions.size());
    functions.push_back(FunctionCompiler(module, p).compile(name));
    for (int i = 0; i < p->sizep; i++)
        compile_function(module, p->p[i], functions);
}

} // namespace

std::unique_ptr<llvm::Module> compile(llvm::LLVMContext& context,
                                      const Proto* main,
                                      const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    std::vector<llvm::Constant*> functions;
    compile_function(*module, main, functions);

    auto define = [&](llvm::Constant* value, const char* name) {
        new llvm::GlobalVariable(*module, value->getType(), true,
                                 llvm::GlobalValue::ExternalLinkage, value,
                                 name);
    };
    auto* ptr = llvm::PointerType::getUnqual(context);
    define(llvm::ConstantDataArray::getString(context, chunk, false),
           "lm_chunk");
    define(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                                  chunk.size()),
           "lm_chunk_size");
    define(llvm::ConstantArray::get(
               llvm::ArrayType::get(ptr, functions.size()), functions),
           "lm_functions");
    define(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                  functions.size()),
           "lm_nfunctions");
    return module;
}
