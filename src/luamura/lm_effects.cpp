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

bool EffectModel::reads(unsigned slot) const {
    return llvm::any_of(node->inputs,
                        [&](const Operand& op) { return op.slot == slot; });
}

bool EffectModel::writes(unsigned slot) const {
    return llvm::any_of(node->outputs,
                        [&](const Operand& op) { return op.slot == slot; });
}

void EffectModel::read(unsigned slot) {
    if (slot < top && !reads(slot))
        node->inputs.push_back({slot});
}

void EffectModel::write(unsigned slot) {
    if (slot < top && !writes(slot))
        ir.addOutput(*node, slot);
}

void EffectModel::readRange(unsigned from, unsigned to) {
    for (unsigned slot = from; slot < std::min(to, top); ++slot)
        read(slot);
}

void EffectModel::writeRange(unsigned from, unsigned to) {
    for (unsigned slot = from; slot < std::min(to, top); ++slot)
        write(slot);
}

// Metamethods, __close, finalizers run by the GC and callees are Lua
// code, which can reach captured slots.
void EffectModel::runsLua() {
    node->effects = Node::Effects::Runtime;
    for (unsigned slot : captured) {
        read(slot);
        write(slot);
    }
}

// A count of zero means the values run to the top of the stack.
unsigned EffectModel::upTo(unsigned count, unsigned end) const {
    return count == 0 ? top : end;
}

void EffectModel::model(Node& node) {
    this->node = &node;
    Instruction i = node.bytecode;
    OpCode op = GET_OPCODE(i);
    unsigned a = GETARG_A(i);
    unsigned b = GETARG_B(i);
    unsigned c = GETARG_C(i);

    if (op >= OP_ADDI && op <= OP_SHR) {
        read(b);
        if (op >= OP_ADD)
            read(c);
        write(a);
        return;
    }
    switch (op) {
    case OP_MOVE:
    case OP_NOT:
        read(b);
        write(a);
        break;
    case OP_LOADI:
    case OP_LOADF:
    case OP_LOADK:
    case OP_LOADKX:
    case OP_LOADFALSE:
    case OP_LOADTRUE:
    case OP_LFALSESKIP:
    case OP_GETUPVAL:
        write(a);
        break;
    case OP_LOADNIL:
        writeRange(a, a + b + 1);
        break;
    case OP_SETUPVAL:
    case OP_TBC:
    case OP_EQK:
    case OP_EQI:
    case OP_TEST:
    case OP_RETURN1:
    case OP_ERRNNIL:
        read(a);
        break;
    case OP_GETVARG:
        read(b);
        read(c);
        write(a);
        break;
    case OP_GETTABUP:
    case OP_NEWTABLE:
    case OP_CLOSURE:
        write(a);
        runsLua();
        break;
    case OP_GETTABLE:
        read(c);
        [[fallthrough]];
    case OP_GETI:
    case OP_GETFIELD:
    case OP_UNM:
    case OP_BNOT:
    case OP_LEN:
        read(b);
        write(a);
        runsLua();
        break;
    case OP_SETTABLE:
        read(b);
        [[fallthrough]];
    case OP_SETI:
    case OP_SETFIELD:
        read(a);
        [[fallthrough]];
    case OP_SETTABUP:
        if (!TESTARG_k(i))
            read(c);
        runsLua();
        break;
    case OP_SELF:
        read(b);
        write(a);
        write(a + 1);
        runsLua();
        break;
    case OP_MMBIN:
        read(b);
        [[fallthrough]];
    case OP_MMBINI:
    case OP_MMBINK:
        // the result goes to the arithmetic instruction's register
        read(a);
        write(GETARG_A(ir.proto.code[node.pc - 1]));
        runsLua();
        break;
    case OP_CONCAT:
        readRange(a, a + b);
        writeRange(a, a + b);
        runsLua();
        break;
    case OP_CLOSE:
        readRange(a, top);
        runsLua();
        break;
    case OP_JMP:
    case OP_RETURN0:
    case OP_EXTRAARG:
        break;
    case OP_EQ:
    case OP_LT:
    case OP_LE:
        read(b);
        [[fallthrough]];
    case OP_LTI:
    case OP_LEI:
    case OP_GTI:
    case OP_GEI:
        read(a);
        runsLua();
        break;
    case OP_TESTSET:
        read(b);
        write(a);
        break;
    case OP_CALL:
        // the callee's frame starts above the function
        readRange(a, upTo(b, a + b));
        writeRange(a, top);
        runsLua();
        break;
    case OP_TAILCALL:
        readRange(a, upTo(b, a + b));
        runsLua();
        break;
    case OP_RETURN:
        // closing to-be-closed variables reads them, wherever they are
        readRange(TESTARG_k(i) ? 0 : a, upTo(b, a + b - 1));
        runsLua();
        break;
    case OP_FORPREP:
    case OP_FORLOOP:
        readRange(a, a + 3);
        writeRange(a, a + 3);
        break;
    case OP_TFORPREP:
        readRange(a + 2, a + 4);
        writeRange(a + 2, a + 4);
        break;
    case OP_TFORCALL:
        readRange(a, a + 4);
        writeRange(a + 3, top);
        runsLua();
        break;
    case OP_TFORLOOP:
        read(a + 3);
        break;
    case OP_SETLIST:
        readRange(a, upTo(GETARG_vB(i), a + 1 + GETARG_vB(i)));
        break;
    case OP_VARARG:
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
