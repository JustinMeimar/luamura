#include "lmssa.h"

#include <algorithm>
#include <set>

extern "C" {
#include "lopnames.h"
}

namespace ssa {

namespace {

// The registers a computed instruction reads and writes. Instructions
// that may leave a register unchanged read it, so that they can define
// it either way.
void operands(Instruction i, std::vector<int>& uses,
              std::vector<int>& defs) {
    int a = GETARG_A(i);
    switch (GET_OPCODE(i)) {
    case OP_MOVE:
    case OP_NOT:
        uses = {GETARG_B(i)};
        defs = {a};
        return;
    case OP_LOADI:
    case OP_LOADF:
    case OP_LOADK:
    case OP_LOADFALSE:
    case OP_LFALSESKIP:
    case OP_LOADTRUE:
        defs = {a};
        return;
    case OP_LOADNIL:
        for (int r = a; r <= a + GETARG_B(i); r++)
            defs.push_back(r);
        return;
    case OP_TEST:
    case OP_EQI:
        uses = {a};
        return;
    case OP_TESTSET:
    case OP_ADDI:
    case OP_ADDK:
    case OP_SUBK:
    case OP_MULK:
        uses = {GETARG_B(i), a};
        defs = {a};
        return;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
        uses = {GETARG_B(i), GETARG_C(i), a};
        defs = {a};
        return;
    case OP_FORLOOP:
        uses = {a, a + 1, a + 2};
        defs = {a, a + 2};
        return;
    default:
        return;
    }
}

// How far an instruction may move pc past itself, the most common first.
std::vector<int> moves(const Proto* p, int pc) {
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

bool resumes_after(OpCode op) { return op == OP_CALL || op == OP_TFORCALL; }

class Builder {
public:
    explicit Builder(Function& function) : function(function) {}

    void build();

private:
    Value* value(Value::Kind kind, int reg, Block* block, int position) {
        auto v = std::make_unique<Value>();
        v->kind = kind;
        v->id = static_cast<int>(function.values.size());
        v->reg = reg;
        v->block = block;
        v->position = position;
        function.values.push_back(std::move(v));
        return function.values.back().get();
    }

    Block* block(int start, int end) {
        auto b = std::make_unique<Block>();
        b->id = static_cast<int>(function.blocks.size());
        b->start = start;
        b->end = end;
        function.blocks.push_back(std::move(b));
        return function.blocks.back().get();
    }

    static void edge(Block* from, Block* to) {
        if (std::find(from->succs.begin(), from->succs.end(), to) !=
            from->succs.end())
            return;
        from->succs.push_back(to);
        to->preds.push_back(from);
    }

    std::vector<int> resume_pcs() const;
    void form_blocks();
    void prune();
    void find_resumes();
    void find_dirty();
    void fill(Block* b);
    void seal(Block* b);
    void remove_trivial_phis();

    Value* read(Block* b, int reg);
    Value* read_recursive(Block* b, int reg);
    Value* add_operands(Value* phi);
    Value* try_remove_trivial(Value* phi);

    Function& function;
};

void Builder::form_blocks() {
    const Proto* p = function.proto;
    int n = p->sizecode;
    std::vector<bool> leader(n, false);
    leader[0] = true;
    for (int pc = 0; pc < n; pc++) {
        std::vector<int> next = successors(p, pc);
        bool falls = next.size() == 1 && next[0] == pc + 1;
        if (!falls || resumes_after(GET_OPCODE(p->code[pc]))) {
            for (int s : next)
                leader[s] = true;
            if (pc + 1 < n)
                leader[pc + 1] = true;
        }
    }
    function.at.assign(n, nullptr);
    for (int pc = 0; pc < n;) {
        int end = pc + 1;
        while (end < n && !leader[end])
            end++;
        function.at[pc] = block(pc, end);
        pc = end;
    }
    for (auto& b : function.blocks)
        for (int s : successors(p, b->end - 1))
            edge(b.get(), function.at[s]);
}

// The function resumes at its start and after calls.
std::vector<int> Builder::resume_pcs() const {
    const Proto* p = function.proto;
    std::vector<int> pcs = {0};
    for (int pc = 0; pc + 1 < p->sizecode; pc++)
        if (resumes_after(GET_OPCODE(p->code[pc])))
            pcs.push_back(pc + 1);
    return pcs;
}

// Removes the edges of blocks that never run, such as the jumps after
// test instructions, which branch to the jump's target themselves.
void Builder::prune() {
    std::vector<bool> reached(function.blocks.size(), false);
    std::vector<Block*> work;
    for (int pc : resume_pcs())
        work.push_back(function.at[pc]);
    while (!work.empty()) {
        Block* b = work.back();
        work.pop_back();
        if (reached[b->id])
            continue;
        reached[b->id] = true;
        work.insert(work.end(), b->succs.begin(), b->succs.end());
    }
    for (auto& b : function.blocks) {
        if (reached[b->id])
            continue;
        for (Block* s : b->succs)
            s->preds.erase(
                std::find(s->preds.begin(), s->preds.end(), b.get()));
        b->succs.clear();
    }
}

// A block can start with registers in the frame if every predecessor
// ends with an instruction that leaves them there; otherwise resuming
// enters a block of its own.
void Builder::find_resumes() {
    const Proto* p = function.proto;
    for (int pc : resume_pcs()) {
        Block* target = function.at[pc];
        bool in_frame = std::all_of(
            target->preds.begin(), target->preds.end(),
            [&](Block* pred) { return !computed(p, pred->end - 1); });
        if (in_frame) {
            target->entry = true;
            function.resumes.push_back({pc, target});
            continue;
        }
        Block* resume = block(pc, pc);
        resume->entry = true;
        edge(resume, target);
        function.resumes.push_back({pc, resume});
    }
    for (auto& b : function.blocks)
        if (b->preds.empty())
            b->entry = true;
}

// The registers that may differ from the frame at the start of each
// block.
void Builder::find_dirty() {
    const Proto* p = function.proto;
    auto out = [&](Block* b) {
        auto dirty = b->entry ? decltype(b->dirty)() : b->dirty;
        for (int pc = b->start; pc < b->end; pc++) {
            if (!computed(p, pc)) {
                dirty.reset();
                continue;
            }
            std::vector<int> uses, defs;
            operands(p->code[pc], uses, defs);
            for (int r : defs)
                dirty.set(r);
        }
        return dirty;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& b : function.blocks) {
            if (b->entry)
                continue;
            auto dirty = b->dirty;
            for (Block* pred : b->preds)
                dirty |= out(pred);
            if (dirty != b->dirty) {
                b->dirty = dirty;
                changed = true;
            }
        }
    }
}

Value* Builder::read(Block* b, int reg) {
    auto it = b->current.find(reg);
    if (it != b->current.end())
        return resolve(it->second);
    if (b->reloaded >= -1) {
        Value* load = value(Value::Kind::Load, reg, b, b->reloaded);
        b->loads.push_back(load);
        b->current[reg] = load;
        return load;
    }
    return read_recursive(b, reg);
}

Value* Builder::read_recursive(Block* b, int reg) {
    Value* v;
    if (!b->sealed) {
        v = value(Value::Kind::Phi, reg, b, -1);
        b->phis.push_back(v);
        b->incomplete.push_back(v);
    } else if (b->preds.size() == 1) {
        v = read(b->preds[0], reg);
    } else {
        v = value(Value::Kind::Phi, reg, b, -1);
        b->phis.push_back(v);
        b->current[reg] = v;
        v = add_operands(v);
    }
    b->current[reg] = v;
    return v;
}

Value* Builder::add_operands(Value* phi) {
    for (Block* pred : phi->block->preds)
        phi->operands.push_back(read(pred, phi->reg));
    return try_remove_trivial(phi);
}

Value* Builder::try_remove_trivial(Value* phi) {
    Value* same = nullptr;
    for (Value* op : phi->operands) {
        op = resolve(op);
        if (op == same || op == phi)
            continue;
        if (same != nullptr)
            return phi;
        same = op;
    }
    if (same == nullptr)
        return phi;
    phi->replacement = same;
    return same;
}

void Builder::seal(Block* b) {
    for (Value* phi : b->incomplete)
        add_operands(phi);
    b->incomplete.clear();
    b->sealed = true;
}

void Builder::fill(Block* b) {
    const Proto* p = function.proto;
    if (b->entry) {
        b->reloaded = -1;
        b->dirty.reset();
    }
    for (int pc = b->start; pc < b->end; pc++) {
        Operation ins{pc, computed(p, pc), {}, {}, {}};
        int index = static_cast<int>(b->operations.size());
        if (ins.computed) {
            std::vector<int> uses, defs;
            operands(p->code[pc], uses, defs);
            for (int r : uses)
                ins.uses.push_back(read(b, r));
            for (int r : defs) {
                Value* v = value(Value::Kind::Result, r, b, index);
                ins.results.push_back(v);
                b->current[r] = v;
                b->dirty.set(r);
            }
        } else {
            for (int r = 0; r <= MAX_FSTACK; r++)
                if (b->dirty.test(r))
                    ins.stores.push_back({r, read(b, r)});
            b->dirty.reset();
            b->current.clear();
            b->reloaded = index;
        }
        b->operations.push_back(std::move(ins));
    }
    b->filled = true;
}

void Builder::remove_trivial_phis() {
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& b : function.blocks)
            for (Value* phi : b->phis)
                if (phi->replacement == nullptr &&
                    try_remove_trivial(phi) != phi)
                    changed = true;
    }
    for (auto& b : function.blocks)
        b->phis.erase(std::remove_if(b->phis.begin(), b->phis.end(),
                                     [](Value* phi) {
                                         return phi->replacement != nullptr;
                                     }),
                      b->phis.end());
}

