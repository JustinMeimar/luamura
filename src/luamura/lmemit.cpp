#include "lmemit.h"
#include "lmssa.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

#include <algorithm>
#include <cstddef>
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

// A TValue in machine registers: its Value as 64 bits, and its tag.
struct TV {
    llvm::Value* value;
    llvm::Value* tag;
};

class Emitter {
public:
    explicit Emitter(llvm::Module& module)
        : module(module), context(module.getContext()), builder(context),
          t_ptr(llvm::PointerType::getUnqual(context)),
          t_i8(llvm::Type::getInt8Ty(context)),
          t_i32(llvm::Type::getInt32Ty(context)),
          t_i64(llvm::Type::getInt64Ty(context)),
          t_double(llvm::Type::getDoubleTy(context)) {}

    llvm::Function* emit(const ssa::Function& f, size_t index);

private:
    // lvm.c's instruction, as a function (see LUAMURA there)
    llvm::FunctionCallee handler(OpCode op) {
        auto* type = llvm::FunctionType::get(
            t_ptr, {t_ptr, t_ptr, t_ptr, t_i32}, false);
        return module.getOrInsertFunction(
            std::string("luaV_OP_") + opnames[op], type);
    }

    llvm::Value* tag(int t) { return builder.getInt8(t); }
    TV constant(const TValue* k);
    TV integer(llvm::Value* v) { return {v, tag(LUA_VNUMINT)}; }
    TV number(llvm::Value* d) {
        return {builder.CreateBitCast(d, t_i64), tag(LUA_VNUMFLT)};
    }
    llvm::Value* has_tag(TV v, int t) {
        return builder.CreateICmpEQ(v.tag, tag(t));
    }
    llvm::Value* is_false(TV v);
    llvm::Value* is_number(TV v) {
        return builder.CreateOr(has_tag(v, LUA_VNUMINT),
                                has_tag(v, LUA_VNUMFLT));
    }
    llvm::Value* as_double(TV v) {
        return builder.CreateSelect(
            has_tag(v, LUA_VNUMINT),
            builder.CreateSIToFP(v.value, t_double),
            builder.CreateBitCast(v.value, t_double));
    }
    TV select(llvm::Value* cond, TV a, TV b) {
        return {builder.CreateSelect(cond, a.value, b.value),
                builder.CreateSelect(cond, a.tag, b.tag)};
    }

    llvm::Value* hooked();
    llvm::Value* base();
    llvm::Value* slot(int reg);
    TV load(llvm::Value* address);
    void store(TV v, llvm::Value* address);

    TV get(ssa::Value* v) { return values[ssa::resolve(v)->id]; }
    void set(ssa::Value* v, TV tv) { values[v->id] = tv; }
    // The block at 'pc', branched to from the current insertion block,
    // which becomes the exit of the SSA block being emitted.
    llvm::BasicBlock* at(int pc) {
        exits[current->id] = builder.GetInsertBlock();
        return blocks[f->at[pc]->id];
    }
    void branch(llvm::Value* cond, int pc_true, int pc_false);

    void emit_block(const ssa::Block& b);
    void emit_loads(const ssa::Block& b, int position);
    void emit_computed(const ssa::Operation& ins);
    void emit_observed(const ssa::Block& b, int n);
    llvm::Value* int_arith(OpCode op, llvm::Value* a, llvm::Value* b);
    llvm::Value* float_arith(OpCode op, llvm::Value* a, llvm::Value* b);
    llvm::BasicBlock* fresh(const char* name) {
        return llvm::BasicBlock::Create(context, name, function);
    }

    llvm::Module& module;
    llvm::LLVMContext& context;
    llvm::IRBuilder<> builder;
    llvm::PointerType* t_ptr;
    llvm::IntegerType* t_i8;
    llvm::IntegerType* t_i32;
    llvm::IntegerType* t_i64;
    llvm::Type* t_double;

    const ssa::Function* f = nullptr;
    const ssa::Block* current = nullptr;
    llvm::Function* function = nullptr;
    llvm::Value* L = nullptr;
    llvm::Value* ci = nullptr;
    llvm::Value* code = nullptr;
    llvm::Value* k = nullptr;
    llvm::Value* pc_slot = nullptr;
    llvm::BasicBlock* interpret = nullptr;
    llvm::Value* frame = nullptr;
    std::vector<llvm::BasicBlock*> blocks;
    std::vector<llvm::BasicBlock*> exits;
    std::vector<TV> values;
};

