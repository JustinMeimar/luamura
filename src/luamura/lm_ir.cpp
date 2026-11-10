#include "lm_ir.h"

#include <llvm/ADT/SmallPtrSet.h>
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

LmIR::LmIR(const Proto& proto) : proto(proto) {
    buildBlocks();
    buildNodes();
    DominatorTree dominators;
    dominators.recalculate(*this);
    insertPhis(dominators);
    renameValues(dominators);
}

void LmIR::buildBlocks() {
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
    auto add = [&](PC start, PC end) {
        auto block = std::make_unique<Block>();
        block->parent = this;
        block->id = blocks.size();
        block->start = start;
        block->end = end;
        blocks.push_back(std::move(block));
        return blocks.back().get();
    };
    Block* entry = add(-1, -1);
    std::vector<Block*> at(proto.sizecode, nullptr);
    for (PC pc = 0; pc < proto.sizecode; ++pc)
        if (reached[pc])
            at[pc] = add(pc, ends[pc]);
    auto connect = [](Block* from, Block* to) {
        if (llvm::is_contained(from->successors, to))
            return;
        from->successors.push_back(to);
        to->predecessors.push_back(from);
    };
    connect(entry, at[0]);
    for (auto& block : llvm::drop_begin(blocks)) {
        for (PC target : successorPCs(proto, block->last()))
            connect(block.get(), at[target]);
        if (isCall(proto, block->last()))
            connect(entry, block->successors.front());
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

void LmIR::addOutput(Node& node, unsigned slot) {
    node.outputs.push_back({slot, static_cast<ValueID>(values.size())});
    values.push_back({node.id, slot});
}

void LmIR::buildBytecodeNode(Block& block, PC pc) {
    Node& node = addNode(block, Node::Kind::Bytecode, pc);
    Instruction i = node.bytecode = proto.code[pc];
    unsigned a = GETARG_A(i);
    auto addInput = [&](unsigned slot) {
        if (llvm::none_of(node.inputs, [&](const Operand& op) {
                return op.slot == slot;
            }))
            node.inputs.push_back({slot});
    };
    switch (GET_OPCODE(i)) {
    case OP_MOVE:
    case OP_NOT:
        addInput(GETARG_B(i));
        addOutput(node, a);
        break;
    case OP_LOADI:
    case OP_LOADF:
    case OP_LOADK:
    case OP_LOADKX:
    case OP_LOADFALSE:
    case OP_LOADTRUE:
    case OP_LFALSESKIP:
        addOutput(node, a);
        break;
    case OP_LOADNIL:
        for (unsigned s = a; s <= a + GETARG_B(i); ++s)
            addOutput(node, s);
        break;
    case OP_JMP:
        break;
    case OP_TEST:
        addInput(a);
        break;
    case OP_TESTSET:
        addInput(a);
        addInput(GETARG_B(i));
        addOutput(node, a);
        break;
    default:
        // Runtime calls can change slots through open upvalues.
        node.effects = Node::Effects::Runtime;
        for (unsigned s = 0; s < proto.maxstacksize; ++s) {
            addInput(s);
            addOutput(node, s);
        }
        break;
    }
}

void LmIR::buildNodes() {
    Node& entry = addNode(getEntryBlock(), Node::Kind::FrameRead);
    for (unsigned slot = 0; slot < proto.maxstacksize; ++slot)
        addOutput(entry, slot);
    for (auto& block : llvm::drop_begin(blocks))
        for (PC pc = block->start; pc < block->end; ++pc)
            buildBytecodeNode(*block, pc);
}

void LmIR::insertPhis(DominatorTree& dominators) {
    for (unsigned slot = 0; slot < proto.maxstacksize; ++slot) {
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

void Block::printAsOperand(llvm::raw_ostream& out, bool) const {
    out << "b" << id;
}

void LmIR::print(llvm::raw_ostream& out) const {
    for (const auto& block : blocks) {
        out << "b" << block->id << " (pc " << block->start << "):\n";
        auto printNode = [&](uint32_t id) {
            const Node& node = nodes[id];
            out << "  n" << id << " ";
            for (const Operand& op : node.outputs)
                out << "%" << op.value << "[R" << op.slot << "] ";
            out << "= ";
            if (node.kind == Node::Kind::Phi) {
                out << "phi";
                for (const PhiInput& in : node.incoming)
                    out << " [b" << in.predecessor->id << ": %" << in.value
                        << "]";
            } else if (node.kind == Node::Kind::FrameRead) {
                out << "frame_read";
            } else {
                out << opnames[GET_OPCODE(node.bytecode)] << " @"
                    << node.pc;
                for (const Operand& op : node.inputs)
                    out << " %" << op.value << "[R" << op.slot << "]";
                if (node.effects == Node::Effects::Runtime)
                    out << " [runtime]";
            }
            out << "\n";
        };
        for (uint32_t id : block->phis)
            printNode(id);
        for (uint32_t id : block->instructions)
            printNode(id);
        out << "  ->";
        for (Block* next : block->successors)
            out << " b" << next->id;
        out << "\n";
    }
}

} // namespace luamura
