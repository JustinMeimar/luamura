#include "lm_ir.h"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/GenericDomTreeConstruction.h>
#include <llvm/Support/GenericIteratedDominanceFrontier.h>
#include <algorithm>

extern "C" {
#include "lopnames.h"
}

namespace luamura {

static bool isCall(const Proto& proto, PC pc) {
    OpCode op = GET_OPCODE(proto.code[pc]);
    return op == OP_CALL || op == OP_TFORCALL;
}

// The pcs the instruction at 'pc' may continue at, the most common
// first.
static std::vector<PC> successorPCs(const Proto& proto, PC pc) {
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
    LUAMURA_SWITCH(i) {
    LUAMURA_CASE(JMP):
        return {next + GETARG_sJ(i)};
    LUAMURA_CASE(RETURN):
    LUAMURA_CASE(RETURN0):
    LUAMURA_CASE(RETURN1):
    LUAMURA_CASE(TAILCALL):
    LUAMURA_CASE(EXTRAARG):
        return {};
    LUAMURA_CASE(FORLOOP):
    LUAMURA_CASE(TFORLOOP):
        return {next - GETARG_Bx(i), next};
    LUAMURA_CASE(FORPREP):
        return {next, next + GETARG_Bx(i) + 1};
    LUAMURA_CASE(TFORPREP):
        return {next + GETARG_Bx(i)};
    // these skip the instruction after them
    LUAMURA_CASE(LOADKX):
    LUAMURA_CASE(NEWTABLE):
    LUAMURA_CASE(LFALSESKIP):
        return {next + 1};
    LUAMURA_CASE(SETLIST):
        return {TESTARG_k(i) ? next + 1 : next};
    default:
        // arithmetic skips the metamethod call after it, unless its
        // operands need one
        if (op >= OP_ADDI && op <= OP_SHR)
            return {next + 1, next};
        return {next};
    }
}

LmIR::LmIR(const Proto& proto) : proto(proto) {
    buildNodes(buildBlocks());
    updateSSA();
}

// The pc each block ends at, indexed by its start.
std::vector<PC> LmIR::buildBlocks() {
    std::vector<bool> leaders(proto.sizecode, false);
    leaders[0] = true;
    for (PC pc : llvm::seq<PC>(0, proto.sizecode)) {
        auto next = successorPCs(proto, pc);
        if (next.size() == 1 && next[0] == pc + 1 && !isCall(proto, pc))
            continue;
        for (PC target : next)
            leaders[target] = true;
        if (pc + 1 < proto.sizecode)
            leaders[pc + 1] = true;
    }
    std::vector<PC> ends(proto.sizecode);
    PC end = proto.sizecode;
    for (PC pc : llvm::reverse(llvm::seq<PC>(0, proto.sizecode))) {
        ends[pc] = end;
        if (leaders[pc])
            end = pc;
    }
    std::vector<bool> reached(proto.sizecode, false);
    std::vector<PC> work = {0};
    while (!work.empty()) {
        PC pc = work.back();
        work.pop_back();
        if (reached[pc])
            continue;
        reached[pc] = true;
        for (PC target : successorPCs(proto, ends[pc] - 1))
            work.push_back(target);
    }
    Block& entry = addBlock();
    entry.terminator = Block::Terminator::Resume;
    std::vector<Block*> at(proto.sizecode, nullptr);
    for (PC pc : llvm::seq<PC>(0, proto.sizecode)) {
        if (!reached[pc])
            continue;
        at[pc] = &addBlock();
        at[pc]->start = pc;
        at[pc]->terminator = terminatorAt(ends[pc] - 1);
    }
    addEdge(entry, *at[0]);
    for (auto& block : llvm::drop_begin(blocks)) {
        PC last = ends[block->start] - 1;
        for (PC target : successorPCs(proto, last))
            addEdge(*block, *at[target]);
        if (isCall(proto, last))
            addEdge(entry, *block->successors.front());
    }
    return ends;
}

Block& LmIR::addBlock() {
    auto block = std::make_unique<Block>();
    block->parent = this;
    block->id = blocks.size();
    blocks.push_back(std::move(block));
    return *blocks.back();
}

void LmIR::addEdge(Block& from, Block& to) {
    if (llvm::is_contained(from.successors, &to))
        return;
    from.successors.push_back(&to);
    to.predecessors.push_back(&from);
}

Block& LmIR::splitBlock(Block& block, Block::iterator at) {
    Block& rest = addBlock();
    rest.instruction_ids.assign(at.wrapped(), block.instruction_ids.cend());
    block.instruction_ids.erase(at.wrapped(), block.instruction_ids.cend());
    rest.terminator = block.terminator;
    rest.successors = std::move(block.successors);
    block.successors.clear();
    for (Block* next : rest.successors) {
        llvm::replace(next->predecessors, &block, &rest);
        for (Node& phi : next->phis())
            for (PhiInput& in : phi.incoming)
                if (in.predecessor == &block)
                    in.predecessor = &rest;
    }
    return rest;
}

Block::Terminator LmIR::terminatorAt(PC last) const {
    LUAMURA_SWITCH(proto.code[last]) {
    LUAMURA_CASE(CALL):
        return Block::Terminator::Call;
    LUAMURA_CASE(TAILCALL):
        return Block::Terminator::TailCall;
    LUAMURA_CASE(RETURN):
    LUAMURA_CASE(RETURN0):
    LUAMURA_CASE(RETURN1):
        return Block::Terminator::Return;
    default:
        return Block::Terminator::Branch;
    }
}

Node& LmIR::createNode(Node::Kind kind, PC pc) {
    nodes.push_back({static_cast<NodeID>(nodes.size()), kind});
    nodes.back().pc = pc;
    return nodes.back();
}

Node& LmIR::addNode(Block& block, Node::Kind kind, PC pc) {
    Node& node = createNode(kind, pc);
    (kind == Node::Kind::Phi ? block.phi_ids : block.instruction_ids)
        .push_back(node.id);
    return node;
}

Block::iterator LmIR::insertNode(Block& block, Block::iterator at,
                                 Node::Kind kind) {
    NodeID id = createNode(kind, -1).id;
    return {nodes, block.instruction_ids.insert(at.wrapped(), id)};
}

void LmIR::addOutput(Node& node, Slot slot) {
    node.outputs.push_back({slot, static_cast<ValueID>(values.size())});
    values.push_back({node.id, slot});
}

void LmIR::updateSSA() {
    DominatorTree dominators;
    dominators.recalculate(*this);
    insertPhis(dominators);
    renameValues(dominators);
}

void LmIR::buildNodes(const std::vector<PC>& ends) {
    // NOTE(Justin): Explain why entry is a frame read.
    Node& entry = addNode(getEntryBlock(), Node::Kind::FrameRead);
    for (Slot slot : llvm::seq<Slot>(0, proto.maxstacksize))
        addOutput(entry, slot);
    LmSlotModel slots(*this);
    for (auto& block : llvm::drop_begin(blocks))
        for (PC pc : llvm::seq(block->start, ends[block->start])) {
            Node& node = addNode(*block, Node::Kind::Bytecode, pc);
            node.bytecode = proto.code[pc];
            slots.model(node);
        }
}

void LmIR::insertPhis(DominatorTree& dominators) {
    for (Slot slot : llvm::seq<Slot>(0, proto.maxstacksize)) {
        llvm::SmallPtrSet<Block*, 32> definitions, live;
        std::vector<Block*> work;
        for (auto& block : blocks) {
            bool defined = false;
            for (const Node& node : *block) {
                if (!defined &&
                    llvm::any_of(node.inputs, [&](const Operand& op) {
                        return op.slot == slot;
                    })) {
                    if (live.insert(block.get()).second)
                        work.push_back(block.get());
                }
                if (llvm::any_of(node.outputs, [&](const Operand& op) {
                        return op.slot == slot;
                    })) {
                    defined = true;
                    definitions.insert(block.get());
                }
            }
        }
        while (!work.empty()) {
            Block* block = work.back();
            work.pop_back();
            for (Block* pred : block->predecessors)
                if (!definitions.contains(pred) && live.insert(pred).second)
                    work.push_back(pred);
        }
        llvm::IDFCalculatorBase<Block, false> idf(dominators);
        idf.setDefiningBlocks(definitions);
        idf.setLiveInBlocks(live);
        llvm::SmallVector<Block*, 16> joins;
        idf.calculate(joins);
        for (Block* block : joins)
            if (!hasPhi(*block, slot))
                addOutput(addNode(*block, Node::Kind::Phi), slot);
    }
}

bool LmIR::hasPhi(const Block& block, Slot slot) const {
    return llvm::any_of(block.phis(), [&](const Node& phi) {
        return phi.outputs.front().slot == slot;
    });
}

void LmIR::renameValues(DominatorTree& dominators) {
    for (auto& block : blocks)
        for (Node& phi : block->phis())
            phi.incoming.clear();
    using State = std::vector<ValueID>;
    struct Visit {
        llvm::DomTreeNodeBase<Block>* tree;
        State state;
    };
    std::vector<Visit> work;
    work.push_back(
        {dominators.getRootNode(), State(proto.maxstacksize, no_value)});
    while (!work.empty()) {
        Visit visit = std::move(work.back());
        work.pop_back();
        Block* block = visit.tree->getBlock();
        auto define = [&](const Node& node) {
            for (const Operand& op : node.outputs)
                visit.state[op.slot] = op.value;
        };
        for (const Node& phi : block->phis())
            define(phi);
        for (Node& node : *block) {
            for (Operand& op : node.inputs)
                op.value = visit.state[op.slot];
            define(node);
        }
        for (Block* next : block->successors)
            for (Node& phi : next->phis())
                phi.incoming.push_back(
                    {block, visit.state[phi.outputs.front().slot]});
        for (auto* child : *visit.tree)
            work.push_back({child, visit.state});
    }
}

LmModule::LmModule(const Proto& main) { addFunction(main); }

void LmModule::addFunction(const Proto& proto) {
    functions.push_back(std::make_unique<LmIR>(proto));
    for (const Proto* nested : llvm::ArrayRef(proto.p, proto.sizep))
        addFunction(*nested);
}

void Block::printAsOperand(llvm::raw_ostream& out, bool) const {
    out << "b" << id;
}

void LmIR::printBlock(llvm::raw_ostream& out, const Block& block) const {
    out << "b" << block.id;
    if (block.start >= 0)
        out << " (pc " << block.start << ")";
    out << ":\n";
    for (auto [value, tag] : block.types)
        out << "  assumes %" << value << " has tag " << int32_t(tag)
            << "\n";
    auto printNode = [&](const Node& node) {
        out << "  n" << node.id << " ";
        for (const Operand& op : node.outputs)
            out << "%" << op.value << "[R" << op.slot << "] ";
        out << "= ";
        switch (node.kind) {
        case Node::Kind::Phi:
            out << "phi";
            for (const PhiInput& in : node.incoming)
                out << " [b" << in.predecessor->id << ": %" << in.value
                    << "]";
            break;
        case Node::Kind::FrameRead:
            out << "frame_read";
            break;
        case Node::Kind::Bytecode:
            out << opnames[GET_OPCODE(node.bytecode)] << " @" << node.pc;
            break;
        case Node::Kind::Guard:
            out << "guard";
            break;
        case Node::Kind::UpdateBase:
            out << "update_base";
            break;
        }
        for (const Operand& op : node.inputs)
            out << " %" << op.value << "[R" << op.slot << "]";
        if (any(node.effects & Node::Effects::MovesBase))
            out << " [moves base]";
        if (any(node.effects & Node::Effects::RunsLua))
            out << " [runs lua]";
        out << "\n";
    };
    for (const Node& phi : block.phis())
        printNode(phi);
    for (const Node& node : block)
        printNode(node);
}

void LmIR::print(llvm::raw_ostream& out) const {
    for (const auto& block : blocks) {
        printBlock(out, *block);
        out << "  ->";
        for (Block* next : block->successors)
            out << " b" << next->id;
        out << "\n";
    }
}

void LmIR::writeGraph(llvm::raw_ostream& out, const llvm::Twine& name) {
    llvm::WriteGraph(out, this, false, name);
}

LmSlotModel::LmSlotModel(LmIR& ir) : ir(ir), top(ir.proto.maxstacksize) {
    findCapturedSlots();
}

void LmSlotModel::findCapturedSlots() {
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

bool LmSlotModel::reads(Slot slot) const {
    return llvm::any_of(node->inputs,
                        [&](const Operand& op) { return op.slot == slot; });
}

bool LmSlotModel::writes(Slot slot) const {
    return llvm::any_of(node->outputs,
                        [&](const Operand& op) { return op.slot == slot; });
}

void LmSlotModel::read(Slot slot) {
    if (slot < top && !reads(slot))
        node->inputs.push_back({slot});
}

void LmSlotModel::write(Slot slot) {
    if (slot < top && !writes(slot))
        ir.addOutput(*node, slot);
}

void LmSlotModel::readRange(Slot from, Slot to) {
    for (Slot slot = from; slot < std::min(to, top); ++slot)
        read(slot);
}

void LmSlotModel::writeRange(Slot from, Slot to) {
    for (Slot slot = from; slot < std::min(to, top); ++slot)
        write(slot);
}

void LmSlotModel::movesBase() { node->effects |= Node::Effects::MovesBase; }

// Lua code can grow the stack, and can change a captured slot through
// its open upvalue. SSA sees that change as this node reading and
// writing each captured slot, so later uses get a new value rather
// than one defined before the node.
void LmSlotModel::runsLua() {
    node->effects |= Node::Effects::MovesBase | Node::Effects::RunsLua;
    for (Slot slot : captured) {
        read(slot);
        write(slot);
    }
}

// A count of zero means the values run to the top of the stack.
Slot LmSlotModel::upTo(uint32_t count, Slot end) const {
    return count == 0 ? top : end;
}

void LmSlotModel::model(Node& node) {
    this->node = &node;
    Instruction i = node.bytecode;
    OpCode op = GET_OPCODE(i);
    Slot a = GETARG_A(i);
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
        movesBase();
        break;
    default:
        readRange(0, top);
        writeRange(0, top);
        runsLua();
        break;
    }
}

} // namespace luamura

namespace llvm {

template <> struct DOTGraphTraits<luamura::LmIR*> : DefaultDOTGraphTraits {
    using DefaultDOTGraphTraits::DefaultDOTGraphTraits;

    std::string getNodeLabel(const luamura::Block* block,
                             const luamura::LmIR* ir) {
        std::string label;
        llvm::raw_string_ostream out(label);
        ir->printBlock(out, *block);
        // left-justify each line, as dot centers them by default
        llvm::StringRef text = label;
        std::string justified;
        for (llvm::StringRef line : llvm::split(text.rtrim('\n'), '\n'))
            justified += (line + "\\l").str();
        return justified;
    }
};

} // namespace llvm