TV Emitter::constant(const TValue* v) {
    switch (ttypetag(v)) {
    case LUA_VNUMINT:
        return integer(builder.getInt64(ivalue(v)));
    case LUA_VNUMFLT:
        return number(llvm::ConstantFP::get(t_double, fltvalue(v)));
    case LUA_VNIL:
    case LUA_VFALSE:
    case LUA_VTRUE:
        return {builder.getInt64(0), tag(ttypetag(v))};
    default:
        return load(builder.CreateConstInBoundsGEP1_64(
            t_i8, k, (v - f->proto->k) * sizeof(TValue)));
    }
}

llvm::Value* Emitter::is_false(TV v) {
    auto* nil =
        builder.CreateICmpEQ(builder.CreateAnd(v.tag, 0x0F), tag(LUA_TNIL));
    return builder.CreateOr(has_tag(v, LUA_VFALSE), nil);
}

llvm::Value* Emitter::hooked() {
    auto* mask = builder.CreateLoad(
        builder.getIntNTy(8 * sizeof(l_signalT)),
        builder.CreateConstInBoundsGEP1_64(
            t_i8, L, offsetof(lua_State, hookmask)),
        true);
    return builder.CreateIsNotNull(mask);
}

// The frame moves when the stack grows, so its base is computed again
// after every instruction that runs on the frame.
llvm::Value* Emitter::base() {
    if (frame != nullptr)
        return frame;
    auto* func =
        builder.CreateLoad(t_ptr, builder.CreateConstInBoundsGEP1_64(
                                      t_i8, ci, offsetof(CallInfo, func)));
    frame =
        builder.CreateConstInBoundsGEP1_64(t_i8, func, sizeof(StackValue));
    return frame;
}

llvm::Value* Emitter::slot(int reg) {
    return builder.CreateConstInBoundsGEP1_64(t_i8, base(),
                                              reg * sizeof(StackValue));
}

TV Emitter::load(llvm::Value* address) {
    auto* t = builder.CreateConstInBoundsGEP1_64(t_i8, address,
                                                 offsetof(TValue, tt_));
    return {builder.CreateLoad(t_i64, address),
            builder.CreateLoad(t_i8, t)};
}

void Emitter::store(TV v, llvm::Value* address) {
    auto* t = builder.CreateConstInBoundsGEP1_64(t_i8, address,
                                                 offsetof(TValue, tt_));
    builder.CreateStore(v.value, address);
    builder.CreateStore(v.tag, t);
}

void Emitter::branch(llvm::Value* cond, int pc_true, int pc_false) {
    if (pc_true == pc_false) {
        builder.CreateBr(at(pc_true));
        return;
    }
    builder.CreateCondBr(cond, at(pc_true), at(pc_false));
}

llvm::Value* Emitter::int_arith(OpCode op, llvm::Value* a, llvm::Value* b) {
    switch (op) {
    case OP_ADD:
    case OP_ADDI:
    case OP_ADDK:
        return builder.CreateAdd(a, b);
    case OP_SUB:
    case OP_SUBK:
        return builder.CreateSub(a, b);
    default:
        return builder.CreateMul(a, b);
    }
}

llvm::Value* Emitter::float_arith(OpCode op, llvm::Value* a,
                                  llvm::Value* b) {
    switch (op) {
    case OP_ADD:
    case OP_ADDI:
    case OP_ADDK:
        return builder.CreateFAdd(a, b);
    case OP_SUB:
    case OP_SUBK:
        return builder.CreateFSub(a, b);
    default:
        return builder.CreateFMul(a, b);
    }
}

