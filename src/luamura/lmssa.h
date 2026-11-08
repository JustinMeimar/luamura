#ifndef SSA_H
#define SSA_H

#include "lmlua.h"

#include <bitset>
#include <map>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

// A function's registers in SSA form (Braun et al., CC 2013). luamura
// computes some instructions on SSA values; the others run their lvm.c
// body on the Lua frame, where Lua can observe it, so registers are
// written back before them and loaded again after them.
namespace ssa {

struct Block;

// The contents of a register at some point of the function.
struct Value {
    enum class Kind { Load, Phi, Result };

    Kind kind;
    int id;
    int reg;
    Block* block;
    // Load: the instruction (index in block) it follows, or -1.
    // Result: the defining instruction's index in block.
    int position;
    // Phi: one per predecessor.
    std::vector<Value*> operands;
    Value* replacement = nullptr;
};

Value* resolve(Value* value);

struct Operation {
    int pc;
    bool computed;
    // computed: the registers it reads, then those it writes
    std::vector<Value*> uses;
    std::vector<Value*> results;
    // otherwise: register values written back to the frame before it
    std::vector<std::pair<int, Value*>> stores;
};

struct Block {
    int id;
    int start;
    int end;
    // Registers are in the frame at the start: the function's entry, or
    // where it resumes after a call.
    bool entry = false;
    std::vector<Block*> preds;
    std::vector<Block*> succs;
    std::vector<Value*> phis;
    std::vector<Value*> loads;
    std::vector<Operation> operations;

    // construction
    bool sealed = false;
    bool filled = false;
    int reloaded = -2;
    std::map<int, Value*> current;
    std::vector<Value*> incomplete;
    std::bitset<MAX_FSTACK + 1> dirty;
};

struct Function {
    const Proto* proto;
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<std::unique_ptr<Value>> values;
    // the block starting at each pc, or null
    std::vector<Block*> at;
    // resume pcs and the blocks that start them
    std::vector<std::pair<int, Block*>> resumes;
};

std::unique_ptr<Function> lift(const Proto* proto);

// Whether luamura computes the instruction at 'pc' on SSA values.
bool computed(const Proto* proto, int pc);

// Whether 'value' is known to equal the frame's copy of register 'reg'.
bool in_frame(Value* value, int reg);

void print(const Function& function, std::ostream& out);

} // namespace ssa

#endif // SSA_H
