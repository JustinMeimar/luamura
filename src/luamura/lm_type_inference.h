#ifndef LM_TYPE_INFERENCE_H
#define LM_TYPE_INFERENCE_H

#include "lm_ir.h"
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <vector>

namespace luamura {

// The tag of an instruction's result, given the tag of each register it
// reads, or no_type if those do not determine it. Arithmetic has a
// result tag exactly when its operands rule out the metamethod.
TypeTag resultType(const Proto& proto, Instruction i,
                   llvm::function_ref<TypeTag(Slot)> typeOf);

// The tag each value has wherever it is used, where its definition
// determines one.
class TypeInference {
public:
    explicit TypeInference(const LmIR& ir);

    TypeTag typeOf(ValueID value) const { return types[value]; }
    // the tag of the value the node reads from the register, which the
    // node's block may assume
    TypeTag typeOf(const Block& block, const Node& node, Slot slot) const;

private:
    void infer(const Block& block, const Node& node);
    bool skipsMetamethod(const Block& from, const Block& to) const;
    TypeTag joinIncoming(const Block& block, const Node& phi) const;

    const Proto& proto;
    std::vector<TypeTag> types;
    // edges from reached blocks that the known types do not rule out
    llvm::DenseSet<std::pair<const Block*, const Block*>> taken;
    llvm::SmallPtrSet<const Block*, 32> visited;
};

} // namespace luamura

#endif // LM_TYPE_INFERENCE_H
