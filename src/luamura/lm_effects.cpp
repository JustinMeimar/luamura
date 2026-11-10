#include "lm_effects.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>

#include <algorithm>

namespace luamura {

EffectModel::EffectModel(LmIR& ir) : ir(ir), top(ir.proto.maxstacksize) {
    findCapturedSlots();
}

void EffectModel::findCapturedSlots() {
    const Proto& proto = ir.proto;
    for (Instruction i : llvm::ArrayRef(proto.code, proto.sizecode)) {
        if (GET_OPCODE(i) != OP_CLOSURE)
            continue;
        const Proto& nested = *proto.p[GETARG_Bx(i)];
        for (const Upvaldesc& upvalue :
             llvm::ArrayRef(nested.upvalues, nested.sizeupvalues))
            if (upvalue.instack)
                captured.push_back(upvalue.idx);
    }
    llvm::sort(captured);
    captured.erase(llvm::unique(captured), captured.end());
}

bool EffectModel::reads(FrameSlot slot) const {
    return llvm::any_of(node->inputs,
                        [&](const Operand& op) { return op.slot == slot; });
}

bool EffectModel::writes(FrameSlot slot) const {
    return llvm::any_of(node->outputs,
                        [&](const Operand& op) { return op.slot == slot; });
}

void EffectModel::read(FrameSlot slot) {
    if (slot < top && !reads(slot))
        node->inputs.push_back({slot});
}

void EffectModel::write(FrameSlot slot) {
    if (slot < top && !writes(slot))
        ir.addOutput(*node, slot);
}

void EffectModel::readRange(FrameSlot from, FrameSlot to) {
    for (FrameSlot slot = from; slot < std::min(to, top); ++slot)
        read(slot);
}

void EffectModel::writeRange(FrameSlot from, FrameSlot to) {
    for (FrameSlot slot = from; slot < std::min(to, top); ++slot)
        write(slot);
}

// Metamethods, __close, finalizers run by the GC and callees are Lua
// code, which can reach captured slots.
void EffectModel::runsLua() {
    node->effects = Node::Effects::Runtime;
    for (FrameSlot slot : captured) {
        read(slot);
        write(slot);
    }
}

// A count of zero means the values run to the top of the stack.
FrameSlot EffectModel::upTo(uint32_t count, FrameSlot end) const {
    return count == 0 ? top : end;
}

void EffectModel::model(Node& node) {
    this->node = &node;
    Instruction i = node.bytecode;
    OpCode op = GET_OPCODE(i);
    FrameSlot a = GETARG_A(i);
    uint32_t b = GETARG_B(i);
    uint32_t c = GETARG_C(i);

    if (op >= OP_ADDI && op <= OP_SHR) {
        read(b);
        if (op >= OP_ADD)
            read(c);
        write(a);
        return;
    }
    LUAMURA_SWITCH(i) {
    LUAMURA_CASE(MOVE):
    LUAMURA_CASE(NOT):
        read(b);
        write(a);
        break;
    LUAMURA_CASE(LOADI):
    LUAMURA_CASE(LOADF):
    LUAMURA_CASE(LOADK):
    LUAMURA_CASE(LOADKX):
    LUAMURA_CASE(LOADFALSE):
    LUAMURA_CASE(LOADTRUE):
    LUAMURA_CASE(LFALSESKIP):
    LUAMURA_CASE(GETUPVAL):
        write(a);
        break;
    LUAMURA_CASE(LOADNIL):
        writeRange(a, a + b + 1);
        break;
    LUAMURA_CASE(SETUPVAL):
    LUAMURA_CASE(TBC):
    LUAMURA_CASE(EQK):
    LUAMURA_CASE(EQI):
    LUAMURA_CASE(TEST):
    LUAMURA_CASE(RETURN1):
    LUAMURA_CASE(ERRNNIL):
        read(a);
        break;
    LUAMURA_CASE(GETVARG):
        read(b);
        read(c);
        write(a);
        break;
    LUAMURA_CASE(GETTABUP):
    LUAMURA_CASE(NEWTABLE):
    LUAMURA_CASE(CLOSURE):
        write(a);
        runsLua();
        break;
    LUAMURA_CASE(GETTABLE):
        read(c);
        [[fallthrough]];
    LUAMURA_CASE(GETI):
    LUAMURA_CASE(GETFIELD):
    LUAMURA_CASE(UNM):
    LUAMURA_CASE(BNOT):
    LUAMURA_CASE(LEN):
        read(b);
        write(a);
        runsLua();
        break;
    LUAMURA_CASE(SETTABLE):
        read(b);
        [[fallthrough]];
    LUAMURA_CASE(SETI):
    LUAMURA_CASE(SETFIELD):
        read(a);
        [[fallthrough]];
    LUAMURA_CASE(SETTABUP):
        if (!TESTARG_k(i))
            read(c);
        runsLua();
        break;
    LUAMURA_CASE(SELF):
        read(b);
        write(a);
        write(a + 1);
        runsLua();
        break;
    LUAMURA_CASE(MMBIN):
        read(b);
        [[fallthrough]];
    LUAMURA_CASE(MMBINI):
    LUAMURA_CASE(MMBINK):
        // the result goes to the arithmetic instruction's register
        read(a);
        write(GETARG_A(ir.proto.code[node.pc - 1]));
        runsLua();
        break;
    LUAMURA_CASE(CONCAT):
        readRange(a, a + b);
        writeRange(a, a + b);
        runsLua();
        break;
    LUAMURA_CASE(CLOSE):
        readRange(a, top);
        runsLua();
        break;
    LUAMURA_CASE(JMP):
    LUAMURA_CASE(RETURN0):
    LUAMURA_CASE(EXTRAARG):
        break;
    LUAMURA_CASE(EQ):
    LUAMURA_CASE(LT):
    LUAMURA_CASE(LE):
        read(b);
        [[fallthrough]];
    LUAMURA_CASE(LTI):
    LUAMURA_CASE(LEI):
    LUAMURA_CASE(GTI):
    LUAMURA_CASE(GEI):
        read(a);
        runsLua();
        break;
    LUAMURA_CASE(TESTSET):
        read(b);
        write(a);
        break;
    LUAMURA_CASE(CALL):
        // the callee's frame starts above the function
        readRange(a, upTo(b, a + b));
        writeRange(a, top);
        runsLua();
        break;
    LUAMURA_CASE(TAILCALL):
        readRange(a, upTo(b, a + b));
        runsLua();
        break;
    LUAMURA_CASE(RETURN):
        // closing to-be-closed variables reads them, wherever they are
        readRange(TESTARG_k(i) ? 0 : a, upTo(b, a + b - 1));
        runsLua();
        break;
    LUAMURA_CASE(FORPREP):
    LUAMURA_CASE(FORLOOP):
        readRange(a, a + 3);
        writeRange(a, a + 3);
        break;
    LUAMURA_CASE(TFORPREP):
        readRange(a + 2, a + 4);
        writeRange(a + 2, a + 4);
        break;
    LUAMURA_CASE(TFORCALL):
        readRange(a, a + 4);
        writeRange(a + 3, top);
        runsLua();
        break;
    LUAMURA_CASE(TFORLOOP):
        read(a + 3);
        break;
    LUAMURA_CASE(SETLIST):
        readRange(a, upTo(GETARG_vB(i), a + 1 + GETARG_vB(i)));
        break;
    LUAMURA_CASE(VARARG):
        if (TESTARG_k(i))
            read(b);
        writeRange(a, upTo(c, a + c - 1));
        break;
    default:
        readRange(0, top);
        writeRange(0, top);
        runsLua();
        break;
    }
}

} // namespace luamura
