#include "lm_lower.h"

namespace luamura {

void LmIRLowerer::lower(const LmModule& module) {
    for (const auto& ir : module.functions)
        lower(*ir);
}

void LmIRLowerer::lower(const LmIR& ir) {
    for (const auto& block : ir.blocks)
        lower(*block);
}

void LmIRLowerer::lower(const Block& block) {
    for (uint32_t id : block.phis)
        lower(block.parent->nodes[id]);
    for (uint32_t id : block.instructions)
        lower(block.parent->nodes[id]);
    lowerTerminator(block);
}

void LmIRLowerer::lower(const Node& node) {
    switch (node.kind) {
    case Node::Kind::FrameRead:
        return lowerFrameRead(node);
    case Node::Kind::Phi:
        return lowerPhi(node);
    case Node::Kind::Bytecode:
        return lowerBytecode(node);
    case Node::Kind::Guard:
        return lowerGuard(node);
    }
}

} // namespace luamura
