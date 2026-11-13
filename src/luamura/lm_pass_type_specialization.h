#ifndef LM_PASS_TYPE_SPECIALIZATION_H
#define LM_PASS_TYPE_SPECIALIZATION_H

#include "lm_pass.h"
#include "lm_type_inference.h"
#include <llvm/ADT/SmallVector.h>
#include <utility>
#include <vector>

namespace luamura {

// A region runs from the function's entry, or a node that runs Lua, to
// the nodes that run Lua after it. At its start a guard tests the types
// predicted for the values it reads. The specialized channel copies the
// region's blocks along the edges those types leave open; the generic
// channel is the original blocks, for when the guard fails.
class TypeSpecialization : public LmIRPass {
public:
    void run(LmIR& ir) override;

private:
    struct Channel {
        Block* start;
        // the values from before the region that the guard tests, with
        // the tag each is predicted to have
        llvm::SmallVector<std::pair<ValueID, TypeTag>, 4> guards;
        // the region's blocks the guarded types reach, from its start
        std::vector<Block*> blocks;
        // the edges those types leave open from them
        std::vector<std::pair<Block*, Block*>> edges;
    };

    void splitAtRegionEnds(LmIR& ir) const;
    std::vector<Block*> regionStarts(LmIR& ir) const;
    Channel walk(LmIR& ir, const TypeInference& inference,
                 Block& start) const;
    std::vector<Block*> specialize(LmIR& ir, const Channel& channel) const;
};

} // namespace luamura

#endif // LM_PASS_TYPE_SPECIALIZATION_H
