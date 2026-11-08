#include "lmcodegen.h"
#include "lmbuilder.h"
#include "lmcfg.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lopnames.h"
}

namespace {

using BinaryOps = llvm::Instruction::BinaryOps;

// The integer and float operations of an arithmetic instruction.
std::pair<BinaryOps, BinaryOps> arith_ops(OpCode op) {
    switch (op) {
    case OP_ADD:
    case OP_ADDI:
    case OP_ADDK:
        return {llvm::Instruction::Add, llvm::Instruction::FAdd};
    case OP_SUB:
    case OP_SUBK:
        return {llvm::Instruction::Sub, llvm::Instruction::FSub};
    default:
        return {llvm::Instruction::Mul, llvm::Instruction::FMul};
    }
}

class FunctionCompiler {
public:
    FunctionCompiler(llvm::Module& module, const Proto* proto)
        : module(module), proto(proto), builder(module.getContext()) {}

    llvm::Function* compile(const std::string& name);

private:
    void instruction(int pc);
    bool compile_inline(int pc);
    void call_runtime(int pc);
    void tail_call(llvm::Value* next);
    void arith(int pc, Operand a, Operand b);
    void for_loop(int pc);
    void jump(int pc, llvm::Value* take);

    llvm::Value* reg(int r) {
        return builder.offset(base, r * sizeof(StackValue));
    }
    Operand constant(int n);
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
    Builder builder;
    llvm::Function* function = nullptr;
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    llvm::Value* code = nullptr;
    llvm::Value* k = nullptr;
    llvm::Value* pc_address = nullptr;
    llvm::Value* base = nullptr;
    llvm::BasicBlock* interpret = nullptr;
    // the block starting at each pc, or null
    std::vector<llvm::BasicBlock*> blocks;
};

llvm::Function* FunctionCompiler::compile(const std::string& name) {
    auto* ptr = builder.t_ptr;
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
    k = builder.CreateCall(runtime("lm_constants", ptr, {ptr}), {ci});
    auto* saved = builder.CreateCall(
        runtime("lm_resume", builder.t_i32, {ptr}), {ci});
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

// The stack moves when it grows, so each instruction finds its base.
void FunctionCompiler::instruction(int pc) {
    auto* func = builder.CreateLoad(
        builder.t_ptr, builder.offset(ci, offsetof(CallInfo, func)));
    base = builder.offset(func, sizeof(StackValue));
    if (!compile_inline(pc))
        call_runtime(pc);
}

bool FunctionCompiler::compile_inline(int pc) {
    Instruction i = proto->code[pc];
    llvm::Value* ra = reg(GETARG_A(i));
    switch (GET_OPCODE(i)) {
    case OP_MOVE:
        builder.store(builder.load(reg(GETARG_B(i))), ra);
        return true;
    case OP_LOADI:
        builder.store(builder.int_operand(builder.getInt64(GETARG_sBx(i))),
                      ra);
        return true;
    case OP_LOADF:
        builder.store(builder.float_operand(llvm::ConstantFP::get(
                          builder.t_double, GETARG_sBx(i))),
                      ra);
        return true;
    case OP_LOADK:
        builder.store(constant(GETARG_Bx(i)), ra);
        return true;
    case OP_LOADFALSE:
        builder.store(builder.tag_operand(LUA_VFALSE), ra);
        return true;
    case OP_LFALSESKIP:
        builder.store(builder.tag_operand(LUA_VFALSE), ra);
        builder.CreateBr(blocks[pc + 2]);
        return true;
    case OP_LOADTRUE:
        builder.store(builder.tag_operand(LUA_VTRUE), ra);
        return true;
    case OP_LOADNIL:
        for (int r = GETARG_A(i); r <= GETARG_A(i) + GETARG_B(i); r++)
            builder.store(builder.tag_operand(LUA_VNIL), reg(r));
        return true;
    case OP_NOT: {
        auto* is_false = builder.is_false(builder.load(reg(GETARG_B(i))));
        builder.store(
            {builder.getInt64(0),
             builder.CreateSelect(is_false, builder.getInt8(LUA_VTRUE),
                                  builder.getInt8(LUA_VFALSE))},
            ra);
        return true;
    }
    case OP_JMP:
        builder.CreateBr(blocks[pc + 1 + GETARG_sJ(i)]);
        return true;
    case OP_TEST: {
        auto* is_true =
            builder.CreateNot(builder.is_false(builder.load(ra)));
        jump(pc,
             builder.CreateICmpEQ(is_true, builder.getInt1(GETARG_k(i))));
        return true;
    }
    case OP_TESTSET: {
        Operand rb = builder.load(reg(GETARG_B(i)));
        auto* take = builder.CreateICmpNE(builder.is_false(rb),
                                          builder.getInt1(GETARG_k(i)));
        std::vector<int> next = cfg::successors(proto, pc);
        auto* set = new_block("set");
        builder.CreateCondBr(take, set, blocks[next.back()]);
        builder.SetInsertPoint(set);
        builder.store(rb, ra);
        builder.CreateBr(blocks[next.front()]);
        return true;
    }
    case OP_EQI: {
        Operand a = builder.load(ra);
        int im = GETARG_sB(i);
        auto* eq_int = builder.CreateICmpEQ(a.value, builder.getInt64(im));
        auto* eq_float = builder.CreateFCmpOEQ(
            builder.as_double(a.value),
            llvm::ConstantFP::get(builder.t_double, im));
        auto* cond = builder.CreateSelect(
            builder.has_tag(a, LUA_VNUMINT), eq_int,
            builder.CreateAnd(builder.has_tag(a, LUA_VNUMFLT), eq_float));
        jump(pc, builder.CreateICmpEQ(cond, builder.getInt1(GETARG_k(i))));
        return true;
    }
    case OP_ADDI:
        arith(pc, builder.load(reg(GETARG_B(i))),
              builder.int_operand(builder.getInt64(GETARG_sC(i))));
        return true;
    case OP_ADDK:
    case OP_SUBK:
    case OP_MULK:
        arith(pc, builder.load(reg(GETARG_B(i))), constant(GETARG_C(i)));
        return true;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
        arith(pc, builder.load(reg(GETARG_B(i))),
              builder.load(reg(GETARG_C(i))));
        return true;
    case OP_FORLOOP:
        for_loop(pc);
        return true;
    case OP_EXTRAARG:
        builder.CreateUnreachable();
        return true;
    default:
        return false;
    }
}

// Tests run the jump that follows them, or skip it.
void FunctionCompiler::jump(int pc, llvm::Value* take) {
    std::vector<int> next = cfg::successors(proto, pc);
    if (next.size() == 1) {
        builder.CreateBr(blocks[next[0]]);
        return;
    }
    builder.CreateCondBr(take, blocks[next.front()], blocks[next.back()]);
}

// Integers wrap, other numbers are added as floats, and anything else
// goes on to the next instruction, which calls the metamethod.
void FunctionCompiler::arith(int pc, Operand a, Operand b) {
    OpCode op = GET_OPCODE(proto->code[pc]);
    llvm::Value* ra = reg(GETARG_A(proto->code[pc]));
    auto [int_op, float_op] = arith_ops(op);
    auto* ints = new_block("integers");
    auto* other = new_block("other");
    auto* floats = new_block("floats");

    builder.CreateCondBr(builder.CreateAnd(builder.has_tag(a, LUA_VNUMINT),
                                           builder.has_tag(b, LUA_VNUMINT)),
                         ints, other);
    builder.SetInsertPoint(ints);
    builder.store(
        builder.int_operand(builder.CreateBinOp(int_op, a.value, b.value)),
        ra);
    builder.CreateBr(blocks[pc + 2]);

    builder.SetInsertPoint(other);
    builder.CreateCondBr(
        builder.CreateAnd(builder.is_number(a), builder.is_number(b)),
        floats, blocks[pc + 1]);
    builder.SetInsertPoint(floats);
    builder.store(
        builder.float_operand(builder.CreateBinOp(
            float_op, builder.to_double(a), builder.to_double(b))),
        ra);
    builder.CreateBr(blocks[pc + 2]);
}

// As lvm.c's OP_FORLOOP: an integer loop counts down the iterations
// left in ra, a float loop compares with the limit in ra.
void FunctionCompiler::for_loop(int pc) {
    Instruction i = proto->code[pc];
    llvm::Value* ra = reg(GETARG_A(i));
    llvm::Value* step_address = reg(GETARG_A(i) + 1);
    llvm::Value* idx_address = reg(GETARG_A(i) + 2);
    llvm::BasicBlock* body = blocks[pc + 1 - GETARG_Bx(i)];
    llvm::BasicBlock* exit = blocks[pc + 1];
    auto* ints = new_block("integers");
    auto* int_next = new_block("integer_next");
    auto* floats = new_block("floats");
    auto* float_next = new_block("float_next");

    Operand step = builder.load(step_address);
    builder.CreateCondBr(builder.has_tag(step, LUA_VNUMINT), ints, floats);

    builder.SetInsertPoint(ints);
    auto* count = builder.load(ra).value;
    builder.CreateCondBr(builder.CreateICmpNE(count, builder.getInt64(0)),
                         int_next, exit);
    builder.SetInsertPoint(int_next);
    builder.store_value(builder.CreateSub(count, builder.getInt64(1)), ra);
    builder.store_value(
        builder.CreateAdd(builder.load(idx_address).value, step.value),
        idx_address);
    builder.CreateBr(body);

    builder.SetInsertPoint(floats);
    auto* fstep = builder.as_double(step.value);
    auto* limit = builder.as_double(builder.load(ra).value);
    auto* idx = builder.CreateFAdd(
        builder.as_double(builder.load(idx_address).value), fstep);
    auto* more = builder.CreateSelect(
        builder.CreateFCmpOLT(llvm::ConstantFP::get(builder.t_double, 0.0),
                              fstep),
        builder.CreateFCmpOLE(idx, limit),
        builder.CreateFCmpOLE(limit, idx));
    builder.CreateCondBr(more, float_next, exit);
    builder.SetInsertPoint(float_next);
    builder.store_value(builder.CreateBitCast(idx, builder.t_i64),
                        idx_address);
    builder.CreateBr(body);
}

// lvm.c's function for the instruction runs it on the stack, and leaves
// in pc where the function continues. Calls and returns give the next
// call for luaV_execute to run.
void FunctionCompiler::call_runtime(int pc) {
    Instruction i = proto->code[pc];
    OpCode op = GET_OPCODE(i);
    auto* ptr = builder.t_ptr;
    builder.CreateStore(
        builder.CreateConstInBoundsGEP1_32(builder.t_i32, code, pc + 1),
        pc_address);
    auto* next =
        builder.CreateCall(runtime(std::string("luaV_OP_") + opnames[op],
                                   ptr, {ptr, ptr, ptr, builder.t_i32}),
                           {L, ci, pc_address, builder.getInt32(i)});

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
        builder.t_i32, builder.CreateLoad(ptr, pc_address), code);
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
    auto* ptr = builder.t_ptr;
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

Operand FunctionCompiler::constant(int n) {
    const TValue* v = proto->k + n;
    switch (ttypetag(v)) {
    case LUA_VNUMINT:
        return builder.int_operand(builder.getInt64(ivalue(v)));
    case LUA_VNUMFLT:
        return builder.float_operand(
            llvm::ConstantFP::get(builder.t_double, fltvalue(v)));
    case LUA_VNIL:
    case LUA_VFALSE:
    case LUA_VTRUE:
        return builder.tag_operand(ttypetag(v));
    default:
        return builder.load(builder.offset(k, n * sizeof(TValue)));
    }
}

llvm::Value* FunctionCompiler::hooked() {
    auto* mask = builder.CreateLoad(
        builder.getIntNTy(8 * sizeof(l_signalT)),
        builder.offset(L, offsetof(lua_State, hookmask)), true);
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
