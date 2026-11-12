#include "lm_pass_type_specialization.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <algorithm>

namespace luamura {

void TypeSpecialization::run(LmIR& ir) {
    // only the blocks from bytecode, as specializing adds more
    auto blocks =
        llvm::to_vector(llvm::make_pointer_range(llvm::drop_begin(ir)));
    Covered covered;
    for (Block* block : blocks) {
        if (covered.contains(block) || block->empty())
            continue;
        OpCode op = GET_OPCODE(block->back().bytecode);
        if (op == OP_ADD || op == OP_ADDI)
            specialize(ir, *block, covered);
    }
    ir.updateSSA();
    for (Block& block : ir) {
        if (block.terminator != Block::Terminator::Guard)
            continue;
        for (const Operand& input : block.back().inputs)
            block.successors.front()->types[input.value] = LUA_VNUMINT;
    }
}

bool TypeSpecialization::extendsChain(
    const Node& node, const std::vector<bool>& integers) const {
    if (node.kind != Node::Kind::Bytecode)
        return false;
    OpCode op = GET_OPCODE(node.bytecode);
    if (op != OP_ADD && op != OP_ADDI && op != OP_SUB && op != OP_MUL)
        return false;
    return std::all_of(
        node.inputs.begin(), node.inputs.end(),
        [&](const Operand& input) { return integers[input.slot]; });
}

void TypeSpecialization::specialize(LmIR& ir, Block& block,
                                    Covered& covered) {
    Node first = block.back();
    Block& slow = ir.splitBlock(block, std::prev(block.end()));
    Block& hot = ir.addBlock();
    ir.addNode(block, Node::Kind::Guard, first.pc).inputs = first.inputs;
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
        Node original = source->back();
        Node& copy = ir.addNode(hot, Node::Kind::Bytecode, original.pc);
        copy.bytecode = original.bytecode;
        copy.inputs = original.inputs;
        for (const Operand& output : original.outputs) {
            ir.addOutput(copy, output.slot);
            integers[output.slot] = true;
        }
        covered.insert(source);
        after = source->successors.front();
        if (after->size() != 1 ||
            after->terminator != Block::Terminator::Branch)
            break;
        const Node& next = after->front();
        if (next.pc != original.pc + 2 || !extendsChain(next, integers))
            break;
        source = after;
    }
    ir.addEdge(hot, *after);
}

} // namespace luamura
