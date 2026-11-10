#include "lm_hot_paths.h"

#include <vector>

namespace luamura {

namespace {

class AddIntegerHotPaths {
public:
    explicit AddIntegerHotPaths(LmIR& ir) : ir(ir) {}

    void run() {
        // only the blocks from bytecode; the ones added here are hot
        // paths already
        size_t count = ir.blocks.size();
        for (size_t id = 1; id < count; ++id) {
            Block& block = *ir.blocks[id];
            OpCode op =
                GET_OPCODE(ir.nodes[block.instructions.back()].bytecode);
            if (op == OP_ADD || op == OP_ADDI)
                addHotPath(block);
        }
    }

private:
    // The value of slot before the add, as its node reads it.
    ValueID valueBefore(uint32_t add, Slot slot) const {
        for (const Operand& op : ir.nodes[add].inputs)
            if (op.slot == slot)
                return op.value;
        return no_value;
    }

    // Splits the add from block:
    //   block: ..., guard -> hot | slow
    //   hot:   the add, on integers -> after the metamethod call
    //   slow:  the add, as before
    void addHotPath(Block& block) {
        uint32_t add = block.instructions.back();
        PC pc = ir.nodes[add].pc;
        Instruction i = ir.nodes[add].bytecode;
        Slot a = GETARG_A(i);
        std::vector<Operand> operands = ir.nodes[add].inputs;

        Block& slow = ir.splitBlock(block, block.instructions.size() - 1);
        // the add skips the metamethod call when it succeeds
        Block& after = *slow.successors.front();
        Block& hot = ir.addBlock();

        ir.addNode(block, Node::Kind::Guard, pc).inputs = operands;
        block.terminator = Block::Terminator::Guard;
        ir.addEdge(block, hot);
        ir.addEdge(block, slow);

        for (const Operand& op : operands)
            hot.types[op.value] = LUA_VNUMINT;
        Node& copy = ir.addNode(hot, Node::Kind::Bytecode, pc);
        copy.bytecode = i;
        copy.inputs = operands;
        ir.addOutput(copy, a);
        ValueID result = copy.outputs.front().value;
        ir.addEdge(hot, after);

        for (uint32_t id : after.phis) {
            Node& phi = ir.nodes[id];
            Slot slot = phi.outputs.front().slot;
            ValueID value = slot == a ? result : valueBefore(add, slot);
            if (value == no_value)
                value = incomingFrom(phi, slow);
            phi.incoming.push_back({&hot, value});
        }
    }

    // A slot the add neither reads nor writes keeps its value.
    ValueID incomingFrom(const Node& phi, const Block& block) const {
        for (const PhiInput& in : phi.incoming)
            if (in.predecessor == &block)
                return in.value;
        return no_value;
    }

    LmIR& ir;
};

} // namespace

void addIntegerHotPaths(LmIR& ir) { AddIntegerHotPaths(ir).run(); }

} // namespace luamura