void Builder::build() {
    form_blocks();
    prune();
    find_resumes();
    find_dirty();
    auto seal_ready = [&] {
        for (auto& b : function.blocks)
            if (!b->sealed &&
                std::all_of(b->preds.begin(), b->preds.end(),
                            [](Block* pred) { return pred->filled; }))
                seal(b.get());
    };
    for (auto& b : function.blocks) {
        seal_ready();
        fill(b.get());
    }
    seal_ready();
    remove_trivial_phis();
}

bool in_frame(Value* value, int reg, std::set<Value*>& visiting) {
    value = resolve(value);
    if (value->kind == Value::Kind::Load)
        return value->reg == reg;
    if (value->kind != Value::Kind::Phi)
        return false;
    if (!visiting.insert(value).second)
        return true;
    return std::all_of(
        value->operands.begin(), value->operands.end(),
        [&](Value* op) { return in_frame(op, reg, visiting); });
}

} // namespace

Value* resolve(Value* value) {
    while (value->replacement != nullptr)
        value = value->replacement;
    return value;
}

bool computed(const Proto* proto, int pc) {
    switch (GET_OPCODE(proto->code[pc])) {
    case OP_MOVE:
    case OP_LOADI:
    case OP_LOADF:
    case OP_LOADK:
    case OP_LOADFALSE:
    case OP_LFALSESKIP:
    case OP_LOADTRUE:
    case OP_LOADNIL:
    case OP_NOT:
    case OP_JMP:
    case OP_TEST:
    case OP_TESTSET:
    case OP_EQI:
    case OP_ADDI:
    case OP_ADDK:
    case OP_SUBK:
    case OP_MULK:
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_FORLOOP:
        return true;
    default:
        return false;
    }
}

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
    for (int move : moves(proto, pc))
        next.push_back(pc + 1 + move);
    return next;
}