void Emitter::emit_computed(const ssa::Operation& ins) {
    const Proto* p = f->proto;
    int pc = ins.pc;
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    auto use = [&](size_t n) { return get(ins.uses[n]); };
    auto result = [&](size_t n, TV v) { set(ins.results[n], v); };
    // test operations skip the following jump or take it
    auto jump = [&](llvm::Value* take) {
        std::vector<int> next = ssa::successors(p, pc);
        branch(take, next.front(), next.back());
    };

    switch (op) {
    case OP_MOVE:
        result(0, use(0));
        return;
    case OP_LOADI:
        result(0, integer(builder.getInt64(GETARG_sBx(i))));
        return;
    case OP_LOADF:
        result(0, number(llvm::ConstantFP::get(
                      t_double, static_cast<double>(GETARG_sBx(i)))));
        return;
    case OP_LOADK:
        result(0, constant(p->k + GETARG_Bx(i)));
        return;
    case OP_LOADFALSE:
        result(0, {builder.getInt64(0), tag(LUA_VFALSE)});
        return;
    case OP_LFALSESKIP:
        result(0, {builder.getInt64(0), tag(LUA_VFALSE)});
        builder.CreateBr(at(pc + 2));
        return;
    case OP_LOADTRUE:
        result(0, {builder.getInt64(0), tag(LUA_VTRUE)});
        return;
    case OP_LOADNIL:
        for (size_t n = 0; n < ins.results.size(); n++)
            result(n, {builder.getInt64(0), tag(LUA_VNIL)});
        return;
    case OP_NOT:
        result(0, {builder.getInt64(0),
                   builder.CreateSelect(is_false(use(0)), tag(LUA_VTRUE),
                                        tag(LUA_VFALSE))});
        return;
    case OP_JMP:
        builder.CreateBr(at(pc + 1 + GETARG_sJ(i)));
        return;
    case OP_TEST: {
        auto* truthy = builder.CreateNot(is_false(use(0)));
        jump(builder.CreateICmpEQ(truthy, builder.getInt1(GETARG_k(i))));
        return;
    }
    case OP_TESTSET: {
        auto* take = builder.CreateICmpNE(is_false(use(0)),
                                          builder.getInt1(GETARG_k(i)));
        result(0, select(take, use(0), use(1)));
        jump(take);
        return;
    }
    case OP_EQI: {
        TV a = use(0);
        int im = GETARG_sB(i);
        auto* eq_int = builder.CreateICmpEQ(a.value, builder.getInt64(im));
        auto* eq_flt =
            builder.CreateFCmpOEQ(builder.CreateBitCast(a.value, t_double),
                                  llvm::ConstantFP::get(t_double, im));
        auto* cond = builder.CreateSelect(
            has_tag(a, LUA_VNUMINT), eq_int,
            builder.CreateAnd(has_tag(a, LUA_VNUMFLT), eq_flt));
        jump(builder.CreateICmpEQ(cond, builder.getInt1(GETARG_k(i))));
        return;
    }
    case OP_ADDI:
    case OP_ADDK:
    case OP_SUBK:
    case OP_MULK:
    case OP_ADD:
    case OP_SUB:
    case OP_MUL: {
        TV a = use(0);
        TV b = op == OP_ADDI  ? integer(builder.getInt64(GETARG_sC(i)))
               : op >= OP_ADD ? use(1)
                              : constant(p->k + GETARG_C(i));
        TV old = use(ins.uses.size() - 1);
        // lvm.c's op_arith: integers wrap, other numbers convert to
        // floats, and anything else falls through to the metamethod
        auto* ints = fresh("integers");
        auto* other = fresh("other");
        auto* floats = fresh("floats");
        auto* done = fresh("done");
        builder.CreateCondBr(builder.CreateAnd(has_tag(a, LUA_VNUMINT),
                                               has_tag(b, LUA_VNUMINT)),
                             ints, other);
        builder.SetInsertPoint(ints);
        auto* i_result = int_arith(op, a.value, b.value);
        builder.CreateBr(done);
        builder.SetInsertPoint(other);
        builder.CreateCondBr(builder.CreateAnd(is_number(a), is_number(b)),
                             floats, done);
        builder.SetInsertPoint(floats);
        auto* f_result = builder.CreateBitCast(
            float_arith(op, as_double(a), as_double(b)), t_i64);
        builder.CreateBr(done);

        builder.SetInsertPoint(done);
        auto* value = builder.CreatePHI(t_i64, 3);
        auto* t = builder.CreatePHI(t_i8, 3);
        auto* ok = builder.CreatePHI(builder.getInt1Ty(), 3);
        value->addIncoming(i_result, ints);
        t->addIncoming(tag(LUA_VNUMINT), ints);
        ok->addIncoming(builder.getTrue(), ints);
        value->addIncoming(f_result, floats);
        t->addIncoming(tag(LUA_VNUMFLT), floats);
        ok->addIncoming(builder.getTrue(), floats);
        value->addIncoming(old.value, other);
        t->addIncoming(old.tag, other);
        ok->addIncoming(builder.getFalse(), other);
        result(0, {value, t});
        branch(ok, pc + 2, pc + 1);
        return;
    }
    case OP_FORLOOP: {
        TV count = use(0), step = use(1), idx = use(2);
        auto* ints = fresh("integers");
        auto* floats = fresh("floats");
        auto* done = fresh("done");
        builder.CreateCondBr(has_tag(step, LUA_VNUMINT), ints, floats);

        builder.SetInsertPoint(ints);
        auto* more = builder.CreateICmpNE(count.value, builder.getInt64(0));
        auto* i_count = builder.CreateSelect(
            more, builder.CreateSub(count.value, builder.getInt64(1)),
            count.value);
        auto* i_idx = builder.CreateSelect(
            more, builder.CreateAdd(idx.value, step.value), idx.value);
        builder.CreateBr(done);

        // floatforloop: the first register holds the limit
        builder.SetInsertPoint(floats);
        auto* fstep = builder.CreateBitCast(step.value, t_double);
        auto* flimit = builder.CreateBitCast(count.value, t_double);
        auto* fnext = builder.CreateFAdd(
            builder.CreateBitCast(idx.value, t_double), fstep);
        auto* within = builder.CreateSelect(
            builder.CreateFCmpOLT(llvm::ConstantFP::get(t_double, 0.0),
                                  fstep),
            builder.CreateFCmpOLE(fnext, flimit),
            builder.CreateFCmpOLE(flimit, fnext));
        auto* f_idx = builder.CreateSelect(
            within, builder.CreateBitCast(fnext, t_i64), idx.value);
        builder.CreateBr(done);

        builder.SetInsertPoint(done);
        auto* new_count = builder.CreatePHI(t_i64, 2);
        auto* new_idx = builder.CreatePHI(t_i64, 2);
        auto* again = builder.CreatePHI(builder.getInt1Ty(), 2);
        new_count->addIncoming(i_count, ints);
        new_idx->addIncoming(i_idx, ints);
        again->addIncoming(more, ints);
        new_count->addIncoming(count.value, floats);
        new_idx->addIncoming(f_idx, floats);
        again->addIncoming(within, floats);
        result(0, {new_count, count.tag});
        result(1, {new_idx, idx.tag});
        branch(again, pc + 1 - GETARG_Bx(i), pc + 1);
        return;
    }
    default:
        builder.CreateUnreachable();
        return;
    }
}

