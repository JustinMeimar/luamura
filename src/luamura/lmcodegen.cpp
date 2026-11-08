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
    void tail_call(llvm::Value* next);
    llvm::Value* hooked();
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

// lvm.c's function for the instruction runs it on the stack, and leaves
// in pc where the function continues. Inlined with the instruction as a
// constant, it reduces to what the interpreter does for that instruction
// alone. Calls and returns give the next call for luaV_execute to run.
void FunctionCompiler::instruction(int pc) {
    Instruction i = proto->code[pc];
    OpCode op = GET_OPCODE(i);
    if (op == OP_EXTRAARG) {
        builder.CreateUnreachable();
        return;
    }
    auto* ptr = builder.getPtrTy();
    builder.CreateStore(builder.CreateConstInBoundsGEP1_32(
                            builder.getInt32Ty(), code, pc + 1),
                        pc_address);
    auto* next = builder.CreateCall(
        runtime(std::string("luaV_OP_") + opnames[op], ptr,
                {ptr, ptr, ptr, builder.getInt32Ty()}),
        {L, ci, pc_address, builder.getInt32(i)});
    next->addFnAttr(llvm::Attribute::AlwaysInline);

    switch (op) {
    case OP_CALL: {
        // a C function returned here, and may have set hooks, which only
        // the interpreter runs
        auto* returned = new_block("returned");
        auto* called = new_block("called");
        builder.CreateCondBr(builder.CreateIsNull(next), returned, called);
        builder.SetInsertPoint(called);
        builder.CreateRet(next);
        builder.SetInsertPoint(returned);
        builder.CreateCondBr(hooked(), interpret, blocks[pc + 1]);
        return;
    }
    case OP_TAILCALL:
        tail_call(next);
        return;
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
        builder.CreateRet(next);
        return;
    default:
        break;
    }

    std::vector<int> targets = cfg::successors(proto, pc);
    if (targets.size() == 1) {
        if (targets[0] != pc + 1)
            builder.CreateBr(blocks[targets[0]]);
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        builder.getInt32Ty(), builder.CreateLoad(ptr, pc_address), code);
    auto* to =
        builder.CreateSwitch(moved, blocks[targets[0]], targets.size() - 1);
    auto* type = llvm::cast<llvm::IntegerType>(moved->getType());
    for (size_t n = 1; n < targets.size(); n++)
        to->addCase(llvm::ConstantInt::get(type, targets[n]),
                    blocks[targets[n]]);
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

llvm::Value* FunctionCompiler::hooked() {
    auto* mask = builder.CreateLoad(
        builder.getIntNTy(8 * sizeof(l_signalT)),
        builder.CreateConstInBoundsGEP1_64(builder.getInt8Ty(), L,
                                           offsetof(lua_State, hookmask)),
        true);
    return builder.CreateIsNotNull(mask);
}

void collect(const Proto* p, std::vector<const Proto*>& out) {
    out.push_back(p);
    for (int i = 0; i < p->sizep; i++)
        collect(p->p[i], out);
}

} // namespace

// The runtime attaches the functions to the chunk's Protos in preorder.
std::unique_ptr<llvm::Module> compile(llvm::LLVMContext& context,
                                      const Proto* main,
                                      const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    std::vector<const Proto*> protos;
    collect(main, protos);

    std::vector<llvm::Constant*> functions;
    for (size_t i = 0; i < protos.size(); i++)
        functions.push_back(
            FunctionCompiler(*module, protos[i])
                .compile("lm_function_" + std::to_string(i)));

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
