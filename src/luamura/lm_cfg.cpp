#include "lm_cfg.h"

#include <algorithm>

namespace {

bool is_call(OpCode op) { return op == OP_CALL || op == OP_TFORCALL; }

// The pcs the instruction at 'pc' may continue at, the most common
// first.
std::vector<int> successors(const Proto* p, int pc) {
    Instruction i = p->code[pc];
    OpCode op = GET_OPCODE(i);
    int next = pc + 1;
    switch (op) {
    case OP_JMP:
        return {next + GETARG_sJ(i)};
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
    case OP_TAILCALL:
    case OP_EXTRAARG:
        return {};
    // a test either takes the jump after it or skips it
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
        int skip = next + 1;
        int jump = skip + GETARG_sJ(p->code[next]);
        if (jump == skip)
            return {skip};
        return {jump, skip};
    }
    case OP_FORLOOP:
    case OP_TFORLOOP:
        return {next - GETARG_Bx(i), next};
    case OP_FORPREP:
        return {next, next + GETARG_Bx(i) + 1};
    case OP_TFORPREP:
        return {next + GETARG_Bx(i)};
    // these skip the instruction after them
    case OP_LOADKX:
    case OP_NEWTABLE:
    case OP_LFALSESKIP:
        return {next + 1};
    case OP_SETLIST:
        return {TESTARG_k(i) ? next + 1 : next};
    default:
        // arithmetic skips the metamethod call after it, unless its
        // operands need one
        if (op >= OP_ADDI && op <= OP_SHR)
            return {next + 1, next};
        return {next};
    }
}

// A block ends where control may go anywhere but the next instruction,
// and after calls, where Lua resumes the function.
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

} // namespace

CFG::CFG(const Proto* proto) {
    int n = proto->sizecode;
    std::vector<bool> leader = leaders(proto);
    auto end_of = [&](int start) {
        return static_cast<int>(
            std::find(leader.begin() + start + 1, leader.end(), true) -
            leader.begin());
    };

    std::vector<bool> reached(n, false);
    std::vector<int> work = {0};
    while (!work.empty()) {
        int start = work.back();
        work.pop_back();
        if (reached[start])
            continue;
        reached[start] = true;
        for (int s : successors(proto, end_of(start) - 1))
            work.push_back(s);
    }

    // the block starting at each pc, or -1
    std::vector<int> index(n, -1);
    for (int pc = 0; pc < n; pc++) {
        if (!reached[pc])
            continue;
        index[pc] = static_cast<int>(blocks.size());
        blocks.push_back({pc, end_of(pc), {}});
    }
    entries.push_back(0);
    for (Block& b : blocks) {
        for (int s : successors(proto, b.last()))
            b.successors.push_back(index[s]);
        if (is_call(GET_OPCODE(proto->code[b.last()])))
            entries.push_back(b.successors.front());
    }
}
