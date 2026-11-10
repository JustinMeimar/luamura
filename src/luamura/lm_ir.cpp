#include "lm_ir.h"
#include "lm_effects.h"

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
    DominatorTree dominators;
    dominators.recalculate(*this);
    insertPhis(dominators);
    renameValues(dominators);
}

// The pc each block ends at, indexed by its start.
std::vector<PC> LmIR::buildBlocks() {
    std::vector<bool> leaders(proto.sizecode, false);
    leaders[0] = true;
    for (PC pc = 0; pc < proto.sizecode; ++pc) {
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
    for (PC pc = proto.sizecode - 1; pc >= 0; --pc) {
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
    for (PC pc = 0; pc < proto.sizecode; ++pc) {
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

Block& LmIR::splitBlock(Block& block, size_t index) {
    Block& rest = addBlock();
    rest.instructions.assign(block.instructions.begin() + index,
                             block.instructions.end());
    block.instructions.resize(index);
    rest.terminator = block.terminator;
    rest.successors = std::move(block.successors);
    block.successors.clear();
    for (Block* next : rest.successors) {
        llvm::replace(next->predecessors, &block, &rest);
        for (uint32_t id : next->phis)
            for (PhiInput& in : nodes[id].incoming)
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

Node& LmIR::addNode(Block& block, Node::Kind kind, PC pc) {
    uint32_t id = nodes.size();
    nodes.push_back({id, kind});
    Node& node = nodes.back();
    node.pc = pc;
    (kind == Node::Kind::Phi ? block.phis : block.instructions)
        .push_back(id);
    return node;
}

void LmIR::addOutput(Node& node, Slot slot) {
    node.outputs.push_back({slot, static_cast<ValueID>(values.size())});
    values.push_back({node.id, slot});
}

void LmIR::buildNodes(const std::vector<PC>& ends) {
    Node& entry = addNode(getEntryBlock(), Node::Kind::FrameRead);
    for (Slot slot = 0; slot < proto.maxstacksize; ++slot)
        addOutput(entry, slot);
    EffectModel effects(*this);
    for (auto& block : llvm::drop_begin(blocks))
        for (PC pc = block->start; pc < ends[block->start]; ++pc) {
            Node& node = addNode(*block, Node::Kind::Bytecode, pc);
            node.bytecode = proto.code[pc];
            effects.model(node);
        }
}

void LmIR::insertPhis(DominatorTree& dominators) {
    for (Slot slot = 0; slot < proto.maxstacksize; ++slot) {
        llvm::SmallPtrSet<Block*, 32> definitions, live;
        std::vector<Block*> work;
        for (auto& block : blocks) {
            bool defined = false;
            for (uint32_t id : block->instructions) {
                const Node& node = nodes[id];
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
            addOutput(addNode(*block, Node::Kind::Phi), slot);
    }
}

void LmIR::renameValues(DominatorTree& dominators) {
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
        for (uint32_t id : block->phis)
            define(nodes[id]);
        for (uint32_t id : block->instructions) {
            Node& node = nodes[id];
            for (Operand& op : node.inputs)
                op.value = visit.state[op.slot];
            define(node);
        }
        for (Block* next : block->successors)
            for (uint32_t id : next->phis) {
                Node& phi = nodes[id];
                phi.incoming.push_back(
                    {block, visit.state[phi.outputs.front().slot]});
            }
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
    auto printNode = [&](uint32_t id) {
        const Node& node = nodes[id];
        out << "  n" << id << " ";
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
        }
        for (const Operand& op : node.inputs)
            out << " %" << op.value << "[R" << op.slot << "]";
        if (node.effects == Node::Effects::Runtime)
            out << " [runtime]";
        out << "\n";
    };
    for (uint32_t id : block.phis)
        printNode(id);
    for (uint32_t id : block.instructions)
        printNode(id);
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
