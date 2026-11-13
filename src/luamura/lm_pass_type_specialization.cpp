#include "lm_pass_type_specialization.h"
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>

namespace luamura {

void TypeSpecialization::run(LmIR& ir) {
    TypeInference inference(ir);
    // only the blocks from bytecode, as specializing adds more
    auto blocks =
        llvm::to_vector(llvm::make_pointer_range(llvm::drop_begin(ir)));
    Covered covered;
    for (Block* block : blocks) {
        if (covered.contains(block) || block->empty())
            continue;
        OpCode op = GET_OPCODE(block->back().bytecode);
        if (op == OP_ADD || op == OP_ADDI)
            specialize(ir, inference, *block, covered);
    }
    ir.updateSSA();
    for (Block& block : ir) {
        if (block.terminator != Block::Terminator::Guard)
            continue;
        for (const Operand& input : block.back().inputs)
            block.successors.front()->types[input.value] = LUA_VNUMINT;
    }
}

void TypeSpecialization::specialize(LmIR& ir,
                                    const TypeInference& inference,
                                    Block& block, Covered& covered) {
    Node first = block.back();
    // the tags of the registers the guarded path has set so far
    std::vector<TypeTag> types(ir.proto.maxstacksize, no_type);
    auto hotResultType = [&](const Block& source, const Node& node) {
        return resultType(ir.proto, node.bytecode, [&](Slot slot) {
            return types[slot] != no_type
                       ? types[slot]
                       : inference.typeOf(source, node, slot);
        });
    };
    llvm::SmallVector<Operand, 2> guarded;
    for (const Operand& input : first.inputs) {
        if (inference.typeOf(input.value) != no_type)
            continue;
        guarded.push_back(input);
        types[input.slot] = LUA_VNUMINT;
    }
    if (hotResultType(block, first) == no_type)
        return;

    Block& slow = ir.splitBlock(block, std::prev(block.end()));
    Block& hot = ir.addBlock();
    ir.addNode(block, Node::Kind::Guard, first.pc)
        .inputs.assign(guarded.begin(), guarded.end());
    block.terminator = Block::Terminator::Guard;
    ir.addEdge(block, hot);
    ir.addEdge(block, slow);

    Block* source = &slow;
    Block* after = nullptr;
    // Bound duplication while measuring the benefit of longer paths.
    constexpr uint32_t maxOperations = 8;
    for (uint32_t count = 0; count < maxOperations; ++count) {
        Node original = source->back();
        TypeTag result = hotResultType(*source, original);
        Node& copy = ir.addNode(hot, Node::Kind::Bytecode, original.pc);
        copy.bytecode = original.bytecode;
        copy.inputs = original.inputs;
        for (const Operand& output : original.outputs) {
            ir.addOutput(copy, output.slot);
            types[output.slot] = result;
        }
        covered.insert(source);
        after = source->successors.front();
        if (after->size() != 1 ||
            after->terminator != Block::Terminator::Branch)
            break;
        const Node& next = after->front();
        if (next.kind != Node::Kind::Bytecode ||
            !isArithmetic(GET_OPCODE(next.bytecode)) ||
            next.pc != original.pc + 2 ||
            hotResultType(*after, next) == no_type)
            break;
        source = after;
    }
    ir.addEdge(hot, *after);
}

} // namespace luamura
