#include "lm_pass_base_updates.h"

namespace luamura {

void BaseUpdates::run(LmIR& ir) {
    Block& entry = ir.getEntryBlock();
    ir.insertNode(entry, entry.begin(), Node::Kind::UpdateBase);
    for (Block& block : ir) {
        // A call continues at its successor, and a return or tail call
        // leaves the frame, so neither updates the base after itself.
        if (block.terminator == Block::Terminator::Call) {
            Block& next = *block.successors.front();
            ir.insertNode(next, next.begin(), Node::Kind::UpdateBase);
        }
        for (auto it = block.begin(); it != block.end(); ++it) {
            if (!any(it->effects & Node::Effects::MovesBase))
                continue;
            auto after = std::next(it);
            if (after == block.end() &&
                block.terminator != Block::Terminator::Branch)
                continue;
            it = ir.insertNode(block, after, Node::Kind::UpdateBase);
        }
    }
}

} // namespace luamura
