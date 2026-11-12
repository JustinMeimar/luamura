#include "lm_pass_base_updates.h"

namespace luamura {

void BaseUpdates::run(LmIR& ir) {
    ir.insertNode(ir.getEntryBlock(), 0, Node::Kind::UpdateBase);
    for (auto& block : ir.blocks) {
        // A call continues at its successor, and a return or tail call
        // leaves the frame, so neither updates the base after itself.
        if (block->terminator == Block::Terminator::Call)
            ir.insertNode(*block->successors.front(), 0,
                          Node::Kind::UpdateBase);
        size_t last = block->instructions.size();
        for (size_t index = last; index > 0; --index) {
            const Node& node = ir.nodes[block->instructions[index - 1]];
            if (node.effects == Node::Effects::Frame)
                continue;
            if (index == last &&
                block->terminator != Block::Terminator::Branch)
                continue;
            ir.insertNode(*block, index, Node::Kind::UpdateBase);
        }
    }
}

} // namespace luamura
