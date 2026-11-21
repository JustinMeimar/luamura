#include "lm_cfg.h"

#include <algorithm>

namespace luamura {

CFG::CFG(const Proto& proto) : proto(proto) {
    findLeaders();
    addBlocks(reachable());
}

bool CFG::isCall(PC pc) const {
    OpCode op = GET_OPCODE(proto.code[pc]);
    return op == OP_CALL || op == OP_TFORCALL;
}

// The pcs the instruction at 'pc' may continue at, the most common
// first.
std::vector<PC> CFG::successorPCs(PC pc) const {
    Instruction i = proto.code[pc];
    OpCode op = GET_OPCODE(i);
    PC next = pc + 1;
    // a test either takes the jump after it or skips it
    if (testTMode(op)) {
        PC skip = next + 1;
        PC jump = skip + GETARG_sJ(proto.code[next]);
        if (jump == skip)
            return {skip};
        return {jump, skip};
    }
    switch (op) {
    case OP_JMP:
        return {next + GETARG_sJ(i)};
    case OP_RETURN:
    case OP_RETURN0:
    case OP_RETURN1:
    case OP_TAILCALL:
    case OP_EXTRAARG:
        return {};
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
void CFG::findLeaders() {
    PC n = proto.sizecode;
    leader.assign(n, false);
    leader[0] = true;
    for (PC pc = 0; pc < n; pc++) {
        std::vector<PC> next = successorPCs(pc);
        bool falls = next.size() == 1 && next[0] == pc + 1;
        if (falls && !isCall(pc))
            continue;
        for (PC s : next)
            leader[s] = true;
        if (pc + 1 < n)
            leader[pc + 1] = true;
    }
}

PC CFG::blockEnd(PC start) const {
    return static_cast<PC>(
        std::find(leader.begin() + start + 1, leader.end(), true) -
        leader.begin());
}

// whether each pc starts a block that Lua can reach
std::vector<bool> CFG::reachable() const {
    std::vector<bool> reached(proto.sizecode, false);
    std::vector<PC> work = {0};
    while (!work.empty()) {
        PC start = work.back();
        work.pop_back();
        if (reached[start])
            continue;
        reached[start] = true;
        for (PC s : successorPCs(blockEnd(start) - 1))
            work.push_back(s);
    }
    return reached;
}

void CFG::addBlocks(const std::vector<bool>& reached) {
    PC n = proto.sizecode;
    // the block starting at each reached pc
    std::vector<BlockIndex> index(n);
    for (PC pc = 0; pc < n; pc++) {
        if (!reached[pc])
            continue;
        index[pc] = static_cast<BlockIndex>(blocks.size());
        blocks.push_back({pc, blockEnd(pc), {}});
    }
    entries.push_back(0);
    for (Block& b : blocks) {
        for (PC s : successorPCs(b.last()))
            b.successors.push_back(index[s]);
        if (isCall(b.last()))
            entries.push_back(b.successors.front());
    }
}

} // namespace luamura
