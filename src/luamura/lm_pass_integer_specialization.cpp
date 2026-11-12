#include "lm_pass_integer_specialization.h"
#include <algorithm>
#include <vector>

namespace luamura {

namespace {

class IntegerSpecialization {
public:
    explicit IntegerSpecialization(LmIR& ir) : ir(ir) {}

    void run() {
        size_t count = ir.blocks.size();
        std::vector<bool> covered(count, false);
        for (size_t id = 1; id < count; ++id) {
            Block& block = *ir.blocks[id];
            if (covered[id] || block.instructions.empty())
                continue;
            OpCode op =
                GET_OPCODE(ir.nodes[block.instructions.back()].bytecode);
            if (op == OP_ADD || op == OP_ADDI)
                specialize(block, covered);
        }
        ir.updateSSA();
        for (auto& block : ir.blocks) {
            if (block->terminator != Block::Terminator::Guard)
                continue;
            const Node& guard = ir.nodes[block->instructions.back()];
            for (const Operand& input : guard.inputs)
                block->successors.front()->types[input.value] = LUA_VNUMINT;
        }
    }

private:
    bool extendsChain(const Node& node,
                      const std::vector<bool>& integers) const {
        if (node.kind != Node::Kind::Bytecode)
            return false;
        OpCode op = GET_OPCODE(node.bytecode);
        if (op != OP_ADD && op != OP_ADDI && op != OP_SUB && op != OP_MUL)
            return false;
        return std::all_of(
            node.inputs.begin(), node.inputs.end(),
            [&](const Operand& input) { return integers[input.slot]; });
    }

    void specialize(Block& block, std::vector<bool>& covered) {
        Node first = ir.nodes[block.instructions.back()];
        Block& slow = ir.splitBlock(block, block.instructions.size() - 1);
        Block& hot = ir.addBlock();
        ir.addNode(block, Node::Kind::Guard, first.pc).inputs =
            first.inputs;
        block.terminator = Block::Terminator::Guard;
        ir.addEdge(block, hot);
        ir.addEdge(block, slow);

        std::vector<bool> integers(ir.proto.maxstacksize, false);
        for (const Operand& input : first.inputs)
            integers[input.slot] = true;
        Block* source = &slow;
        Block* after = nullptr;
        // Bound duplication while measuring the benefit of longer paths.
        constexpr uint32_t maxOperations = 8;
        for (uint32_t count = 0; count < maxOperations; ++count) {
            Node original = ir.nodes[source->instructions.back()];
            Node& copy = ir.addNode(hot, Node::Kind::Bytecode, original.pc);
            copy.bytecode = original.bytecode;
            copy.inputs = original.inputs;
            for (const Operand& output : original.outputs) {
                ir.addOutput(copy, output.slot);
                integers[output.slot] = true;
            }
            if (source->id < covered.size())
                covered[source->id] = true;
            after = source->successors.front();
            if (after->instructions.size() != 1 ||
                after->terminator != Block::Terminator::Branch)
                break;
            const Node& next = ir.nodes[after->instructions.front()];
            if (next.pc != original.pc + 2 || !extendsChain(next, integers))
                break;
            source = after;
        }
        ir.addEdge(hot, *after);
    }

    LmIR& ir;
};

} // namespace

void specializeIntegerArithmetic(LmIR& ir) {
    IntegerSpecialization(ir).run();
}

} // namespace luamura
