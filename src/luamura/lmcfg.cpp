#include "lmcfg.h"

namespace cfg {

namespace {

// How far past itself the instruction at 'pc' may move pc.
std::vector<int> offsets(const Proto* p, int pc) {
    Instruction i = p->code[pc];
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
        int jump = GETARG_sJ(p->code[pc + 1]) + 1;
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

bool is_call(OpCode op) { return op == OP_CALL || op == OP_TFORCALL; }

} // namespace

std::vector<int> successors(const Proto* proto, int pc) {
    Instruction i = proto->code[pc];
    switch (GET_OPCODE(i)) {
    case OP_JMP:
        return {pc + 1 + GETARG_sJ(i)};
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
    case OP_TAILCALL:
    case OP_EXTRAARG:
        return {};
    default:
        break;
    }
    std::vector<int> next;
    for (int offset : offsets(proto, pc))
        next.push_back(pc + 1 + offset);
    return next;
}

std::vector<bool> leaders(const Proto* proto) {
    int n = proto->sizecode;
    std::vector<bool> leader(n, false);
    leader[0] = true;
    for (int pc = 0; pc < n; pc++) {
        std::vector<int> next = successors(proto, pc);
        bool falls = next.size() == 1 && next[0] == pc + 1;
        if (falls && !is_call(GET_OPCODE(proto->code[pc])))
            continue;
        for (int s : next)
            leader[s] = true;
        if (pc + 1 < n)
            leader[pc + 1] = true;
    }
    return leader;
}

std::vector<int> entries(const Proto* proto) {
    std::vector<int> pcs = {0};
    for (int pc = 0; pc + 1 < proto->sizecode; pc++)
        if (is_call(GET_OPCODE(proto->code[pc])))
            pcs.push_back(pc + 1);
    return pcs;
}

} // namespace cfg
