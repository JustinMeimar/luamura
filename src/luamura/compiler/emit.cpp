#include "emit.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <vector>

extern "C" {
#include "lopnames.h"
}

namespace {

void collect(const Proto* p, std::vector<const Proto*>& out) {
    out.push_back(p);
    for (int i = 0; i < p->sizep; i++)
        collect(p->p[i], out);
}

void define(llvm::Module& module, llvm::Constant* value, const char* name) {
    new llvm::GlobalVariable(module, value->getType(), true,
                             llvm::GlobalValue::ExternalLinkage, value,
                             name);
}

bool is_call(OpCode op) {
    return op == OP_CALL || op == OP_TAILCALL || op == OP_RETURN ||
           op == OP_RETURN0 || op == OP_RETURN1;
}

// The distances an instruction's helper may move pc by, as luaV_execute
// moves it past the instruction. The first is the most common.
std::vector<int> moves(const Proto* p, int index) {
    Instruction i = p->code[index];
    OpCode op = GET_OPCODE(i);
    switch (op) {
    case OP_EQ:
    case OP_LT:
    case OP_LE:
    case OP_EQK:
    case OP_EQI:
    case OP_LTI:
    case OP_LEI:
    case OP_GTI:
    case OP_GEI:
    case OP_TEST:
    case OP_TESTSET: {
        int jump = GETARG_sJ(p->code[index + 1]) + 1;
        if (jump == 1)
            return {1};
        return {jump, 1};
    }
    case OP_FORLOOP:
    case OP_TFORLOOP:
        return {-GETARG_Bx(i), 0};
    case OP_FORPREP:
        return {0, GETARG_Bx(i) + 1};
    case OP_TFORPREP:
        return {GETARG_Bx(i)};
    case OP_LOADKX:
    case OP_NEWTABLE:
    case OP_LFALSESKIP:
        return {1};
    case OP_SETLIST:
        return {TESTARG_k(i) ? 1 : 0};
    default:
        if (op >= OP_ADDI && op <= OP_SHR)
            return {1, 0};
        return {0};
    }
}

class Emitter {
public:
    Emitter(llvm::Module& module)
        : module(module), context(module.getContext()), builder(context),
          t_ptr(llvm::PointerType::getUnqual(context)),
          t_i32(llvm::Type::getInt32Ty(context)) {}

    llvm::Function* emit(const Proto* p, size_t index);

private:
    llvm::FunctionCallee helper(OpCode op) {
        auto* result =
            is_call(op) ? static_cast<llvm::Type*>(t_ptr) : t_i32;
        auto* type = llvm::FunctionType::get(
            result, {t_ptr, t_ptr, t_ptr, t_i32}, false);
        return module.getOrInsertFunction(std::string("lm_") + opnames[op],
                                          type);
    }

    void emit_instruction(const Proto* p, int index);

    llvm::Module& module;
    llvm::LLVMContext& context;
    llvm::IRBuilder<> builder;
    llvm::PointerType* t_ptr;
    llvm::IntegerType* t_i32;

    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    llvm::Value* code = nullptr;
    std::vector<llvm::BasicBlock*> blocks;
};

// Runs from the saved pc, which is 0 on entry and the instruction after
// an interrupted one on resumption, so every instruction is a target.
llvm::Function* Emitter::emit(const Proto* p, size_t index) {
    auto* type = llvm::FunctionType::get(t_ptr, {t_ptr, t_ptr}, false);
    auto* function = llvm::Function::Create(
        type, llvm::GlobalValue::InternalLinkage,
        "lm_function_" + std::to_string(index), module);
    L = function->getArg(0);
    ci = function->getArg(1);

    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    auto* invalid = llvm::BasicBlock::Create(context, "invalid", function);
    blocks.clear();
    for (int i = 0; i < p->sizecode; i++)
        blocks.push_back(llvm::BasicBlock::Create(
            context, "pc" + std::to_string(i), function));

    builder.SetInsertPoint(invalid);
    builder.CreateUnreachable();

    builder.SetInsertPoint(entry);
    code = builder.CreateCall(
        module.getOrInsertFunction("lm_code", t_ptr, t_ptr), {ci});
    auto* pc = builder.CreateCall(
        module.getOrInsertFunction("lm_resume", t_i32, t_ptr), {ci});
    auto* resume = builder.CreateSwitch(pc, invalid, p->sizecode);
    for (int i = 0; i < p->sizecode; i++)
        resume->addCase(builder.getInt32(i), blocks[i]);

    for (int i = 0; i < p->sizecode; i++) {
        builder.SetInsertPoint(blocks[i]);
        emit_instruction(p, i);
    }
    return function;
}

void Emitter::emit_instruction(const Proto* p, int index) {
    Instruction i = p->code[index];
    OpCode op = GET_OPCODE(i);
    auto target = [&](int move) { return blocks[index + 1 + move]; };

    if (op == OP_JMP) {
        builder.CreateBr(target(GETARG_sJ(i)));
        return;
    }
    if (op == OP_EXTRAARG) {
        builder.CreateUnreachable();
        return;
    }

    auto* pc = builder.CreateConstInBoundsGEP1_32(t_i32, code, index + 1);
    auto* result =
        builder.CreateCall(helper(op), {L, ci, pc, builder.getInt32(i)});
    if (op == OP_CALL) {
        auto* run = llvm::BasicBlock::Create(
            context, "run", builder.GetInsertBlock()->getParent());
        builder.CreateCondBr(builder.CreateIsNull(result), target(0), run);
        builder.SetInsertPoint(run);
        builder.CreateRet(result);
        return;
    }
    if (is_call(op)) {
        builder.CreateRet(result);
        return;
    }

    std::vector<int> next = moves(p, index);
    if (next.size() == 1) {
        builder.CreateBr(target(next[0]));
        return;
    }
    auto* branch =
        builder.CreateSwitch(result, target(next[0]), next.size() - 1);
    for (size_t n = 1; n < next.size(); n++)
        branch->addCase(builder.getInt32(next[n]), target(next[n]));
}

} // namespace

std::unique_ptr<llvm::Module> emit_module(llvm::LLVMContext& context,
                                          const Proto* main,
                                          const std::string& chunk) {
    auto module = std::make_unique<llvm::Module>("luamura", context);
    std::vector<const Proto*> protos;
    collect(main, protos);

    auto* t_ptr = llvm::PointerType::getUnqual(context);
    auto* t_i32 = llvm::Type::getInt32Ty(context);
    auto* t_i64 = llvm::Type::getInt64Ty(context);

    Emitter emitter(*module);
    std::vector<llvm::Constant*> functions;
    for (size_t i = 0; i < protos.size(); i++)
        functions.push_back(emitter.emit(protos[i], i));

    define(*module,
           llvm::ConstantDataArray::getString(context, chunk, false),
           "lm_chunk");
    define(*module, llvm::ConstantInt::get(t_i64, chunk.size()),
           "lm_chunk_size");
    define(*module,
           llvm::ConstantArray::get(
               llvm::ArrayType::get(t_ptr, functions.size()), functions),
           "lm_functions");
    define(*module, llvm::ConstantInt::get(t_i32, functions.size()),
           "lm_nfunctions");
    return module;
}