void Emitter::emit_observed(const ssa::Block& b, int n) {
    const ssa::Operation& ins = b.operations[n];
    bool last = n + 1 == static_cast<int>(b.operations.size());
    const Proto* p = f->proto;
    int pc = ins.pc;
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    if (op == OP_EXTRAARG) {
        builder.CreateUnreachable();
        return;
    }
    for (auto& [reg, v] : ins.stores)
        if (!ssa::in_frame(v, reg))
            store(get(v), slot(reg));

    builder.CreateStore(
        builder.CreateConstInBoundsGEP1_32(t_i32, code, pc + 1), pc_slot);
    auto* result = builder.CreateCall(
        handler(op), {L, ci, pc_slot, builder.getInt32(i)});
    frame = nullptr;
    emit_loads(b, n);
    if (op == OP_CALL) {
        // a C function returned, and may have set hooks
        auto* returned = fresh("returned");
        auto* run = fresh("run");
        builder.CreateCondBr(builder.CreateIsNull(result), returned, run);
        builder.SetInsertPoint(run);
        builder.CreateRet(result);
        builder.SetInsertPoint(returned);
        builder.CreateCondBr(hooked(), interpret, at(pc + 1));
        return;
    }
    if (op == OP_TAILCALL) {
        // a Lua callee replaces this function in ci; enter its
        // implementation without growing the C stack
        auto* callee = fresh("callee");
        auto* done = fresh("done");
        builder.CreateCondBr(builder.CreateICmpEQ(result, ci), callee,
                             done);
        builder.SetInsertPoint(done);
        builder.CreateRet(result);
        builder.SetInsertPoint(callee);
        auto* implementation = builder.CreateCall(
            module.getOrInsertFunction("lm_compiled", t_ptr, t_ptr, t_ptr),
            {L, ci});
        auto* compiled = fresh("compiled");
        builder.CreateCondBr(builder.CreateIsNull(implementation), done,
                             compiled);
        builder.SetInsertPoint(compiled);
        auto* call = builder.CreateCall(function->getFunctionType(),
                                        implementation, {L, ci});
        call->setTailCallKind(llvm::CallInst::TCK_MustTail);
        builder.CreateRet(call);
        return;
    }
    if (is_call(op)) {
        builder.CreateRet(result);
        return;
    }
    if (!last)
        return;
    std::vector<int> targets = ssa::successors(p, pc);
    if (targets.size() == 1) {
        builder.CreateBr(at(targets[0]));
        return;
    }
    auto* moved = builder.CreatePtrDiff(
        t_i32, builder.CreateLoad(t_ptr, pc_slot), code);
    auto* sw =
        builder.CreateSwitch(moved, at(targets[0]), targets.size() - 1);
    for (size_t n = 1; n < targets.size(); n++)
        sw->addCase(llvm::ConstantInt::get(
                        llvm::cast<llvm::IntegerType>(moved->getType()),
                        targets[n]),
                    at(targets[n]));
}