bool in_frame(Value* value, int reg) {
    std::set<Value*> visiting;
    return in_frame(value, reg, visiting);
}

std::unique_ptr<Function> lift(const Proto* proto) {
    auto function = std::make_unique<Function>();
    function->proto = proto;
    Builder(*function).build();
    return function;
}

void print(const Function& function, std::ostream& out) {
    const Proto* p = function.proto;
    auto name = [](Value* v) {
        return "v" + std::to_string(resolve(v)->id);
    };
    auto loads = [&](const Block& b, int position) {
        for (Value* v : b.loads)
            if (v->position == position)
                out << "    " << name(v) << " = load r" << v->reg << "\n";
    };
    out << "function " << getstr(p->source) << ":" << p->linedefined
        << "\n";
    for (auto& b : function.blocks) {
        out << "  block " << b->id << " [" << b->start << ", " << b->end
            << ")" << (b->entry ? " entry" : "") << " <-";
        for (Block* pred : b->preds)
            out << " " << pred->id;
        out << "\n";
        for (Value* phi : b->phis) {
            out << "    " << name(phi) << " = phi r" << phi->reg;
            for (Value* op : phi->operands)
                out << " " << name(op);
            out << "\n";
        }
        loads(*b, -1);
        for (size_t n = 0; n < b->operations.size(); n++) {
            const Operation& ins = b->operations[n];
            out << "    " << ins.pc << " "
                << opnames[GET_OPCODE(p->code[ins.pc])];
            for (Value* v : ins.results)
                out << " " << name(v) << ":r" << v->reg;
            if (!ins.uses.empty()) {
                out << " <-";
                for (Value* v : ins.uses)
                    out << " " << name(v);
            }
            for (auto& [reg, v] : ins.stores)
                if (!in_frame(v, reg))
                    out << " r" << reg << "=" << name(v);
            out << "\n";
            loads(*b, static_cast<int>(n));
        }
    }
}

} // namespace ssa
