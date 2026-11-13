#ifndef LM_PASS_TYPE_SPECIALIZATION_H
#define LM_PASS_TYPE_SPECIALIZATION_H

#include "lm_pass.h"
#include "lm_type_inference.h"
#include <llvm/ADT/SmallVector.h>
#include <optional>
#include <utility>
#include <vector>

namespace luamura {

// A region runs from the function's entry, or a node that runs Lua, to
// the next node that runs Lua. At its start a guard tests the types
// predicted for the values it reads. The specialized channel copies the
// region's nodes along the edges those types leave open; the generic
// channel is the original nodes, for when the guard fails.
class TypeSpecialization : public LmIRPass {
public:
    void run(LmIR& ir) override;

private:
    struct Channel {
        NodeID start;
        // the registers the guard tests, read before the channel sets
        // them, with the tag each is predicted to have
        llvm::SmallVector<std::pair<Slot, TypeTag>, 4> guards;
        std::vector<NodeID> nodes;
        // the node that runs Lua, which the channel rejoins the generic
        // channel before
        std::optional<NodeID> stop;
        // otherwise, how the channel's last node leaves it
        Block::Terminator terminator = Block::Terminator::Branch;
        std::vector<Block*> successors;
    };

    std::vector<NodeID> regionStarts(LmIR& ir) const;
    Channel walk(LmIR& ir, const TypeInference& inference,
                 NodeID start) const;
    Block& specialize(LmIR& ir, const Channel& channel) const;
};

} // namespace luamura

#endif // LM_PASS_TYPE_SPECIALIZATION_H