void Emitter::emit_loads(const ssa::Block& b, int position) {
    for (ssa::Value* v : b.loads)
        if (v->position == position)
            set(v, load(slot(v->reg)));
}

void Emitter::emit_block(const ssa::Block& b) {
    builder.SetInsertPoint(blocks[b.id]);
    current = &b;
    frame = nullptr;
    for (ssa::Value* phi : b.phis)
        set(phi, {builder.CreatePHI(t_i64, b.preds.size()),
                  builder.CreatePHI(t_i8, b.preds.size())});
    emit_loads(b, -1);
    for (size_t n = 0; n < b.operations.size(); n++) {
        const ssa::Operation& ins = b.operations[n];
        if (ins.computed)
            emit_computed(ins);
        else
            emit_observed(b, static_cast<int>(n));
    }
    if (b.operations.empty()) {
        builder.CreateBr(at(b.succs[0]->start));
        return;
    }
    if (builder.GetInsertBlock()->getTerminator() == nullptr)
        builder.CreateBr(at(b.end));
}

// Lua can resume the function at its start and after calls; anywhere
// else, the interpreter continues it.
llvm::Function* Emitter::emit(const ssa::Function& lifted, size_t index) {
    f = &lifted;
    auto* type = llvm::FunctionType::get(t_ptr, {t_ptr, t_ptr}, false);
    function = llvm::Function::Create(
        type, llvm::GlobalValue::InternalLinkage,
        "lm_function_" + std::to_string(index), module);
    L = function->getArg(0);
    ci = function->getArg(1);

    // blocks in reverse postorder from where the function resumes, so
    // that values are emitted before their uses
    std::vector<const ssa::Block*> order;
    std::vector<bool> seen(f->blocks.size(), false);
    std::vector<std::pair<const ssa::Block*, size_t>> stack;
    for (auto& [pc, root] : f->resumes) {
        if (seen[root->id])
            continue;
        seen[root->id] = true;
        stack.push_back({root, 0});
        while (!stack.empty()) {
            auto& [b, next] = stack.back();
            if (next == b->succs.size()) {
                order.push_back(b);
                stack.pop_back();
                continue;
            }
            const ssa::Block* s = b->succs[next++];
            if (!seen[s->id]) {
                seen[s->id] = true;
                stack.push_back({s, 0});
            }
        }
    }
    std::reverse(order.begin(), order.end());

    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    interpret = llvm::BasicBlock::Create(context, "interpret", function);
    blocks.assign(f->blocks.size(), nullptr);
    exits.assign(f->blocks.size(), nullptr);
    for (const ssa::Block* b : order)
        blocks[b->id] = llvm::BasicBlock::Create(
            context, "pc" + std::to_string(b->start), function);
    values.assign(f->values.size(), {nullptr, nullptr});

    builder.SetInsertPoint(interpret);
    builder.CreateRet(ci);

    builder.SetInsertPoint(entry);
    pc_slot = builder.CreateAlloca(t_ptr);
    code = builder.CreateCall(
        module.getOrInsertFunction("lm_code", t_ptr, t_ptr), {ci});
    k = builder.CreateCall(
        module.getOrInsertFunction("lm_constants", t_ptr, t_ptr), {ci});
    auto* pc = builder.CreateCall(
        module.getOrInsertFunction("lm_resume", t_i32, t_ptr), {ci});
    auto* resume = builder.CreateSwitch(pc, interpret, f->resumes.size());
    for (auto& [start, b] : f->resumes)
        resume->addCase(builder.getInt32(start), blocks[b->id]);

    for (const ssa::Block* b : order)
        emit_block(*b);

    for (const ssa::Block* b : order)
        for (ssa::Value* phi : b->phis) {
            TV tv = values[phi->id];
            auto* value = llvm::cast<llvm::PHINode>(tv.value);
            auto* tag = llvm::cast<llvm::PHINode>(tv.tag);
            for (size_t n = 0; n < b->preds.size(); n++) {
                const ssa::Block* pred = b->preds[n];
                if (!seen[pred->id])
                    continue;
                TV in = get(phi->operands[n]);
                value->addIncoming(in.value, exits[pred->id]);
                tag->addIncoming(in.tag, exits[pred->id]);
            }
        }
    return function;
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
        functions.push_back(emitter.emit(*ssa::lift(protos[i]), i));

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
